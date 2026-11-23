#include "ssh/SftpWorker.h"

#include <libssh2_sftp.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <algorithm>

namespace termdock {

struct SftpHandle {
    LIBSSH2_SESSION *session = nullptr;
    LIBSSH2_SFTP *sftp = nullptr;
};

namespace {

// 传输进度节流：每 128KB 或每 200ms 报一次，同时给出平均速度（字节/秒）
struct ProgressMeter {
    QElapsedTimer timer;
    qint64 lastBytes = 0;
    qint64 lastMs = 0;

    void start()
    {
        timer.start();
        lastBytes = 0;
        lastMs = 0;
    }

    bool shouldReport(qint64 done) const
    {
        return done - lastBytes >= 128 * 1024 || timer.elapsed() - lastMs >= 200;
    }

    void note(qint64 done)
    {
        lastBytes = done;
        lastMs = timer.elapsed();
    }

    double speed(qint64 done) const
    {
        const double seconds = timer.elapsed() / 1000.0;
        return seconds > 0.001 ? double(done) / seconds : 0.0;
    }
};

} // namespace

SftpWorker::SftpWorker(const SshParams &params, QObject *parent)
    : QThread(parent)
    , m_params(params)
{
    qRegisterMetaType<SftpEntryList>("termdock::SftpEntryList");
}

SftpWorker::~SftpWorker()
{
    stop();
    wait(5000);
}

quint64 SftpWorker::enqueue(Op op, const QString &path, const QString &path2)
{
    QMutexLocker locker(&m_mutex);
    Request request;
    request.id = m_nextId++;
    request.op = op;
    request.path = path;
    request.path2 = path2;
    m_requests.enqueue(request);
    return request.id;
}

void SftpWorker::stop()
{
    m_stopRequested.storeRelease(1);
}

QString SftpWorker::sftpError(SftpHandle *sftp) const
{
    const unsigned long code = sftp && sftp->sftp ? libssh2_sftp_last_error(sftp->sftp) : 0;
    return QStringLiteral("SFTP 错误码 %1（%2）")
        .arg(code)
        .arg(sftp ? SshConnection::lastError(sftp->session) : QStringLiteral("无会话"));
}

void SftpWorker::run()
{
    SshConnection conn;
    QString error;
    if (!conn.connectAndAuth(m_params, 15000, &error)) {
        emit failed(error);
        return;
    }
    libssh2_session_set_blocking(conn.session(), 1);

    SftpHandle handle;
    handle.session = conn.session();
    handle.sftp = libssh2_sftp_init(conn.session());
    if (!handle.sftp) {
        emit failed(QStringLiteral("SFTP 子系统初始化失败：%1").arg(SshConnection::lastError(conn.session())));
        conn.close();
        return;
    }

    // 用 realpath(".") 拿到登录用户的 home 目录
    QByteArray pathBuffer(4096, Qt::Uninitialized);
    const int len = libssh2_sftp_realpath(handle.sftp, ".", pathBuffer.data(),
                                          size_t(pathBuffer.size() - 1));
    QString home = QStringLiteral("/");
    if (len > 0) {
        pathBuffer.resize(len);
        home = QString::fromUtf8(pathBuffer);
    }
    emit connected(home);

    while (!m_stopRequested.loadAcquire()) {
        Request request;
        bool haveRequest = false;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_requests.isEmpty()) {
                request = m_requests.dequeue();
                haveRequest = true;
            }
        }
        if (!haveRequest) {
            msleep(20);
            continue;
        }
        handleRequest(&handle, request);
    }

    libssh2_sftp_shutdown(handle.sftp);
    conn.close();
}

bool SftpWorker::handleRequest(SftpHandle *sftp, const Request &request)
{
    switch (request.op) {
    case Op::List:
        return listDir(sftp, request);
    case Op::Download:
        return downloadFile(sftp, request);
    case Op::Upload:
        return uploadFile(sftp, request);
    case Op::Mkdir:
        return makeDir(sftp, request);
    case Op::CreateFile:
        return createFile(sftp, request);
    case Op::Remove:
        return removePath(sftp, request);
    case Op::Rename:
        return renamePath(sftp, request);
    }
    return false;
}

