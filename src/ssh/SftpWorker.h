#pragma once

#include <QAtomicInt>
#include <QMutex>
#include <QQueue>
#include <QString>
#include <QThread>

#include "ssh/SftpTypes.h"
#include "ssh/SshConnection.h"

namespace termdock {

// SFTP 操作跑在独立线程，避免大文件传输卡住终端。使用独立于 shell 的连接。
class SftpWorker : public QThread {
    Q_OBJECT

public:
    enum class Op {
        List,
        Download,
        Upload,
        Mkdir,
        CreateFile,
        Remove,
        Rename,
    };

    explicit SftpWorker(const SshParams &params, QObject *parent = nullptr);
    ~SftpWorker() override;

    quint64 enqueue(Op op, const QString &path, const QString &path2 = QString());
    void stop();

signals:
    void connected(const QString &homePath);
    void failed(const QString &message);
    void listed(quint64 id, const QString &path, const SftpEntryList &entries);
    void progressed(quint64 id, qint64 done, qint64 total, double bytesPerSecond);
    void succeeded(quint64 id, const QString &message);
    void failedOp(quint64 id, const QString &message);

protected:
    void run() override;

private:
    struct Request {
        quint64 id = 0;
        Op op = Op::List;
        QString path;
        QString path2;
    };

    bool handleRequest(struct SftpHandle *sftp, const Request &request);
    bool listDir(struct SftpHandle *sftp, const Request &request);
    SftpEntryList listEntries(struct SftpHandle *sftp, const QString &path, QString *error);
    bool statIsDir(struct SftpHandle *sftp, const QString &path, bool *isDir);
    bool downloadRecursive(struct SftpHandle *sftp, const QString &remote, const QString &local,
                           const Request &request, QString *error);
    bool removeRecursive(struct SftpHandle *sftp, const QString &path, QString *error);
    bool downloadFile(struct SftpHandle *sftp, const Request &request);
    bool uploadFile(struct SftpHandle *sftp, const Request &request);
    bool makeDir(struct SftpHandle *sftp, const Request &request);
    bool createFile(struct SftpHandle *sftp, const Request &request);
    bool removePath(struct SftpHandle *sftp, const Request &request);
    bool renamePath(struct SftpHandle *sftp, const Request &request);
    QString sftpError(struct SftpHandle *sftp) const;

    SshParams m_params;
    QMutex m_mutex;
    QQueue<Request> m_requests;
    quint64 m_nextId = 1;
    QAtomicInt m_stopRequested{0};
};

} // namespace termdock
