#pragma once

#include <QByteArray>
#include <QString>

namespace termdock {

// 可选的「密码恢复保险」：用恢复口令（PBKDF2）派生 KEK，把主密钥包一层存到
// 数据目录的 recovery.json。开启后即使 key 文件丢失，也能用口令解出历史密码。
// 关闭 = 删除该文件，随时可开可关。
class Recovery {
public:
    static bool enabled();
    static bool enable(const QString &passphrase, QString *error);
    static bool disable();
    // 用口令解出主密钥；成功返回 true，masterKey 为 32 字节原始密钥
    static bool unlock(const QString &passphrase, QByteArray *masterKey, QString *error);
    static QString filePath();
};

} // namespace termdock
