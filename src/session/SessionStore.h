#pragma once

#include <QString>

#include "session/Session.h"

namespace termdock {

// sessions.json + known_hosts.json 的读写。所有文件都在 ~/.config/termdock 下。
class SessionStore {
public:
    static SessionList load(QString *error = nullptr);
    static bool save(const SessionList &sessions, QString *error = nullptr);

    // 主机指纹：首次连接记录（TOFU），之后不一致就拒绝连接。
    // 指纹库状态要和"没有这条记录"区分开：文件坏了不能当成首次连接直接信任。
    enum class KnownHosts { Ok, Missing, Corrupt };
    static KnownHosts knownHostsState(QString *error = nullptr);
    static QString knownFingerprint(const QString &host, int port);
    static bool rememberFingerprint(const QString &host, int port, const QString &fingerprint,
                                   QString *error = nullptr);
    static void forgetFingerprint(const QString &host, int port);
};

} // namespace termdock
