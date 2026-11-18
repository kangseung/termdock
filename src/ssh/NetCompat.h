#pragma once

#include <QString>

// 必须先带进 libssh2.h：它在 _WIN32 下会先包含 <winsock2.h>，
// 这样后续任何 <windows.h> 都不会插到 winsock2 之前（否则撞 winsock 1/2 冲突）。
#include <libssh2.h>

namespace termdock::net {

// 自己定义 Socket，不用 libssh2_socket_t：后者只在较新的 libssh2（1.11+）里才有，
// 直接依赖会让老发行版的 Linux 编不过。
#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
inline constexpr Socket kInvalidSocket = -1;
#endif

// Windows 下初始化 Winsock（进程内只做一次）；POSIX 上什么也不做。
// 任何 socket 调用之前都必须先走到这里，否则 Windows 上 getaddrinfo/socket 直接失败。
void ensureInit();

// 关掉一个 socket，并把它设成 kInvalidSocket 语义的调用方状态由调用方自己维护。
void closeSocket(Socket sock);

// 等 socket 可读/可写，返回 false 表示超时或出错。
bool waitSocket(Socket sock, int timeoutMs, bool forWrite);

// 把刚连上的 socket 设成阻塞/非阻塞。
bool setBlocking(Socket sock, bool blocking);

// 系统错误码对应的可读文本（POSIX 用 errno，Windows 用 WSAGetLastError）。
QString lastSocketError();

// 带超时的非阻塞 connect：成功时把 socket 写进 *out。
// 失败时把中文原因写进 *error（调用方直接展示）。
bool connectWithTimeout(const QString &host, int port, int timeoutMs, Socket *out, QString *error);

} // namespace termdock::net
