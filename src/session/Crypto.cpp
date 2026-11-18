#include "session/Crypto.h"

#include "app/AppConfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace termdock {

namespace {
constexpr int kKeyLength = 32;
constexpr int kIvLength = 12;
constexpr int kTagLength = 16;
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
            *error = QStringLiteral("无法创建配置目录 %1").arg(AppConfig::configDir());
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

QString Crypto::encrypt(const QString &plain, QString *error)
{
    if (plain.isEmpty())
        return QString();
    const QByteArray key = masterKey(error);
    if (key.isEmpty())
        return QString();

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
    const QByteArray plainBytes = plain.toUtf8();
    ok = ok && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(cipher.data()), &len,
                                 reinterpret_cast<const unsigned char *>(plainBytes.constData()),
                                 plainBytes.size()) == 1;
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

QString Crypto::decrypt(const QString &encoded, QString *error)
{
    if (encoded.isEmpty())
        return QString();
    if (!encoded.startsWith(QLatin1String(kPrefix))) {
        if (error)
            *error = QStringLiteral("密码格式无法识别");
        return QString();
    }
    const QByteArray packed = QByteArray::fromBase64(encoded.mid(int(sizeof(kPrefix) - 1)).toLatin1());
    if (packed.size() < kIvLength + kTagLength) {
        if (error)
            *error = QStringLiteral("密码数据不完整");
        return QString();
    }
    const QByteArray key = masterKey(error);
    if (key.isEmpty())
        return QString();

    const QByteArray iv = packed.left(kIvLength);
    const QByteArray tag = packed.right(kTagLength);
    const QByteArray cipher = packed.mid(kIvLength, packed.size() - kIvLength - kTagLength);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        if (error)
            *error = QStringLiteral("EVP_CIPHER_CTX_new 失败");
        return QString();
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
            *error = QStringLiteral("密码解密失败（密钥可能已变更）");
        return QString();
    }
    plain.resize(total);
    return QString::fromUtf8(plain);
}

} // namespace termdock
