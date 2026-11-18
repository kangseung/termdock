#include "app/AppConfig.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

// QStandardPaths 在 Linux 下解析为 $XDG_CONFIG_HOME/termdock，
// macOS 为 ~/Library/Preferences/termdock，Windows 为 %APPDATA%/termdock。

// 说明：本文件只负责路径解析，不涉及任何 IO，便于单测时替换。
namespace termdock {

QString AppConfig::configDir()
{
    // 用 GenericConfigLocation 再拼固定目录名，避免 QStandardPaths 的
    // AppConfigLocation 因 org/app 名字变化而产生 ~/.config/termdock/termdock 这种嵌套。
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.config");
    return base + QStringLiteral("/termdock");
}

QString AppConfig::sessionsFile()
{
    return configDir() + QStringLiteral("/sessions.json");
}

QString AppConfig::keyFile()
{
    return configDir() + QStringLiteral("/key");
}

QString AppConfig::knownHostsFile()
{
    return configDir() + QStringLiteral("/known_hosts.json");
}

bool AppConfig::ensureConfigDir()
{
    QDir dir;
    return dir.mkpath(configDir());
}

} // namespace termdock
