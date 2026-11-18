#include "app/Theme.h"

#include <QFontDatabase>
#include <QStringList>

namespace termdock {

// 界面配色参考 QDarkStyleSheet（python-qdarkstyle）的深色方案：
// 底色 #19232D、控件 #29343D、边框 #37414B、强调色 #148CD2 / #00BCD4。
// 终端 16 色沿用 Konsole Breeze Dark 的取色，在 #19232D 上对比度足够。
Theme::Theme()
{
    m_background = QColor(0x19, 0x23, 0x2d);
    m_foreground = QColor(0xdc, 0xe3, 0xe8);
    m_cursor = QColor(0x00, 0xbc, 0xd4);
    m_selection = QColor(0x14, 0x8c, 0xd2, 0x88);
    m_bold = QColor(0xf2, 0xf6, 0xfa);

    m_palette = {
        QColor(0x23, 0x2e, 0x39), // black
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

    m_panelBg = QColor(0x19, 0x23, 0x2d);
    m_border = QColor(0x37, 0x41, 0x4b);
    m_accent = QColor(0x14, 0x8c, 0xd2);
    m_muted = QColor(0x7e, 0x87, 0x94);
    m_tabBg = QColor(0x1e, 0x29, 0x33);
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
        // 拉丁字符用等宽字体，汉字交给 CJK 字体兜底（汉字天然占两格）。
        // 按平台分开列：Windows 上没有 JetBrains/DejaVu，Linux 上也没有 Cascadia/Consolas。
        // 混在一个列表里会改变 Linux 的既有选择（装了 Cascadia 的用户等宽字体就被换掉），
        // 所以这里用条件编译而不是拼接。
#ifdef _WIN32
        font.setFamilies(QStringList{QStringLiteral("Cascadia Mono"),
                                     QStringLiteral("Consolas"),
                                     QStringLiteral("Noto Sans CJK SC"),
                                     QStringLiteral("Noto Sans Mono CJK SC"),
                                     QStringLiteral("Microsoft YaHei UI")});
#else
        font.setFamilies(QStringList{QStringLiteral("JetBrainsMono NFM"),
                                     QStringLiteral("JetBrains Mono"),
                                     QStringLiteral("DejaVu Sans Mono"),
                                     QStringLiteral("Noto Sans CJK SC"),
                                     QStringLiteral("Noto Sans Mono CJK SC")});
#endif
    } else {
        font.setFamily(m_fontFamily);
    }
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setPointSize(m_fontSize);
    // Windows 的 ClearType 在 hand 风格下抗锯齿更好；全提示（PreferFullHinting）
    // 会让小字号中文发糊
#ifdef _WIN32
    font.setHintingPreference(QFont::PreferDefaultHinting);
#else
    font.setHintingPreference(QFont::PreferFullHinting);
#endif
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
/* ===== 基础 ===== */
QWidget {
    background: #19232D;
    color: #DFE4EA;
    /* Windows 字体放在 Linux 后面：Linux 上 "Noto Sans CJK SC" 基本总能命中，
       追加不会改变现有观感；反过来把雅黑放前面会把 Linux 字体抢走。 */
    font-family: "Noto Sans CJK SC", "Source Han Sans SC", "Helvetica Neue", "Microsoft YaHei UI", "Segoe UI", sans-serif;
    font-size: 13px;
    selection-background-color: #148CD2;
    selection-color: #FFFFFF;
}
QMainWindow, QDialog { background: #19232D; }
QFrame[frameShape="4"], QFrame[frameShape="5"] { color: #37414B; }
QLabel { background: transparent; }
QLabel#hint { color: #7E8794; font-size: 12px; }
/* SFTP 路径栏：平时像一行灰字，点进去就能改 */
QLineEdit#pathEdit {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 5px;
    padding: 3px 10px;
    color: #8FA0B3;
    font-size: 12px;
    selection-background-color: #266B94;
}
QLineEdit#pathEdit:hover { color: #B6BFCE; }
QLineEdit#pathEdit:focus {
    background: #1E2933;
    border: 1px solid #148CD2;
    color: #E9EFF8;
}
QLabel#placeholder {
    color: #55606E;
    font-size: 14px;
    background: transparent;
}

/* ===== 分隔条 ===== */
QSplitter::handle { background: #19232D; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QSplitter::handle:hover { background: #148CD2; }

/* ===== 菜单栏 / 菜单 ===== */
QMenuBar { background: #19232D; border-bottom: 1px solid #232E39; }
QMenuBar::item { padding: 5px 10px; background: transparent; border-radius: 4px; }
QMenuBar::item:selected { background: #29343D; }
QMenuBar::item:pressed { background: #148CD2; color: #FFFFFF; }
QMenu {
    background: #1E2933;
    border: 1px solid #37414B;
    border-radius: 6px;
    padding: 5px;
}
QMenu::item {
    padding: 6px 26px 6px 22px;
    border-radius: 4px;
    background: transparent;
}
QMenu::item:selected { background: #148CD2; color: #FFFFFF; }
QMenu::item:disabled { color: #5B6570; }
QMenu::separator { height: 1px; background: #2A3540; margin: 5px 8px; }
QMenu::indicator { width: 14px; height: 14px; left: 6px; }

/* ===== 工具栏 / 工具按钮 ===== */
QToolBar {
    background: #19232D;
    border: 0;
    spacing: 4px;
    padding: 4px 6px;
}
QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 5px;
    padding: 4px 10px;
    color: #C9D2DC;
}
QToolButton:hover { background: #29343D; border-color: #37414B; }
QToolButton:pressed { background: #148CD2; color: #FFFFFF; }

/* ===== 状态栏 ===== */
QStatusBar { background: #151E27; border-top: 1px solid #232E39; }
QStatusBar::item { border: 0; }
QStatusBar QLabel { color: #8E97A3; }

/* ===== 标签页 ===== */
QTabWidget::pane {
    border: 1px solid #2A3540;
    border-radius: 9px;
    background: #19232D;
    top: -1px;
}
QTabWidget::tab-bar { left: 8px; top: 4px; }
QTabBar { qproperty-drawBase: 0; background: transparent; }
QTabBar::tab {
    background: transparent;
    color: #79828F;
    padding: 7px 16px;
    margin: 0px 4px 0px 0px;
    min-width: 84px;
    border: 1px solid transparent;
    border-top-left-radius: 9px;
    border-top-right-radius: 9px;
}
QTabBar::tab:hover:!selected { background: #1E2933; color: #B6BFCE; }
QTabBar::tab:selected {
    background: #19232D;
    color: #E9EFF8;
    border: 1px solid #2A3540;
    border-bottom: none;
}
QTabBar::close-button {
    subcontrol-position: right;
    image: url(:/icons/tab-close.png);
    width: 12px;
    height: 12px;
    margin-left: 6px;
    margin-right: 2px;
}
QTabBar::close-button:hover { image: url(:/icons/tab-close-hover.png); }

/* ===== 输入控件 ===== */
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QTextEdit {
    background: #1E2933;
    border: 1px solid #37414B;
    border-radius: 5px;
    padding: 5px 8px;
    color: #DFE4EA;
}
QLineEdit:hover, QComboBox:hover, QSpinBox:hover, QPlainTextEdit:hover {
    border-color: #455364;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QPlainTextEdit:focus, QTextEdit:focus {
    border-color: #148CD2;
    background: #202C37;
}
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled {
    color: #5B6570;
    background: #1A242E;
}
QComboBox::drop-down { border: 0; width: 20px; }
QComboBox::down-arrow { image: url(:/icons/chevron.png); width: 12px; height: 12px; }
QComboBox QAbstractItemView {
    background: #1E2933;
    border: 1px solid #37414B;
    selection-background-color: #148CD2;
    selection-color: #FFFFFF;
    outline: 0;
}
QSpinBox::up-button, QSpinBox::down-button { width: 16px; background: #29343D; border: 0; }

/* ===== 按钮 ===== */
QPushButton {
    background: #29343D;
    border: 1px solid #37414B;
    border-radius: 5px;
    padding: 6px 14px;
    color: #DFE4EA;
    min-height: 16px;
}
QPushButton:hover { background: #37414B; border-color: #455364; }
QPushButton:pressed { background: #148CD2; border-color: #148CD2; color: #FFFFFF; }
QPushButton:disabled { background: #202A34; color: #5B6570; border-color: #2A3540; }
QPushButton:default, QPushButton#primary {
    background: #148CD2;
    border-color: #148CD2;
    color: #FFFFFF;
    font-weight: 600;
}
QPushButton#primary:hover, QPushButton:default:hover { background: #199AE0; border-color: #199AE0; }
QPushButton#newSession {
    font-size: 17px;
    font-weight: 600;
    padding: 0px;
    border-radius: 6px;
    background: #29343D;
}
QPushButton#newSession:hover { background: #148CD2; border-color: #148CD2; color: #FFFFFF; }

/* ===== 复选框 ===== */
QCheckBox { spacing: 7px; background: transparent; }
QCheckBox::indicator {
    width: 15px;
    height: 15px;
    border: 1px solid #455364;
    border-radius: 4px;
    background: #1E2933;
}
QCheckBox::indicator:hover { border-color: #148CD2; }
QCheckBox::indicator:checked {
    background: #148CD2;
    border-color: #148CD2;
    image: url(:/icons/check.png);
}
QCheckBox::indicator:disabled { background: #202A34; border-color: #2A3540; }

/* ===== 列表 / 树 / 表格 ===== */
QListWidget, QTreeView, QTableWidget, QTableView, QListView {
    background: #19232D;
    border: 0;
    outline: 0;
    alternate-background-color: #1C2731;
}
QListWidget::item {
    border-radius: 6px;
    padding: 6px 8px;
    margin: 2px 6px;
    color: #C9D2DC;
}
QListWidget::item:hover:!selected { background: #212C36; }
QListWidget::item:selected { background: #148CD2; color: #FFFFFF; }
QTreeView::item, QTableWidget::item, QTableView::item {
    padding: 4px 8px;
    border: 0;
}
QTreeView::item:hover:!selected, QTableWidget::item:hover:!selected { background: #212C36; }
QTreeView::item:selected, QTableWidget::item:selected, QTableView::item:selected {
    background: #148CD2;
    color: #FFFFFF;
}
QHeaderView { background: #1C2731; border: 0; }
QHeaderView::section {
    background: #222D38;
    color: #9AA5B2;
    border: 0;
    border-right: 1px solid #2A3540;
    border-bottom: 1px solid #2A3540;
    padding: 5px 8px;
    font-size: 12px;
}
QHeaderView::section:hover { background: #29343D; color: #C9D2DC; }
QTableWidget, QTableView {
    background: #19232D;
    alternate-background-color: #1C2731;
    border: 1px solid #2A3540;
    border-radius: 6px;
    gridline-color: #232E39;
    selection-background-color: #148CD2;
    selection-color: #FFFFFF;
}
QTableCornerButton::section { background: #222D38; border: 0; }

/* ===== 滚动条 ===== */
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 2px 2px 2px 0px;
}
QScrollBar::handle:vertical {
    background: #3B4754;
    border-radius: 4px;
    min-height: 28px;
}
QScrollBar::handle:vertical:hover { background: #148CD2; }
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 0px 2px 2px 2px;
}
QScrollBar::handle:horizontal {
    background: #3B4754;
    border-radius: 4px;
    min-width: 28px;
}
QScrollBar::handle:horizontal:hover { background: #148CD2; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QAbstractScrollArea::corner { background: transparent; }

/* ===== 提示 / 消息框 ===== */
QToolTip {
    background: #29343D;
    color: #DFE4EA;
    border: 1px solid #455364;
    border-radius: 5px;
    padding: 4px 7px;
}
QMessageBox { background: #19232D; }
QMessageBox QLabel { color: #DFE4EA; }
)");
}

} // namespace termdock
