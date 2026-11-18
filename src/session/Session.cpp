#include "session/Session.h"

#include "session/Crypto.h"

#include <QJsonValue>
#include <QUuid>

namespace termdock {

QString Session::newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

Session Session::createEmpty()
{
    Session session;
    session.id = newId();
    session.name = QStringLiteral("新会话");
    session.port = 22;
    session.user = QStringLiteral("root");
    session.color = QStringLiteral("#7aa2f7");
    return session;
}

QString Session::displayTarget() const
{
    return QStringLiteral("%1@%2:%3").arg(user, host).arg(port);
}

QString Session::displaySubtitle() const
{
    if (!note.isEmpty())
        return QStringLiteral("%1 · %2").arg(displayTarget(), note);
    return displayTarget();
}

QJsonObject Session::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), id);
    object.insert(QStringLiteral("name"), name);
    object.insert(QStringLiteral("host"), host);
    object.insert(QStringLiteral("port"), port);
    object.insert(QStringLiteral("user"), user);
    object.insert(QStringLiteral("note"), note);
    object.insert(QStringLiteral("color"), color);
    if (!password.isEmpty()) {
        const QString encrypted = Crypto::encrypt(password);
        if (!encrypted.isEmpty())
            object.insert(QStringLiteral("password"), encrypted);
    } else if (!passwordCipher.isEmpty()) {
        // 解不开就原样留着：主密钥找回来时还能恢复，不能被一次保存清空
        object.insert(QStringLiteral("password"), passwordCipher);
    }
    return object;
}

Session Session::fromJson(const QJsonObject &object, bool *passwordDecryptFailed)
{
    Session session;
    session.id = object.value(QStringLiteral("id")).toString();
    if (session.id.isEmpty())
        session.id = newId();
    session.name = object.value(QStringLiteral("name")).toString();
    session.host = object.value(QStringLiteral("host")).toString();
    session.port = object.value(QStringLiteral("port")).toInt(22);
    if (session.port <= 0 || session.port > 65535)
        session.port = 22;
    session.user = object.value(QStringLiteral("user")).toString();
    session.note = object.value(QStringLiteral("note")).toString();
    session.color = object.value(QStringLiteral("color")).toString();
    if (session.color.isEmpty())
        session.color = QStringLiteral("#7aa2f7");

    if (passwordDecryptFailed)
        *passwordDecryptFailed = false;
    const QString encrypted = object.value(QStringLiteral("password")).toString();
    if (!encrypted.isEmpty()) {
        session.passwordCipher = encrypted;
        QString error;
        session.password = Crypto::decrypt(encrypted, &error);
        if (session.password.isEmpty() && passwordDecryptFailed)
            *passwordDecryptFailed = true;
    }
    if (session.name.isEmpty())
        session.name = session.host;
    return session;
}

} // namespace termdock