SftpEntryList SftpWorker::listEntries(SftpHandle *sftp, const QString &path, QString *error)
{
    SftpEntryList entries;
    const QByteArray remotePath = path.toUtf8();
    LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_open_ex(sftp->sftp, remotePath.constData(),
                                                   size_t(remotePath.size()), 0, 0,
                                                   LIBSSH2_SFTP_OPENDIR);
    if (!dir) {
        if (error)
            *error = sftpError(sftp);
        return entries;
    }

    QByteArray nameBuffer(4096, Qt::Uninitialized);
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    while (true) {
        const int rc = libssh2_sftp_readdir_ex(dir, nameBuffer.data(), size_t(nameBuffer.size()),
                                               nullptr, 0, &attributes);
        if (rc <= 0)
            break;
        const QString name = QString::fromUtf8(nameBuffer.constData(), rc);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;

        SftpEntry entry;
        entry.name = name;
        entry.path = (path == QLatin1String("/")) ? QLatin1Char('/') + name
                                                  : path + QLatin1Char('/') + name;
        if (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
            entry.mode = attributes.permissions;
            entry.isDir = (attributes.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR;
            entry.isSymlink = (attributes.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFLNK;
        }
        if (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE)
            entry.size = qint64(attributes.filesize);
        if (attributes.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
            entry.mtime = qint64(attributes.mtime);
        entries.append(entry);
    }
    libssh2_sftp_close_handle(dir);

    // SFTP v3 的 readdir 只给链接自己的属性（类型是 l），所以软链目录会被当成文件，
    // 既没有展开箭头也点不开。这里对符号链接补一次 stat（跟随链接）拿目标的类型。
    // 有些服务端连权限位都不给（type 未知），这种也补一次，否则同样认不出目录。
    // 上限保护：一次列目录最多补查 200 个，避免上千个链接的老机器上列目录变卡。
    int probeBudget = 200;
    for (SftpEntry &entry : entries) {
        const quint32 type = entry.mode & LIBSSH2_SFTP_S_IFMT;
        if (!entry.isSymlink && type != 0)
            continue;
        if (probeBudget-- <= 0)
            break;
        LIBSSH2_SFTP_ATTRIBUTES target{};
        const QByteArray targetPath = entry.path.toUtf8();
        if (libssh2_sftp_stat(sftp->sftp, targetPath.constData(), &target) != 0) {
            entry.isBrokenLink = entry.isSymlink; // 断链：界面上压暗，仍显示成链接
            continue;
        }
        if (target.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
            // 只用来判定目标是不是目录；权限列保持链接自己的权限（和 ls -l 一致）
            const quint32 targetMode = target.permissions;
            entry.isDir = (targetMode & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR;
        }
        if (target.flags & LIBSSH2_SFTP_ATTR_SIZE)
            entry.size = qint64(target.filesize);
        if (target.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
            entry.mtime = qint64(target.mtime);
    }

    std::sort(entries.begin(), entries.end(), [](const SftpEntry &a, const SftpEntry &b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return entries;
}

bool SftpWorker::listDir(SftpHandle *sftp, const Request &request)
{
    QString error;
    const SftpEntryList entries = listEntries(sftp, request.path, &error);
    if (!error.isEmpty()) {
        emit failedOp(request.id, QStringLiteral("无法打开目录 %1：%2").arg(request.path, error));
        return false;
    }
    emit listed(request.id, request.path, entries);
    return true;
}

bool SftpWorker::statIsDir(SftpHandle *sftp, const QString &path, bool *isDir)
{
    const QByteArray remotePath = path.toUtf8();
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    const int rc = libssh2_sftp_stat_ex(sftp->sftp, remotePath.constData(), size_t(remotePath.size()),
                                        LIBSSH2_SFTP_STAT, &attributes);
    if (rc != 0) {
        *isDir = false;
        return false;
    }
    *isDir = (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
        && (attributes.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR;
    return true;
}

// 判断路径本身是不是符号链接（lstat 不跟随链接）。删除时靠它避免顺着链接
// 把目标目录里的东西一起删掉 —— 那属于彻底的数据事故。
bool SftpWorker::pathIsLink(SftpHandle *sftp, const QString &path, bool *isLink)
{
    const QByteArray remotePath = path.toUtf8();
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    const int rc = libssh2_sftp_stat_ex(sftp->sftp, remotePath.constData(), size_t(remotePath.size()),
                                        LIBSSH2_SFTP_LSTAT, &attributes);
    if (rc != 0) {
        *isLink = false;
        return false;
    }
    *isLink = (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
              && (attributes.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFLNK;
    return true;
}

bool SftpWorker::downloadFile(SftpHandle *sftp, const Request &request)
{
    bool isDir = false;
    if (!statIsDir(sftp, request.path, &isDir)) {
        emit failedOp(request.id, QStringLiteral("无法读取 %1 的属性：%2").arg(request.path, sftpError(sftp)));
        return false;
    }
    if (isDir) {
        QString error;
        if (!downloadRecursive(sftp, request.path, request.path2, request, &error)) {
            emit failedOp(request.id, QStringLiteral("下载目录 %1 失败：%2").arg(request.path, error));
            return false;
        }
        emit succeeded(request.id, QStringLiteral("已下载目录 %1 → %2").arg(request.path, request.path2));
        return true;
    }

    const QByteArray remotePath = request.path.toUtf8();
    LIBSSH2_SFTP_HANDLE *file = libssh2_sftp_open_ex(sftp->sftp, remotePath.constData(),
                                                    size_t(remotePath.size()), LIBSSH2_FXF_READ, 0,
                                                    LIBSSH2_SFTP_OPENFILE);
    if (!file) {
        emit failedOp(request.id, QStringLiteral("无法打开远端文件 %1：%2").arg(request.path, sftpError(sftp)));
        return false;
    }

    QFileInfo info(request.path2);
    QDir().mkpath(info.absolutePath());
    QFile local(info.absoluteFilePath());
    if (!local.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        libssh2_sftp_close_handle(file);
        emit failedOp(request.id, QStringLiteral("无法写入本地文件 %1：%2").arg(info.absoluteFilePath(), local.errorString()));
        return false;
    }

    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    qint64 total = 0;
    if (libssh2_sftp_fstat_ex(file, &attributes, 0) == 0 && (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE))
        total = qint64(attributes.filesize);

    QByteArray buffer(32768, Qt::Uninitialized);
    qint64 done = 0;
    ProgressMeter meter;
    meter.start();
    bool ok = true;
    while (true) {
        const ssize_t got = libssh2_sftp_read(file, buffer.data(), size_t(buffer.size()));
        if (got == 0)
            break;
        if (got < 0) {
            ok = false;
            break;
        }
        if (local.write(buffer.constData(), got) != got) {
            ok = false;
            break;
        }
        done += got;
        if (meter.shouldReport(done)) {
            meter.note(done);
            emit progressed(request.id, done, total, meter.speed(done));
        }
    }
    libssh2_sftp_close_handle(file);
    local.close();

    if (!ok) {
        emit failedOp(request.id, QStringLiteral("下载 %1 失败：%2").arg(request.path, sftpError(sftp)));
        return false;
    }
    emit progressed(request.id, done, total > 0 ? total : done, meter.speed(done));
    emit succeeded(request.id, QStringLiteral("已下载 %1 → %2").arg(request.path, info.absoluteFilePath()));
    return true;
}

bool SftpWorker::downloadRecursive(SftpHandle *sftp, const QString &remote, const QString &local,
                                   const Request &request, QString *error, int depth)
{
    // 软链目录会被当成目录往下走，遇到 "链接指回上层" 的环就会无限递归
    if (depth > 64) {
        *error = QStringLiteral("目录层级过深（可能有循环符号链接）：%1").arg(remote);
        return false;
    }
    QDir().mkpath(local);
    QString listError;
    const SftpEntryList entries = listEntries(sftp, remote, &listError);
    if (!listError.isEmpty()) {
        *error = listError;
        return false;
    }
    for (const SftpEntry &entry : entries) {
        const QString localChild = local + QLatin1Char('/') + entry.name;
        if (entry.isDir) {
            if (!downloadRecursive(sftp, entry.path, localChild, request, error, depth + 1))
                return false;
            continue;
        }
        // 单个文件：直接读远端写本地
        const QByteArray remotePath = entry.path.toUtf8();
        LIBSSH2_SFTP_HANDLE *file = libssh2_sftp_open_ex(sftp->sftp, remotePath.constData(),
                                                        size_t(remotePath.size()), LIBSSH2_FXF_READ, 0,
                                                        LIBSSH2_SFTP_OPENFILE);
        if (!file) {
            *error = sftpError(sftp);
            return false;
        }
        QFile localFile(localChild);
        if (!localFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            libssh2_sftp_close_handle(file);
            *error = localFile.errorString();
            return false;
        }
        QByteArray buffer(32768, Qt::Uninitialized);
        qint64 done = 0;
        ProgressMeter meter;
        meter.start();
        bool ok = true;
        while (true) {
            const ssize_t got = libssh2_sftp_read(file, buffer.data(), size_t(buffer.size()));
            if (got == 0)
                break;
            if (got < 0 || localFile.write(buffer.constData(), got) != got) {
                ok = false;
                break;
            }
            done += got;
            if (meter.shouldReport(done)) {
                meter.note(done);
                emit progressed(request.id, done, entry.size, meter.speed(done));
            }
        }
        libssh2_sftp_close_handle(file);
        localFile.close();
        if (!ok) {
            *error = sftpError(sftp);
            return false;
        }
    }
    return true;
}

bool SftpWorker::removeRecursive(SftpHandle *sftp, const QString &path, QString *error)
{
    // 先按 lstat 看路径本身：符号链接一律只 unlink 链接，
    // 绝不顺着链接递归删到目标目录里去（软链目录的 isDir 是跟随链接算出来的）
    bool isLink = false;
    if (!pathIsLink(sftp, path, &isLink)) {
        *error = sftpError(sftp);
        return false;
    }
    bool isDir = false;
    if (!isLink && !statIsDir(sftp, path, &isDir)) {
        *error = sftpError(sftp);
        return false;
    }
    if (isLink || !isDir) {
        const QByteArray remotePath = path.toUtf8();
        if (libssh2_sftp_unlink_ex(sftp->sftp, remotePath.constData(), size_t(remotePath.size())) != 0) {
            *error = sftpError(sftp);
            return false;
        }
        return true;
    }

    QString listError;
    const SftpEntryList entries = listEntries(sftp, path, &listError);
    if (!listError.isEmpty()) {
        *error = listError;
        return false;
    }
    for (const SftpEntry &entry : entries) {
        if (!removeRecursive(sftp, entry.path, error))
            return false;
    }
    const QByteArray remotePath = path.toUtf8();
    if (libssh2_sftp_rmdir_ex(sftp->sftp, remotePath.constData(), size_t(remotePath.size())) != 0) {
        *error = sftpError(sftp);
        return false;
    }
    return true;
}

bool SftpWorker::uploadFile(SftpHandle *sftp, const Request &request)
{
    QFile local(request.path);
    if (!local.open(QIODevice::ReadOnly)) {
        emit failedOp(request.id, QStringLiteral("无法读取本地文件 %1：%2").arg(request.path, local.errorString()));
        return false;
    }
    const qint64 total = local.size();
    const QByteArray remotePath = request.path2.toUtf8();
    LIBSSH2_SFTP_HANDLE *file = libssh2_sftp_open_ex(
        sftp->sftp, remotePath.constData(), size_t(remotePath.size()),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR
            | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH,
        LIBSSH2_SFTP_OPENFILE);
    if (!file) {
        emit failedOp(request.id, QStringLiteral("无法创建远端文件 %1：%2").arg(request.path2, sftpError(sftp)));
        return false;
    }

    QByteArray buffer(32768, Qt::Uninitialized);
    qint64 done = 0;
    ProgressMeter meter;
    meter.start();
    bool ok = true;
    while (!local.atEnd()) {
        const qint64 readBytes = local.read(buffer.data(), buffer.size());
        if (readBytes <= 0) {
            ok = false;
            break;
        }
        qint64 offset = 0;
        while (offset < readBytes) {
            const ssize_t written = libssh2_sftp_write(file, buffer.constData() + offset,
                                                        size_t(readBytes - offset));
            if (written <= 0) {
                ok = false;
                break;
            }
            offset += written;
            done += written;
            if (meter.shouldReport(done)) {
                meter.note(done);
                emit progressed(request.id, done, total, meter.speed(done));
            }
        }
        if (!ok)
            break;
    }
    libssh2_sftp_close_handle(file);

    if (!ok) {
        emit failedOp(request.id, QStringLiteral("上传 %1 失败：%2").arg(request.path, sftpError(sftp)));
        return false;
    }
    emit progressed(request.id, done, total, meter.speed(done));
    emit succeeded(request.id, QStringLiteral("已上传 %1 → %2").arg(request.path, request.path2));
    return true;
}

bool SftpWorker::makeDir(SftpHandle *sftp, const Request &request)
{
    const QByteArray path = request.path.toUtf8();
    const int rc = libssh2_sftp_mkdir_ex(sftp->sftp, path.constData(), size_t(path.size()),
                                         LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP
                                             | LIBSSH2_SFTP_S_IXGRP | LIBSSH2_SFTP_S_IROTH
                                             | LIBSSH2_SFTP_S_IXOTH);
    if (rc != 0) {
        emit failedOp(request.id, QStringLiteral("创建目录 %1 失败：%2").arg(request.path, sftpError(sftp)));
        return false;
    }
    emit succeeded(request.id, QStringLiteral("已创建目录 %1").arg(request.path));
    return true;
}

bool SftpWorker::createFile(SftpHandle *sftp, const Request &request)
{
    const QByteArray path = request.path.toUtf8();
    LIBSSH2_SFTP_HANDLE *file = libssh2_sftp_open_ex(
        sftp->sftp, path.constData(), size_t(path.size()),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH,
        LIBSSH2_SFTP_OPENFILE);
    if (!file) {
        emit failedOp(request.id, QStringLiteral("创建文件 %1 失败：%2").arg(request.path, sftpError(sftp)));
        return false;
    }
    libssh2_sftp_close_handle(file);
    emit succeeded(request.id, QStringLiteral("已创建文件 %1").arg(request.path));
    return true;
}

bool SftpWorker::removePath(SftpHandle *sftp, const Request &request)
{
    bool isLink = false;
    bool isDir = false;
    QString error;
    if (!pathIsLink(sftp, request.path, &isLink)
        || (!isLink && !statIsDir(sftp, request.path, &isDir))) {
        emit failedOp(request.id, QStringLiteral("无法读取 %1 的属性：%2").arg(request.path, sftpError(sftp)));
        return false;
    }
    if (!removeRecursive(sftp, request.path, &error)) {
        emit failedOp(request.id, (isDir ? QStringLiteral("删除目录 %1 失败：%2")
                                         : QStringLiteral("删除文件 %1 失败：%2"))
                                     .arg(request.path, error));
        return false;
    }
    emit succeeded(request.id, isDir ? QStringLiteral("已递归删除目录 %1").arg(request.path)
                                     : QStringLiteral("已删除 %1").arg(request.path));
    return true;
}

bool SftpWorker::renamePath(SftpHandle *sftp, const Request &request)
{
    const QByteArray from = request.path.toUtf8();
    const QByteArray to = request.path2.toUtf8();
    const int rc = libssh2_sftp_rename_ex(sftp->sftp, from.constData(), size_t(from.size()),
                                          to.constData(), size_t(to.size()),
                                          LIBSSH2_SFTP_RENAME_OVERWRITE);
    if (rc != 0) {
        emit failedOp(request.id, QStringLiteral("重命名 %1 → %2 失败：%3")
                                             .arg(request.path, request.path2, sftpError(sftp)));
        return false;
    }
    emit succeeded(request.id, QStringLiteral("已重命名 %1 → %2").arg(request.path, request.path2));
    return true;
}

} // namespace termdock
