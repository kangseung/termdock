#pragma once

#include <QMetaType>
#include <QString>
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

using SftpEntryList = QVector<SftpEntry>;

} // namespace termdock

Q_DECLARE_METATYPE(termdock::SftpEntryList)
