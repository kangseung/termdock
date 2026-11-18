#include "ssh/NetCompat.h"

#include <QByteArray>

#ifdef _WIN32
#    include <ws2tcpip.h>
#else
#    include <arpa/inet.h>
#    include <cerrno>
#    include <cstring>
#    include <fcntl.h>
#    include <netdb.h>
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    include <poll.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

namespace termdock::net {

namespace {

#ifdef _WIN32
// Winsock 必须在进程内初始化一次，且生命周期覆盖所有 socket。
// 用静态对象在退出阶段再 WSACleanup，避免与仍在结束的线程抢资源。
struct WinsockGuard {
    WinsockGuard() { WSAStartup(MAKEWORD(2, 2), &data); }
    ~WinsockGuard() { WSACleanup(); }
    WSADATA data{};
};
#endif

QString socketErrorText(int code)
{
#ifdef _WIN32
    wchar_t *buffer = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, DWORD(code), 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    QString text;
    if (len && buffer)
        text = QString::fromWCharArray(buffer, int(len)).trimmed();
    if (buffer)
        LocalFree(buffer);
    if (text.isEmpty())
        text = QStringLiteral("错误码 %1").arg(code);
    return text;
#else
    return QString::fromLocal8Bit(std::strerror(code));
#endif
}

} // namespace

void ensureInit()
{
#ifdef _WIN32
    static WinsockGuard guard;
    (void)guard;
#endif
}

void closeSocket(Socket sock)
{
    if (sock == kInvalidSocket)
        return;
#ifdef _WIN32
    closesocket(sock);
#else
    ::close(sock);
#endif
}

bool waitSocket(Socket sock, int timeoutMs, bool forWrite)
{
    if (sock == kInvalidSocket)
        return false;
#ifdef _WIN32
    // 用 select 而不是 WSAPoll：WSAPoll 在 Windows 上漏报连接失败（已知老 bug），
    // 拿它代替 poll 会让"连不上的主机"一直等到超时，而不是立刻报错。
    fd_set readSet;
    fd_set writeSet;
    fd_set errorSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    if (forWrite)
        FD_SET(sock, &writeSet);
    else
        FD_SET(sock, &readSet);
    FD_SET(sock, &errorSet);

    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int rc = ::select(0, forWrite ? nullptr : &readSet, forWrite ? &writeSet : nullptr,
                            &errorSet, &tv);
    if (rc <= 0)
        return false;
    if (FD_ISSET(sock, &errorSet))
        return true; // 让上层读到真实错误，而不是在这里当成超时
    return forWrite ? FD_ISSET(sock, &writeSet) != 0 : FD_ISSET(sock, &readSet) != 0;
#else
    pollfd pfd{};
    pfd.fd = sock;
    pfd.events = forWrite ? POLLOUT : POLLIN;
    const int rc = ::poll(&pfd, 1, timeoutMs);
    if (rc <= 0)
        return false;
    return (pfd.revents & (forWrite ? POLLOUT : POLLIN)) != 0 || (pfd.revents & POLLHUP) != 0;
#endif
}

bool setBlocking(Socket sock, bool blocking)
{
    if (sock == kInvalidSocket)
        return false;
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0)
        return false;
    const int target = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return fcntl(sock, F_SETFL, target) == 0;
#endif
}

QString lastSocketError()
{
#ifdef _WIN32
    return socketErrorText(int(WSAGetLastError()));
#else
    return socketErrorText(errno);
#endif
}

bool connectWithTimeout(const QString &host, int port, int timeoutMs, Socket *out, QString *error)
{
    ensureInit();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *result = nullptr;
    const QByteArray hostName = host.toUtf8();
    const QByteArray portStr = QByteArray::number(port);
    if (getaddrinfo(hostName.constData(), portStr.constData(), &hints, &result) != 0 || !result) {
        if (error)
            *error = QStringLiteral("无法解析主机 %1").arg(host);
        return false;
    }

    QString lastMessage = QStringLiteral("连接失败");
    for (addrinfo *it = result; it; it = it->ai_next) {
        const Socket sock = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == kInvalidSocket)
            continue;

        bool ok = false;
        setBlocking(sock, false);
        if (::connect(sock, it->ai_addr, int(it->ai_addrlen)) == 0) {
            ok = true;
        } else {
#ifdef _WIN32
            const bool inProgress = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            const bool inProgress = errno == EINPROGRESS;
#endif
            if (inProgress) {
                if (waitSocket(sock, timeoutMs, true)) {
                    int soError = 0;
#ifdef _WIN32
                    int len = sizeof(soError);
#else
                    socklen_t len = sizeof(soError);
#endif
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soError),
                               &len);
                    if (soError == 0) {
                        ok = true;
                    } else {
                        lastMessage = QStringLiteral("连接被拒绝：%1").arg(socketErrorText(soError));
                    }
                } else {
                    lastMessage = QStringLiteral("连接超时（%1 秒）").arg(timeoutMs / 1000);
                }
            } else {
                lastMessage = QStringLiteral("无法连接：%1").arg(lastSocketError());
            }
        }

        if (ok) {
            setBlocking(sock, true);
            const int one = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one),
                       sizeof(one));
            setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>(&one),
                       sizeof(one));
            *out = sock;
            freeaddrinfo(result);
            return true;
        }
        closeSocket(sock);
    }
    freeaddrinfo(result);
    if (error)
        *error = lastMessage;
    return false;
}

} // namespace termdock::net
