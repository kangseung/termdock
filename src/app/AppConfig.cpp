#include "app/AppConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>

// QStandardPaths 在 Linux 下解析为 $XDG_CONFIG_HOME/termdock，
// macOS 为 ~/Library/Preferences/termdock，Windows 为 %APPDATA%/termdock。
namespace termdock {

namespace {
constexpr char kSettingsKey[] = "storage/dataDir";
QString g_overrideDir;
} // namespace

QString AppConfig::executableDir()
{
    if (!QCoreApplication::instance())
        return QDir::currentPath();
    return QCoreApplication::applicationDirPath();
}

QString AppConfig::defaultDataDir()
{
    // 优先跟软件放一起（便携式），目录不可写时退回用户配置目录
    const QString beside = executableDir() + QStringLiteral("/termdock-data");
    if (isDirWritable(executableDir()))
        return QDir::cleanPath(beside);

    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.config");
    return QDir::cleanPath(base + QStringLiteral("/termdock"));
}

bool AppConfig::isDirWritable(const QString &path)
{
    if (path.isEmpty())
        return false;
    QDir dir(path);
    if (!dir.exists())
        return dir.mkpath(path);
    QFile probe(dir.filePath(QStringLiteral(".termdock-write-test")));
    if (!probe.open(QIODevice::WriteOnly))
        return false;
    probe.close();
    probe.remove();
    return true;
}

QString AppConfig::dataDir()
{
    if (!g_overrideDir.isEmpty())
        return g_overrideDir;

    const QByteArray fromEnv = qgetenv("TERMDOCK_DATA_DIR");
    if (!fromEnv.isEmpty())
        return QDir::cleanPath(QString::fromUtf8(fromEnv));

    const QString stored = QSettings().value(QLatin1String(kSettingsKey)).toString();
    if (!stored.isEmpty())
        return QDir::cleanPath(stored);

    return defaultDataDir();
}

bool AppConfig::hasStoredDataDir()
{
    const QByteArray fromEnv = qgetenv("TERMDOCK_DATA_DIR");
    if (!fromEnv.isEmpty())
        return true;
    return !QSettings().value(QLatin1String(kSettingsKey)).toString().isEmpty();
}

bool AppConfig::setDataDir(const QString &path)
{
    if (path.isEmpty())
        return false;
    const QString clean = QDir::cleanPath(path);
    QDir dir;
    if (!dir.mkpath(clean))
        return false;
    if (!isDirWritable(clean))
        return false;
    QSettings().setValue(QLatin1String(kSettingsKey), clean);
    g_overrideDir.clear();
    return true;
}

void AppConfig::setDataDirOverride(const QString &path)
{
    g_overrideDir = path.isEmpty() ? QString() : QDir::cleanPath(path);
}

QString AppConfig::sessionsFile()
{
    return dataDir() + QStringLiteral("/sessions.json");
}

QString AppConfig::keyFile()
{
    return dataDir() + QStringLiteral("/key");
}

QString AppConfig::knownHostsFile()
{
    return dataDir() + QStringLiteral("/known_hosts.json");
}

bool AppConfig::ensureConfigDir()
{
    QDir dir;
    return dir.mkpath(dataDir());
}

} // namespace termdock
