#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace termdock {

// 会话密码的本地加密：AES-256-GCM，密钥放在数据目录的 key 文件（0600）。
//
// 另外提供一层「恢复保险」（可选开启）：用用户设置的恢复口令经 PBKDF2-SHA256
// 派生 KEK，把主密钥再包一层存进 recovery.json。这样即使 key 文件丢了，
// 只要记得恢复口令，仍能解出历史保存的账号密码。
class Crypto {
public:
    static QString encrypt(const QString &plain, QString *error = nullptr);
    static QString decrypt(const QString &encoded, QString *error = nullptr);

    // 用显式密钥加解密（恢复保险用）
    static QString encryptWithKey(const QByteArray &key, const QByteArray &plain,
                                  QString *error = nullptr);
    static QByteArray decryptWithKey(const QByteArray &key, const QString &encoded,
                                     QString *error = nullptr);

    static QByteArray masterKey(QString *error = nullptr);
    // 只读路径用：key 文件不存在就报错，绝不现场生成（否则会把还能救的密文废掉）
    static QByteArray existingMasterKey(QString *error = nullptr);
    static QByteArray randomBytes(int count);

    // 恢复保险：口令 → PBKDF2 → KEK → 包住主密钥
    static QJsonObject wrapMasterKey(const QString &passphrase, QString *error = nullptr);
    static QByteArray unwrapMasterKey(const QJsonObject &payload, const QString &passphrase,
                                      QString *error = nullptr);

private:
    static QByteArray readKey(bool createIfMissing, QString *error);
    static QByteArray deriveKey(const QByteArray &passphrase, const QByteArray &salt, int iterations);
};

} // namespace termdock
