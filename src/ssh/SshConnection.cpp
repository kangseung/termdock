#include "ssh/SshConnection.h"

#include "ssh/NetCompat.h"

#include <QByteArray>

#include <mutex>

namespace termdock {

namespace {

void ensureLibssh2()
{
    static std::once_flag once;
    std::call_once(once, []() { libssh2_init(0); });
}

// keyboard-interactive 回调不携带 session 上下文，用 thread_local 传递密码：
// 每个 SSH 连接都独占一个工作线程，因此这样是安全的。
thread_local const char *g_keyboardPassword = nullptr;

void kbdInteractiveCallback(const char * /*name*/, int /*name_len*/, const char * /*instruction*/,
                            int /*instruction_len*/, int num_prompts,
                            const LIBSSH2_USERAUTH_KBDINT_PROMPT * /*prompts*/,
                            LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void ** /*abstract*/)
{
    for (int i = 0; i < num_prompts; ++i) {
        const size_t len = g_keyboardPassword ? strlen(g_keyboardPassword) : 0;
        responses[i].text = static_cast<char *>(malloc(len + 1));
        if (responses[i].text) {
            if (len)
                memcpy(responses[i].text, g_keyboardPassword, len);
            responses[i].text[len] = '\0';
            responses[i].length = static_cast<unsigned int>(len);
        }
    }
}

} // namespace

SshConnection::~SshConnection()
{
    close();
}

void SshConnection::close()
{
    if (m_session) {
        libssh2_session_disconnect_ex(m_session, SSH_DISCONNECT_BY_APPLICATION, "bye", "");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    if (m_sock != net::kInvalidSocket) {
        net::closeSocket(m_sock);
        m_sock = net::kInvalidSocket;
    }
}

QString SshConnection::lastError(LIBSSH2_SESSION *session)
{
    if (!session)
        return QStringLiteral("libssh2 会话无效");
    char *message = nullptr;
    int length = 0;
    const int code = libssh2_session_last_error(session, &message, &length, 0);
    const QString detail = message ? QString::fromUtf8(message, length) : QString();
    return QStringLiteral("%1 (libssh2 %2)").arg(detail).arg(code);
}

bool SshConnection::waitSocket(int timeoutMs, bool forWrite)
{
    return net::waitSocket(m_sock, timeoutMs, forWrite);
}

bool SshConnection::connectSocket(const QString &host, int port, int timeoutMs, QString *error)
{
    return net::connectWithTimeout(host, port, timeoutMs, &m_sock, error);
}

bool SshConnection::handshake(int timeoutMs, QString *error)
{
    m_session = libssh2_session_init();
    if (!m_session) {
        if (error)
            *error = QStringLiteral("初始化 libssh2 会话失败");
        return false;
    }
    libssh2_session_set_blocking(m_session, 0);

    const int deadlineMs = timeoutMs;
    int waited = 0;
    int rc = LIBSSH2_ERROR_EAGAIN;
    while (rc == LIBSSH2_ERROR_EAGAIN && waited < deadlineMs) {
        rc = libssh2_session_handshake(m_session, m_sock);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(100, false);
            waited += 100;
        }
    }
    if (rc != 0) {
        if (error)
            *error = QStringLiteral("SSH 握手失败：%1").arg(lastError(m_session));
        return false;
    }

    size_t keyLen = 0;
    int keyType = 0;
    const char *hostKey = libssh2_session_hostkey(m_session, &keyLen, &keyType);
    if (!hostKey) {
        if (error)
            *error = QStringLiteral("无法获取主机密钥");
        return false;
    }
    const char *sha256 = libssh2_hostkey_hash(m_session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (sha256) {
        const QByteArray raw(sha256, 32);
        m_fingerprint = QStringLiteral("SHA256:")
            + QString::fromLatin1(raw.toBase64(QByteArray::OmitTrailingEquals));
    }
    (void)keyLen;
    switch (keyType) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:
        m_hostKeyType = QStringLiteral("ssh-rsa");
        break;
    case LIBSSH2_HOSTKEY_TYPE_DSS:
        m_hostKeyType = QStringLiteral("ssh-dss");
        break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
        m_hostKeyType = QStringLiteral("ecdsa-sha2-nistp256");
        break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
        m_hostKeyType = QStringLiteral("ecdsa-sha2-nistp384");
        break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
        m_hostKeyType = QStringLiteral("ecdsa-sha2-nistp521");
        break;
    case LIBSSH2_HOSTKEY_TYPE_ED25519:
        m_hostKeyType = QStringLiteral("ssh-ed25519");
        break;
    default:
        m_hostKeyType = QStringLiteral("unknown");
        break;
    }
    return true;
}

bool SshConnection::authenticate(const SshParams &params, int timeoutMs, QString *error)
{
    const QByteArray user = params.user.toUtf8();
    const QByteArray password = params.password.toUtf8();

    int waited = 0;
    int rc = LIBSSH2_ERROR_EAGAIN;
    while (rc == LIBSSH2_ERROR_EAGAIN && waited < timeoutMs) {
        rc = libssh2_userauth_password_ex(m_session, user.constData(), unsigned(user.size()),
                                          password.constData(), unsigned(password.size()), nullptr);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(100, false);
            waited += 100;
        }
    }
    if (rc == 0)
        return true;

    // 记录密码认证的失败原因，后面若 keyboard-interactive 也失败，用它作为主提示
    char *passwordMessage = nullptr;
    int passwordMessageLength = 0;
    libssh2_session_last_error(m_session, &passwordMessage, &passwordMessageLength, 0);
    const QString passwordError = passwordMessage
        ? QString::fromUtf8(passwordMessage, passwordMessageLength)
        : QStringLiteral("未知原因");

    // 密码不被接受时尝试 keyboard-interactive（部分服务器只开放该方式）
    g_keyboardPassword = password.constData();
    waited = 0;
    rc = LIBSSH2_ERROR_EAGAIN;
    while (rc == LIBSSH2_ERROR_EAGAIN && waited < timeoutMs) {
        rc = libssh2_userauth_keyboard_interactive_ex(m_session, user.constData(), unsigned(user.size()),
                                                      kbdInteractiveCallback);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(100, false);
            waited += 100;
        }
    }
    if (rc == 0)
        return true;

    g_keyboardPassword = nullptr;
    if (error) {
        *error = QStringLiteral("认证失败：用户名或密码不正确（%1）").arg(passwordError);
    }
    return false;
}

bool SshConnection::connectAndAuth(const SshParams &params, int timeoutMs, QString *error)
{
    ensureLibssh2();
    if (!connectSocket(params.host, params.port, timeoutMs, error))
        return false;
    if (!handshake(timeoutMs, error)) {
        close();
        return false;
    }
    // 必须在 authenticate() 之前核对指纹：否则密码已经交给可能是中人的主机。
    // ShellSession 和 SftpWorker 都走这里，所以两条通道都会被挡住。
    if (!params.expectedFingerprint.isEmpty() && params.expectedFingerprint != m_fingerprint) {
        if (error) {
            *error = QStringLiteral("主机密钥已变化，连接已中止（未发送密码）：记录的指纹 %1，本次为 %2")
                         .arg(params.expectedFingerprint, m_fingerprint);
        }
        close();
        return false;
    }
    if (!authenticate(params, timeoutMs, error)) {
        close();
        return false;
    }
    return true;
}

} // namespace termdock
