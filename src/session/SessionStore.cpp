#include "session/SessionStore.h"

#include "app/AppConfig.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace termdock {

namespace {

bool readJson(const QString &path, QJsonObject *out, QString *error)
{
    QFile file(path);
    if (!file.exists())
        return false;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法打开 %1：%2").arg(path, file.errorString());
        return false;
    }
    const QByteArray raw = file.readAll();
    if (raw.trimmed().isEmpty())
        return false;
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("%1 解析失败：%2").arg(path, parseError.errorString());
        return false;
    }
    *out = doc.object();
    return true;
}

} // namespace

SessionList SessionStore::load(QString *error)
{
    SessionList sessions;
    const QString path = AppConfig::sessionsFile();
    QJsonObject root;
    QString readError;
    if (!readJson(path, &root, &readError)) {
        if (!readError.isEmpty()) {
            if (error)
                *error = readError;
            return sessions;
        }
        return sessions; // 文件不存在：返回空列表
    }

    const QJsonArray array = root.value(QStringLiteral("sessions")).toArray();
    sessions.reserve(array.size());
    int decryptionFailures = 0;
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        bool failed = false;
        const Session session = Session::fromJson(value.toObject(), &failed);
        if (failed)
            ++decryptionFailures;
        sessions.append(session);
    }
    if (decryptionFailures > 0 && error)
        *error = QStringLiteral("有 %1 条会话的密码无法解密（密钥文件可能被替换），请重新编辑这些会话。")
                     .arg(decryptionFailures);
    return sessions;
}

bool SessionStore::save(const SessionList &sessions, QString *error)
{
    if (!AppConfig::ensureConfigDir()) {
        if (error)
            *error = QStringLiteral("无法创建配置目录：%1").arg(AppConfig::configDir());
        return false;
    }

    QJsonArray array;
    for (const Session &session : sessions)
        array.append(session.toJson());
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("sessions"), array);

    const QString path = AppConfig::sessionsFile();
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入 %1：%2").arg(path, file.errorString());
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("保存 %1 失败：%2").arg(path, file.errorString());
        return false;
    }
    return true;
}

QString SessionStore::knownFingerprint(const QString &host, int port)
{
    QJsonObject root;
    if (!readJson(AppConfig::knownHostsFile(), &root, nullptr))
        return QString();
    const QString key = QStringLiteral("%1:%2").arg(host).arg(port);
    return root.value(key).toObject().value(QStringLiteral("fingerprint")).toString();
}

void SessionStore::rememberFingerprint(const QString &host, int port, const QString &fingerprint)
{
    QJsonObject root;
    readJson(AppConfig::knownHostsFile(), &root, nullptr);
    QJsonObject entry;
    entry.insert(QStringLiteral("fingerprint"), fingerprint);
    entry.insert(QStringLiteral("seenAt"),
                 QString::number(QDateTime::currentSecsSinceEpoch()));
    root.insert(QStringLiteral("%1:%2").arg(host).arg(port), entry);

    if (!AppConfig::ensureConfigDir())
        return;
    QSaveFile file(AppConfig::knownHostsFile());
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

void SessionStore::forgetFingerprint(const QString &host, int port)
{
    QJsonObject root;
    if (!readJson(AppConfig::knownHostsFile(), &root, nullptr))
        return;
    root.remove(QStringLiteral("%1:%2").arg(host).arg(port));
    QSaveFile file(AppConfig::knownHostsFile());
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

} // namespace termdock
