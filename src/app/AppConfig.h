#pragma once

#include <QString>

namespace termdock {


// 所有运行期路径都从这里派生：~/.config/termdock/（QStandardPaths::AppConfigLocation）
class AppConfig {
public:
    static QString configDir();
    static QString sessionsFile();
    static QString keyFile();
    static QString knownHostsFile();
    static bool ensureConfigDir();
};

} // namespace termdock
