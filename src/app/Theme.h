#pragma once

#include <QColor>
#include <QFont>
#include <QString>
#include <vector>

// 主题集中管理：终端 16 色 + 界面配色 + QSS，改这里即可换皮。
namespace termdock {

// Konsole 风格的暗色主题。终端 16 色调色板 + 界面配色都集中在这里，
// 后续想换主题只需要改这个文件里的默认值。
class Theme {
public:
    static const Theme &instance();
    static Theme &mutableInstance(); // 用于运行时调整字号

    // 终端配色
    QColor background() const { return m_background; }
    QColor foreground() const { return m_foreground; }
    QColor cursor() const { return m_cursor; }
    QColor selection() const { return m_selection; }
    QColor boldText() const { return m_bold; }
    const std::vector<QColor> &palette() const { return m_palette; }

    // 界面配色
    QColor panelBackground() const { return m_panelBg; }
    QColor panelBorder() const { return m_border; }
    QColor accent() const { return m_accent; }
    QColor mutedText() const { return m_muted; }
    QColor tabBackground() const { return m_tabBg; }

    QFont terminalFont() const;
    QString styleSheet() const;

    void setTerminalFontFamily(const QString &family);
    void setTerminalFontSize(int pt);
    QString terminalFontFamily() const { return m_fontFamily; }
    int terminalFontSize() const { return m_fontSize; }

private:
    Theme();

    QColor m_background;
    QColor m_foreground;
    QColor m_cursor;
    QColor m_selection;
    QColor m_bold;
    std::vector<QColor> m_palette;

    QColor m_panelBg;
    QColor m_border;
    QColor m_accent;
    QColor m_muted;
    QColor m_tabBg;

    QString m_fontFamily;
    int m_fontSize = 11;
};

} // namespace termdock
