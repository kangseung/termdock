#pragma once

#include <QByteArray>
#include <QString>

namespace termdock {

// 会话密码的本地加密：AES-256-GCM，密钥放在 ~/.config/termdock/key（0600）。
// 说明：密钥与密文同机存放，目的是防止明文字段被随手 grep/同步出去，
// 不等价于系统钥匙串级别的保护。需要更强保护时应接入 libsecret / Keychain。
class Crypto {
public:
    static QString encrypt(const QString &plain, QString *error = nullptr);
    static QString decrypt(const QString &encoded, QString *error = nullptr);

private:
    static QByteArray masterKey(QString *error);
    static QByteArray randomBytes(int count);
};

} // namespace termdock
