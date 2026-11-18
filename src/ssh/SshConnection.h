#pragma once

#include <QString>

#include <libssh2.h>

namespace termdock {

struct SshParams {
    QString host;
    int port = 22;
    QString user;
    QString password;
    bool allowUnknownHostKey = true; // 首次连接采用 TOFU：记录指纹
};

// 封装 socket + libssh2 会话的建立过程（连接、握手、认证、指纹）。
// 不做任何 UI 决策：指纹由调用方决定是否信任。
class SshConnection {
public:
    SshConnection() = default;
    ~SshConnection();
    SshConnection(const SshConnection &) = delete;
    SshConnection &operator=(const SshConnection &) = delete;

    bool connectAndAuth(const SshParams &params, int timeoutMs, QString *error);
    void close();

    LIBSSH2_SESSION *session() const { return m_session; }
    int socket() const { return m_sock; }
    QString fingerprint() const { return m_fingerprint; }
    QString hostKeyType() const { return m_hostKeyType; }

    static QString lastError(LIBSSH2_SESSION *session);

private:
    bool connectSocket(const QString &host, int port, int timeoutMs, QString *error);
    bool handshake(int timeoutMs, QString *error);
    bool authenticate(const SshParams &params, int timeoutMs, QString *error);
    bool waitSocket(int timeoutMs, bool forWrite);

    int m_sock = -1;
    LIBSSH2_SESSION *m_session = nullptr;
    QString m_fingerprint;
    QString m_hostKeyType;
};

} // namespace termdock
