#include "app/AppConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

// QStandardPaths 在 Linux 下解析为 $XDG_CONFIG_HOME/termdock，
// macOS 为 ~/Library/Preferences/termdock，Windows 为 %APPDATA%/termdock。
namespace termdock {

namespace {
constexpr char kSettingsKey[] = "storage/dataDir";
QString g_overrideDir;

// 临时目录里的数据重启就没了，绝不能当成持久的数据目录
bool isInsideTempDir(const QString &path)
{
    const QString tmp = QDir::cleanPath(QDir::tempPath());
    const QString clean = QDir::cleanPath(path);
    return clean == tmp || clean.startsWith(tmp + QLatin1Char('/'));
}

// 不可当持久存放位置：临时目录（重启清空）、CMake 构建目录（重新构建/清目录就没了）
bool looksVolatile(const QString &dir)
{
    if (isInsideTempDir(dir))
        return true;
    return QFile::exists(dir + QStringLiteral("/CMakeCache.txt"));
}

// 把误存到临时目录的数据搬到正式目录（只搬不删，缺什么补什么）
void migrateFrom(const QString &from, const QString &to)
{
    if (from.isEmpty() || to.isEmpty() || from == to)
        return;
    QDir target(to);
    if (!target.exists() && !target.mkpath(to))
        return;
    const QStringList files = {QStringLiteral("key"), QStringLiteral("sessions.json"),
                               QStringLiteral("known_hosts.json"), QStringLiteral("recovery.json")};
    for (const QString &name : files) {
        const QString source = from + QLatin1Char('/') + name;
        const QString destination = to + QLatin1Char('/') + name;
        if (!QFile::exists(source) || QFile::exists(destination))
            continue;
        if (QFile::copy(source, destination)) {
            QFile::setPermissions(destination, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            qWarning("termdock: 已把 %s 迁移到 %s", qPrintable(source), qPrintable(destination));
        }
    }
}
} // namespace

QString AppConfig::executableDir()
{
    if (!QCoreApplication::instance())
        return QDir::currentPath();
    return QCoreApplication::applicationDirPath();
}

QString AppConfig::defaultDataDir()
{
    // 优先跟软件放一起（便携式）；但临时目录、开发构建目录不算「软件目录」，
    // 那些位置重启/重新构建就没了，绝不能放数据。
    // 装在 bin/ 目录里的情况（如 ~/.local/bin）也不放旁边：bin 是程序目录，数据该在配置目录。
    const QString exeDir = executableDir();
    const QString beside = exeDir + QStringLiteral("/termdock-data");
    const bool installedInBin = QFileInfo(exeDir).fileName() == QStringLiteral("bin");
    if (!installedInBin && isDirWritable(exeDir) && !looksVolatile(exeDir))
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
    if (!fromEnv.isEmpty()) {
        const QString envDir = QDir::cleanPath(QString::fromUtf8(fromEnv));
        // 环境变量同样不接受临时目录：那里的 key/sessions 重启就没了
        if (!isInsideTempDir(envDir))
            return envDir;
        qWarning("termdock: 忽略临时目录里的 TERMDOCK_DATA_DIR=%s", qPrintable(envDir));
    }

    const QString stored = QSettings().value(QLatin1String(kSettingsKey)).toString();
    if (!stored.isEmpty()) {
        const QString clean = QDir::cleanPath(stored);
        if (!isInsideTempDir(clean))
            return clean;
        // 历史版本/测试曾把数据目录写到 /tmp（重启即丢），这里直接纠正
        const QString fallback = defaultDataDir();
        qWarning("termdock: 忽略临时目录中的数据目录 %s，改用 %s", qPrintable(clean),
                 qPrintable(fallback));
        migrateFrom(clean, fallback);
        return fallback;
    }

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
    if (isInsideTempDir(clean)) {
        qWarning("termdock: 拒绝把数据目录设到临时目录 %s（重启会丢）", qPrintable(clean));
        return false;
    }
    if (!isDirWritable(clean))
        return false;
    QSettings().setValue(QLatin1String(kSettingsKey), clean);
    g_overrideDir.clear();
    return true;
}

void AppConfig::setDataDirOverride(const QString &path)
{
    if (!path.isEmpty() && isInsideTempDir(path))
        qWarning("termdock: 数据目录在临时目录 %s，进程退出后数据会丢", qPrintable(path));
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
