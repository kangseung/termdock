#pragma once

#include <QString>

namespace termdock {

// 所有运行期数据（会话 / 密钥 / 主机指纹）的落地目录。
// 首次启动由用户在 DataDirDialog 里选择，默认是软件自身目录下的 termdock-data，
// 这样整个软件目录可以直接拷到 U 盘或另一台机器继续用。
class AppConfig {
public:
    static QString executableDir();
    static QString defaultDataDir();

    // 解析顺序：内存覆盖 → 环境变量 TERMDOCK_DATA_DIR → 配置文件记录 → 默认目录
    static QString dataDir();
    static QString configDir() { return dataDir(); }

    static bool hasStoredDataDir();
    static bool setDataDir(const QString &path);      // 建目录 + 写入配置
    static void setDataDirOverride(const QString &path); // 仅本次进程生效（自测用）
    static bool isDirWritable(const QString &path);

    static QString sessionsFile();
    static QString keyFile();
    static QString knownHostsFile();
    static bool ensureConfigDir();
};

} // namespace termdock
