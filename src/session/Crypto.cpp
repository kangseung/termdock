#include "session/Crypto.h"

#include "app/AppConfig.h"

#include <QDateTime>
#include <QDir>
#include <QFile>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace termdock {

namespace {
constexpr int kKeyLength = 32;
constexpr int kIvLength = 12;
constexpr int kTagLength = 16;
constexpr int kSaltLength = 16;
constexpr int kKdfIterations = 210000;
constexpr char kPrefix[] = "gcm1:";
} // namespace

QByteArray Crypto::randomBytes(int count)
{
    QByteArray bytes(count, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), count) != 1)
        return QByteArray();
    return bytes;
}

QByteArray Crypto::masterKey(QString *error)
{
    const QString path = AppConfig::keyFile();
    QFile file(path);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("无法读取密钥文件 %1").arg(path);
            return QByteArray();
        }
        const QByteArray key = file.readAll();
        if (key.size() != kKeyLength) {
            if (error)
                *error = QStringLiteral("密钥文件损坏：%1").arg(path);
            return QByteArray();
        }
        return key;
    }

    if (!AppConfig::ensureConfigDir()) {
        if (error)
            *error = QStringLiteral("无法创建数据目录 %1").arg(AppConfig::dataDir());
        return QByteArray();
    }
    const QByteArray key = randomBytes(kKeyLength);
    if (key.isEmpty()) {
        if (error)
            *error = QStringLiteral("随机数生成失败");
        return QByteArray();
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法写入密钥文件 %1").arg(path);
        return QByteArray();
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(key);
    file.close();
    return key;
}

QString Crypto::encryptWithKey(const QByteArray &key, const QByteArray &plain, QString *error)
{
    if (plain.isEmpty())
        return QString();
    if (key.size() != kKeyLength) {
        if (error)
            *error = QStringLiteral("密钥长度不正确");
        return QString();
    }

    const QByteArray iv = randomBytes(kIvLength);
    if (iv.isEmpty()) {
        if (error)
            *error = QStringLiteral("IV 生成失败");
        return QString();
    }

    QByteArray cipher(plain.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    QByteArray tag(kTagLength, Qt::Uninitialized);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        if (error)
            *error = QStringLiteral("EVP_CIPHER_CTX_new 失败");
        return QString();
    }
    int len = 0;
    int total = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvLength, nullptr) == 1;
    ok = ok && EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                                  reinterpret_cast<const unsigned char *>(key.constData()),
                                  reinterpret_cast<const unsigned char *>(iv.constData())) == 1;
    ok = ok && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(cipher.data()), &len,
                                 reinterpret_cast<const unsigned char *>(plain.constData()),
                                 plain.size()) == 1;
    total = len;
    ok = ok && EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(cipher.data()) + total,
                                   &len) == 1;
    total += len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLength, tag.data()) == 1;
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        if (error)
            *error = QStringLiteral("加密失败");
        return QString();
    }
    cipher.resize(total);
    const QByteArray packed = iv + cipher + tag;
    return QString::fromLatin1(kPrefix) + QString::fromLatin1(packed.toBase64());
}

QByteArray Crypto::decryptWithKey(const QByteArray &key, const QString &encoded, QString *error)
{
    if (encoded.isEmpty())
        return QByteArray();
    if (key.size() != kKeyLength) {
        if (error)
            *error = QStringLiteral("密钥长度不正确");
        return QByteArray();
    }
    if (!encoded.startsWith(QLatin1String(kPrefix))) {
        if (error)
            *error = QStringLiteral("数据格式无法识别");
        return QByteArray();
    }
    const QByteArray packed = QByteArray::fromBase64(encoded.mid(int(sizeof(kPrefix) - 1)).toLatin1());
    if (packed.size() < kIvLength + kTagLength) {
        if (error)
            *error = QStringLiteral("数据不完整");
        return QByteArray();
    }

    const QByteArray iv = packed.left(kIvLength);
    const QByteArray tag = packed.right(kTagLength);
    const QByteArray cipher = packed.mid(kIvLength, packed.size() - kIvLength - kTagLength);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        if (error)
            *error = QStringLiteral("EVP_CIPHER_CTX_new 失败");
        return QByteArray();
    }
    QByteArray plain(cipher.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    int len = 0;
    int total = 0;
    QByteArray mutableTag = tag;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvLength, nullptr) == 1;
    ok = ok && EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                                  reinterpret_cast<const unsigned char *>(key.constData()),
                                  reinterpret_cast<const unsigned char *>(iv.constData())) == 1;
    ok = ok && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plain.data()), &len,
                                 reinterpret_cast<const unsigned char *>(cipher.constData()),
                                 cipher.size()) == 1;
    total = len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLength, mutableTag.data()) == 1;
    ok = ok && EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(plain.data()) + total,
                                   &len) == 1;
    total += len;
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        if (error)
            *error = QStringLiteral("解密失败（口令或密钥不正确）");
        return QByteArray();
    }
    plain.resize(total);
    return plain;
}

