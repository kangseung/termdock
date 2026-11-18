#pragma once

#include <QAtomicInt>
#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QThread>

#include "ssh/SshConnection.h"

namespace termdock {

// 一个 SSH shell 会话跑在独立线程里：非阻塞 libssh2 循环 + 输入队列。
class ShellSession : public QThread {
    Q_OBJECT

public:
    ShellSession(const SshParams &params, int cols, int rows, QObject *parent = nullptr);
    ~ShellSession() override;

    void sendInput(const QByteArray &data);
    void resizePty(int cols, int rows);
    void stop();

signals:
    void connected(const QString &fingerprint, const QString &hostKeyType);
    void failed(const QString &message);
    void readyRead(const QByteArray &data);
    void closedWith(const QString &reason);

protected:
    void run() override;

private:
    SshParams m_params;
    QMutex m_mutex;
    QByteArray m_input;
    int m_cols;
    int m_rows;
    int m_pendingCols = 0;
    int m_pendingRows = 0;
    QAtomicInt m_stopRequested{0};
    QAtomicInt m_resizePending{0};
};

} // namespace termdock
