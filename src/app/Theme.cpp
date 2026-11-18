#include "app/Theme.h"

#include <QFontDatabase>
#include <QStringList>

namespace termdock {

// 构造函数里统一给所有颜色赋值，避免分散到各个 widget 里硬编码。
Theme::Theme()
{
    // 参考 Konsole "Breeze Dark" 的取色，稍作提亮以便长时间阅读。
    m_background = QColor(0x1c, 0x1f, 0x26);
    m_foreground = QColor(0xd6, 0xdb, 0xe4);
    m_cursor = QColor(0x8f, 0xbc, 0xf8);
    m_selection = QColor(0x3a, 0x4d, 0x6b, 0xcc);
    m_bold = QColor(0xf2, 0xf5, 0xfa);

    m_palette = {
        QColor(0x28, 0x2c, 0x34), // black
        QColor(0xe0, 0x6c, 0x75), // red
        QColor(0x9e, 0xd0, 0x72), // green
        QColor(0xe5, 0xc0, 0x7b), // yellow
        QColor(0x7a, 0xa2, 0xf7), // blue
        QColor(0xc4, 0x9c, 0xea), // magenta
        QColor(0x62, 0xc5, 0xc0), // cyan
        QColor(0xc8, 0xcf, 0xdb), // white
        QColor(0x4a, 0x50, 0x5c), // bright black
        QColor(0xf0, 0x8a, 0x92), // bright red
        QColor(0xb2, 0xe3, 0x8a), // bright green
        QColor(0xf2, 0xd3, 0x93), // bright yellow
        QColor(0x9a, 0xba, 0xff), // bright blue
        QColor(0xd7, 0xb4, 0xf5), // bright magenta
        QColor(0x84, 0xdc, 0xd6), // bright cyan
        QColor(0xf4, 0xf7, 0xfb), // bright white
    };

    m_panelBg = QColor(0x16, 0x18, 0x1e);
    m_border = QColor(0x2c, 0x31, 0x3a);
    m_accent = QColor(0x7a, 0xa2, 0xf7);
    m_muted = QColor(0x7d, 0x86, 0x96);
    m_tabBg = QColor(0x1a, 0x1d, 0x23);
}

const Theme &Theme::instance()
{
    static Theme theme;
    return theme;
}

Theme &Theme::mutableInstance()
{
    return const_cast<Theme &>(instance());
}

QFont Theme::terminalFont() const
{
    QFont font;
    if (m_fontFamily.isEmpty()) {
        // 拉丁字符用等宽字体，汉字交给 CJK 字体兜底（汉字天然占两格）
        font.setFamilies(QStringList{QStringLiteral("JetBrainsMono NFM"),
                                     QStringLiteral("JetBrains Mono"),
                                     QStringLiteral("DejaVu Sans Mono"),
                                     QStringLiteral("Noto Sans CJK SC"),
                                     QStringLiteral("Noto Sans Mono CJK SC")});
    } else {
        font.setFamily(m_fontFamily);
    }
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setPointSize(m_fontSize);
    font.setHintingPreference(QFont::PreferFullHinting);
    return font;
}

void Theme::setTerminalFontFamily(const QString &family)
{
    if (!family.isEmpty())
        m_fontFamily = family;
}

void Theme::setTerminalFontSize(int pt)
{
    if (pt >= 6 && pt <= 40)
        m_fontSize = pt;
}

QString Theme::styleSheet() const
{
    return QStringLiteral(R"(
QWidget {
    background: #16181e;
    color: #d6dbe4;
    font-family: "Noto Sans CJK SC", "Source Han Sans SC", sans-serif;
    font-size: 13px;
}
QSplitter::handle { background: #2c313a; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QTabWidget::pane { border: 0; background: #1c1f26; }
QTabBar::tab {
    background: #1a1d23;
    color: #8b93a3;
    padding: 7px 14px;
    margin-right: 1px;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
}
QTabBar::tab:selected { background: #1c1f26; color: #e7ecf5; }
QTabBar::tab:hover:!selected { background: #22262e; color: #c8cfdb; }
QTabBar::close-button { subcontrol-position: right; }
QLineEdit, QComboBox, QSpinBox, QPlainTextEdit {
    background: #12141a;
    border: 1px solid #2c313a;
    border-radius: 6px;
    padding: 5px 8px;
    selection-background-color: #3a4d6b;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 1px solid #7aa2f7; }
QPushButton {
    background: #232830;
    border: 1px solid #2c313a;
    border-radius: 6px;
    padding: 6px 14px;
    color: #d6dbe4;
}
QPushButton:hover { background: #2b313b; border-color: #3a4150; }
QPushButton:pressed { background: #1d222a; }
QPushButton#primary { background: #7aa2f7; color: #10131a; border: 0; font-weight: 600; }
QPushButton#primary:hover { background: #8fb4ff; }
QListWidget, QTreeView {
    background: #16181e;
    border: 0;
    outline: 0;
}
QListWidget::item { border-radius: 6px; padding: 2px; margin: 2px 4px; }
QListWidget::item:selected { background: #263043; }
QListWidget::item:hover:!selected { background: #1d222a; }
QTreeView::item { height: 24px; }
QTreeView::item:selected { background: #263043; }
QHeaderView::section {
    background: #191c22;
    color: #8b93a3;
    border: 0;
    border-bottom: 1px solid #2c313a;
    padding: 4px 6px;
}
QToolBar { background: #16181e; border: 0; padding: 2px; }
QToolButton { border-radius: 5px; padding: 4px; }
QToolButton:hover { background: #262b34; }
QToolButton:pressed { background: #1d222a; }
QMenu { background: #1e222a; border: 1px solid #2c313a; padding: 4px; }
QMenu::item { padding: 5px 22px; border-radius: 4px; }
QMenu::item:selected { background: #263043; }
QStatusBar { background: #16181e; color: #8b93a3; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #333a46; border-radius: 5px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: #414a59; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QScrollBar:horizontal { background: transparent; height: 10px; }
QScrollBar::handle:horizontal { background: #333a46; border-radius: 5px; min-width: 24px; }
QCheckBox { spacing: 6px; }
QDialog { background: #16181e; }
QLabel#hint { color: #7d8696; }
)");
}

} // namespace termdock
