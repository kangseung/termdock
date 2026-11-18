#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

namespace termdock {

struct SftpEntry {
    QString name;
    QString path;
    bool isDir = false;
    bool isSymlink = false;
    qint64 size = 0;
    qint64 mtime = 0;
    quint32 mode = 0;

    QString permissionsText() const
    {
        // 高位掩码用十六进制表示，避免八进制字面量的可移植性争议
        const quint32 type = mode & 0xF000u;
        QChar kind = QLatin1Char('-');
        if (type == 0x4000u)
            kind = QLatin1Char('d');
        else if (type == 0xA000u)
            kind = QLatin1Char('l');

        QString text(kind);
        const char *symbols = "rwx";
        for (int shift = 8; shift >= 0; shift -= 3) {
            for (int bit = 2; bit >= 0; --bit) {
                const bool set = ((mode >> (shift + bit)) & 1u) != 0;
                text.append(QLatin1Char(set ? symbols[2 - bit] : '-'));
            }
        }
        return text;
    }
};

// 远端路径是 POSIX 语义，绝不能用 QFileInfo 解析：Windows 上它会把文件名里的
// 反斜杠当成分隔符（Linux 允许文件名含 '\'），还会把 "a:b" 当成盘符，
// 于是"刷新哪个目录"和"上传到哪"都会算错。
namespace remotepath {

inline QString parent(const QString &path)
{
    if (path.isEmpty())
        return QString();
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash < 0)
        return QStringLiteral(".");
    if (slash == 0)
        return QStringLiteral("/"); // 根目录的父目录还是根，不能返回空
    return path.left(slash);
}

inline QString baseName(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? path : path.mid(slash + 1);
}

inline QString join(const QString &dir, const QString &name)
{
    if (dir.isEmpty())
        return name;
    if (dir.endsWith(QLatin1Char('/')))
        return dir + name;
    return dir + QLatin1Char('/') + name;
}

// POSIX 语义的路径规范化：处理 "."、".." 和重复斜杠，但绝不把 '\' 当分隔符。
// QDir::cleanPath 在 Windows 上会把反斜杠当分隔符，Linux 文件名里的 '\' 就被拆掉了。
inline QString normalize(const QString &path)
{
    if (path.isEmpty())
        return path;
    const bool absolute = path.startsWith(QLatin1Char('/'));
    QStringList parts;
    const QStringList rawParts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : rawParts) {
        if (part == QLatin1String("."))
            continue;
        if (part == QLatin1String("..")) {
            if (!parts.isEmpty() && parts.last() != QLatin1String(".."))
                parts.removeLast();
            else if (!absolute)
                parts.append(part);
            continue;
        }
        parts.append(part);
    }
    const QString joined = parts.join(QLatin1Char('/'));
    if (absolute)
        return QLatin1Char('/') + joined;
    return joined.isEmpty() ? QStringLiteral(".") : joined;
}

} // namespace remotepath

using SftpEntryList = QVector<SftpEntry>;

} // namespace termdock

Q_DECLARE_METATYPE(termdock::SftpEntryList)
