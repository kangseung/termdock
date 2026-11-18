#include "ssh/ShellSession.h"

#include <QMutexLocker>
#include <poll.h>
#include <sys/socket.h>

namespace termdock {

namespace {

bool waitSocketFor(int fd, int timeoutMs, bool forWrite)
{
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = forWrite ? POLLOUT : POLLIN;
    const int rc = ::poll(&pfd, 1, timeoutMs);
    if (rc <= 0)
        return false;
    return (pfd.revents & (forWrite ? POLLOUT : POLLIN)) != 0 || (pfd.revents & POLLHUP) != 0;
}

} // namespace

ShellSession::ShellSession(const SshParams &params, int cols, int rows, QObject *parent)
    : QThread(parent)
    , m_params(params)
    , m_cols(cols)
    , m_rows(rows)
{
}

ShellSession::~ShellSession()
{
    stop();
    wait(5000);
}

void ShellSession::sendInput(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    QMutexLocker locker(&m_mutex);
    if (m_input.size() > 4 * 1024 * 1024)
        return; // 防止对端卡死时无限增长
    m_input.append(data);
}

void ShellSession::resizePty(int cols, int rows)
{
    if (cols <= 0 || rows <= 0)
        return;
    QMutexLocker locker(&m_mutex);
    m_pendingCols = cols;
    m_pendingRows = rows;
    m_resizePending.storeRelease(1);
}

void ShellSession::stop()
{
    m_stopRequested.storeRelease(1);
}

void ShellSession::run()
{
    SshConnection conn;
    QString error;
    if (!conn.connectAndAuth(m_params, 15000, &error)) {
        emit failed(error);
        return;
    }
    emit connected(conn.fingerprint(), conn.hostKeyType());

    LIBSSH2_SESSION *session = conn.session();
    libssh2_session_set_blocking(session, 0);
    const int sock = conn.socket();

    // 打开会话通道
    LIBSSH2_CHANNEL *channel = nullptr;
    while (!m_stopRequested.loadAcquire()) {
        channel = libssh2_channel_open_session(session);
        if (channel)
            break;
        if (libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {
            emit failed(QStringLiteral("打开 SSH 通道失败：%1").arg(SshConnection::lastError(session)));
            conn.close();
            return;
        }
        waitSocketFor(sock, 50, false);
    }
    if (!channel) {
        conn.close();
        return;
    }

    // 申请 PTY（xterm-256color，让远端有 256 色和正确的光标/换行行为）
    const QByteArray termType = QByteArrayLiteral("xterm-256color");
    int rc = LIBSSH2_ERROR_EAGAIN;
    while (rc == LIBSSH2_ERROR_EAGAIN && !m_stopRequested.loadAcquire()) {
        rc = libssh2_channel_request_pty_ex(channel, termType.constData(),
                                            unsigned(termType.size()), nullptr, 0, m_cols, m_rows,
                                            0, 0);
        if (rc == LIBSSH2_ERROR_EAGAIN)
            waitSocketFor(sock, 50, false);
    }
    if (rc != 0) {
        emit failed(QStringLiteral("申请 PTY 失败：%1").arg(SshConnection::lastError(session)));
        libssh2_channel_free(channel);
        conn.close();
        return;
    }

    rc = LIBSSH2_ERROR_EAGAIN;
    while (rc == LIBSSH2_ERROR_EAGAIN && !m_stopRequested.loadAcquire()) {
        rc = libssh2_channel_shell(channel);
        if (rc == LIBSSH2_ERROR_EAGAIN)
            waitSocketFor(sock, 50, false);
    }
    if (rc != 0) {
        emit failed(QStringLiteral("启动 shell 失败：%1").arg(SshConnection::lastError(session)));
        libssh2_channel_free(channel);
        conn.close();
        return;
    }

    libssh2_keepalive_config(session, 1, 30);
    int secondsToNextKeepalive = 30;
    QString closeReason;

    char buffer[65536];
    while (!m_stopRequested.loadAcquire()) {
        // 1) 上行输入
        QByteArray pending;
        {
            QMutexLocker locker(&m_mutex);
            pending.swap(m_input);
        }
        if (!pending.isEmpty()) {
            qsizetype offset = 0;
            while (offset < pending.size()) {
                const ssize_t written = libssh2_channel_write(channel, pending.constData() + offset,
                                                              size_t(pending.size() - offset));
                if (written > 0) {
                    offset += written;
                } else if (written == LIBSSH2_ERROR_EAGAIN) {
                    if (!waitSocketFor(sock, 50, true))
                        continue;
                } else {
                    closeReason = QStringLiteral("写入失败：%1").arg(SshConnection::lastError(session));
                    m_stopRequested.storeRelease(1);
                    break;
                }
            }
            if (offset < pending.size() && !m_stopRequested.loadAcquire()) {
                QMutexLocker locker(&m_mutex);
                m_input.prepend(pending.mid(offset));
            }
        }

        // 2) 窗口大小变化
        if (m_resizePending.fetchAndStoreRelaxed(0)) {
            int cols = 0;
            int rows = 0;
            {
                QMutexLocker locker(&m_mutex);
                cols = m_pendingCols;
                rows = m_pendingRows;
            }
            if (cols > 0 && rows > 0) {
                m_cols = cols;
                m_rows = rows;
                int sizeRc = LIBSSH2_ERROR_EAGAIN;
                while (sizeRc == LIBSSH2_ERROR_EAGAIN && !m_stopRequested.loadAcquire()) {
                    sizeRc = libssh2_channel_request_pty_size(channel, cols, rows);
                    if (sizeRc == LIBSSH2_ERROR_EAGAIN)
                        waitSocketFor(sock, 50, false);
                }
            }
        }

        // 3) 下行数据
        ssize_t got = 0;
        do {
            got = libssh2_channel_read_ex(channel, 0, buffer, sizeof(buffer));
            if (got > 0)
                emit readyRead(QByteArray(buffer, int(got)));
        } while (got > 0);

        ssize_t errGot = 0;
        do {
            errGot = libssh2_channel_read_ex(channel, SSH_EXTENDED_DATA_STDERR, buffer, sizeof(buffer));
            if (errGot > 0)
                emit readyRead(QByteArray(buffer, int(errGot)));
        } while (errGot > 0);

        if (libssh2_channel_eof(channel)) {
            if (closeReason.isEmpty())
                closeReason = QStringLiteral("远端已关闭连接");
            break;
        }
        if (got < 0 && got != LIBSSH2_ERROR_EAGAIN) {
            closeReason = QStringLiteral("读取失败：%1").arg(SshConnection::lastError(session));
            break;
        }

        // 4) 保活
        libssh2_keepalive_send(session, &secondsToNextKeepalive);

        // 5) 等数据（20ms 轮询，兼顾输入延迟与 CPU 占用）
        waitSocketFor(sock, 20, false);
    }

    libssh2_channel_send_eof(channel);
    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    conn.close();

    if (closeReason.isEmpty())
        closeReason = QStringLiteral("连接已断开");
    emit closedWith(closeReason);
}

} // namespace termdock