QString Crypto::encrypt(const QString &plain, QString *error)
{
    if (plain.isEmpty())
        return QString();
    const QByteArray key = masterKey(error);
    if (key.isEmpty())
        return QString();
    return encryptWithKey(key, plain.toUtf8(), error);
}

QString Crypto::decrypt(const QString &encoded, QString *error)
{
    const QByteArray key = masterKey(error);
    if (key.isEmpty())
        return QString();
    return QString::fromUtf8(decryptWithKey(key, encoded, error));
}

QByteArray Crypto::deriveKey(const QByteArray &passphrase, const QByteArray &salt, int iterations)
{
    QByteArray out(kKeyLength, Qt::Uninitialized);
    const int rc = PKCS5_PBKDF2_HMAC(passphrase.constData(), passphrase.size(),
                                     reinterpret_cast<const unsigned char *>(salt.constData()),
                                     salt.size(), iterations, EVP_sha256(), kKeyLength,
                                     reinterpret_cast<unsigned char *>(out.data()));
    if (rc != 1)
        return QByteArray();
    return out;
}

QJsonObject Crypto::wrapMasterKey(const QString &passphrase, QString *error)
{
    if (passphrase.size() < 6) {
        if (error)
            *error = QStringLiteral("恢复口令至少 6 个字符");
        return QJsonObject();
    }
    const QByteArray key = masterKey(error);
    if (key.isEmpty())
        return QJsonObject();

    const QByteArray salt = randomBytes(kSaltLength);
    const QByteArray kek = deriveKey(passphrase.toUtf8(), salt, kKdfIterations);
    if (kek.isEmpty()) {
        if (error)
            *error = QStringLiteral("口令派生失败");
        return QJsonObject();
    }
    const QString wrapped = encryptWithKey(kek, key, error);
    if (wrapped.isEmpty())
        return QJsonObject();

    QJsonObject payload;
    payload.insert(QStringLiteral("version"), 1);
    payload.insert(QStringLiteral("kdf"), QStringLiteral("pbkdf2-sha256"));
    payload.insert(QStringLiteral("iterations"), kKdfIterations);
    payload.insert(QStringLiteral("salt"), QString::fromLatin1(salt.toBase64()));
    payload.insert(QStringLiteral("wrapped"), wrapped);
    payload.insert(QStringLiteral("createdAt"),
                   QString::number(QDateTime::currentSecsSinceEpoch()));
    return payload;
}

QByteArray Crypto::unwrapMasterKey(const QJsonObject &payload, const QString &passphrase,
                                   QString *error)
{
    const QByteArray salt = QByteArray::fromBase64(
        payload.value(QStringLiteral("salt")).toString().toLatin1());
    const int iterations = payload.value(QStringLiteral("iterations")).toInt(kKdfIterations);
    const QString wrapped = payload.value(QStringLiteral("wrapped")).toString();
    if (salt.isEmpty() || wrapped.isEmpty()) {
        if (error)
            *error = QStringLiteral("恢复数据不完整");
        return QByteArray();
    }
    const QByteArray kek = deriveKey(passphrase.toUtf8(), salt, iterations);
    if (kek.isEmpty()) {
        if (error)
            *error = QStringLiteral("口令派生失败");
        return QByteArray();
    }
    return decryptWithKey(kek, wrapped, error);
}

} // namespace termdock
