#include "session/Recovery.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "app/AppConfig.h"
#include "session/Crypto.h"

namespace termdock {

QString Recovery::filePath()
{
    return AppConfig::dataDir() + QStringLiteral("/recovery.json");
}

bool Recovery::enabled()
{
    return QFile::exists(filePath());
}

bool Recovery::enable(const QString &passphrase, QString *error)
{
    if (!AppConfig::ensureConfigDir()) {
        if (error)
            *error = QStringLiteral("无法创建数据目录");
        return false;
    }
    const QJsonObject payload = Crypto::wrapMasterKey(passphrase, error);
    if (payload.isEmpty())
        return false;

    QFile file(filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法写入 %1").arg(filePath());
        return false;
    }
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Indented);
    const bool written = file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        && file.write(body) == body.size() && file.flush();
    file.close();
    if (!written) {
        QFile::remove(filePath());
        if (error)
            *error = QStringLiteral("恢复数据写入失败（已清理半成品）：%1").arg(filePath());
        return false;
    }
    return true;
}

bool Recovery::disable()
{
    return QFile::remove(filePath()) || !QFile::exists(filePath());
}

bool Recovery::unlock(const QString &passphrase, QByteArray *masterKey, QString *error)
{
    QFile file(filePath());
    if (!file.exists()) {
        if (error)
            *error = QStringLiteral("未开启恢复保险");
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取 %1").arg(filePath());
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("恢复数据损坏：%1").arg(parseError.errorString());
        return false;
    }
    const QByteArray key = Crypto::unwrapMasterKey(doc.object(), passphrase, error);
    if (key.isEmpty())
        return false;
    if (masterKey)
        *masterKey = key;
    return true;
}

} // namespace termdock
