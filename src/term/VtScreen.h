#pragma once

#include <QColor>
#include <QString>
#include <deque>
#include <functional>
#include <vector>

namespace termdock {

enum VtAttr : quint16 {
    AttrBold = 0x0001,
    AttrDim = 0x0002,
    AttrItalic = 0x0004,
    AttrUnderline = 0x0008,
    AttrBlink = 0x0010,
    AttrReverse = 0x0020,
    AttrHidden = 0x0040,
    AttrStrike = 0x0080,
    AttrWide = 0x0100,
    AttrWideTail = 0x0200,
};

// 颜色压缩成 32bit：0 = 默认色，(1<<24)|idx = 256 色，(2<<24)|rgb = 真彩。
struct VtColor {
    quint32 v = 0;

    static VtColor def() { return VtColor{0}; }
    static VtColor indexed(int idx) { return VtColor{quint32(1u << 24) | quint32(idx & 0xFF)}; }
    static VtColor rgb(quint32 rgbv) { return VtColor{quint32(2u << 24) | (rgbv & 0xFFFFFFu)}; }

    bool isDefault() const { return v == 0; }
    bool isIndexed() const { return (v >> 24) == 1u; }
    bool isRgb() const { return (v >> 24) == 2u; }
    int index() const { return int(v & 0xFFu); }
    quint32 rgbValue() const { return v & 0xFFFFFFu; }
    QColor resolve(const std::vector<QColor> &palette, const QColor &fallback) const;
    bool operator==(const VtColor &o) const { return v == o.v; }
    bool operator!=(const VtColor &o) const { return v != o.v; }
};

struct VtCell {
    char32_t ch = U' ';
    VtColor fg;
    VtColor bg;
    quint16 attr = 0;
};

struct VtPen {
    VtColor fg;
    VtColor bg;
    quint16 attr = 0;
};

struct VtLine {
    std::vector<VtCell> cells;
    bool wrapped = false; // 该行是自动换行产生的续行（复制时不上换行符）
};

class VtScreen {
public:
    explicit VtScreen(int cols = 80, int rows = 24, int scrollbackLimit = 8000);

    // ---- 几何 ----
    int cols() const { return m_cols; }
    int rows() const { return m_rows; }
    void resize(int cols, int rows);

    // ---- 画笔 ----
    VtPen &pen() { return current().pen; }
    const VtPen &pen() const { return current().pen; }
    void resetPen();

    // ---- 光标 ----
    int cursorRow() const { return current().cy; }
    int cursorCol() const { return current().cx; }
    void setCursor(int row, int col);
    bool cursorVisible() const { return m_cursorVisible; }
    void setCursorVisible(bool visible) { m_cursorVisible = visible; }
    bool insertMode() const { return m_insertMode; }
    void setInsertMode(bool on) { m_insertMode = on; }

    // ---- 模式 ----
    bool autoWrap() const { return m_autoWrap; }
    void setAutoWrap(bool on) { m_autoWrap = on; }
    bool originMode() const { return m_originMode; }
    void setOriginMode(bool on);
    bool applicationCursorKeys() const { return m_appCursorKeys; }
    void setApplicationCursorKeys(bool on) { m_appCursorKeys = on; }
    bool applicationKeypad() const { return m_appKeypad; }
    void setApplicationKeypad(bool on) { m_appKeypad = on; }
    bool bracketedPaste() const { return m_bracketedPaste; }
    void setBracketedPaste(bool on) { m_bracketedPaste = on; }

    // ---- 滚动区域 ----
    void setScrollRegion(int top, int bottom);
    int scrollTop() const { return m_scrollTop; }
    int scrollBottom() const { return m_scrollBottom; }

    // ---- 写入 ----
    void putChar(char32_t ch);
    void lineFeed();
    void reverseIndex();
    void carriageReturn();
    void backspace();
    void tab(int count = 1);
    void backTab(int count = 1);

    // ---- 移动 ----
    void moveUp(int n);
    void moveDown(int n);
    void moveLeft(int n);
    void moveRight(int n);
    void moveToRow(int row);
    void moveToCol(int col);
    void moveToLineStart();
    void nextLine(int n);
    void prevLine(int n);

    // ---- 擦除 / 编辑 ----
    void eraseInDisplay(int mode);
    void eraseInLine(int mode);
    void eraseChars(int n);
    void insertChars(int n);
    void deleteChars(int n);
    void insertLines(int n);
    void deleteLines(int n);
    void scrollUpRegion(int n);
    void scrollDownRegion(int n);

    // ---- 保存 / 恢复 ----
    void saveCursor();
    void restoreCursor();

    // ---- 备用屏 ----
    void enterAltScreen();
    void exitAltScreen();
    bool inAltScreen() const { return m_altActive; }

    // ---- 复位 / 标题 ----
    void fullReset();
    void setTitle(const QString &title);
    QString title() const { return m_title; }

    // ---- 滚回 ----
    int scrollbackCount() const { return int(current().scrollback.size()); }
    int scrollOffset() const { return m_scrollOffset; }
    void setScrollOffset(int offset);
    void scrollToBottom() { m_scrollOffset = 0; }
    int totalLines() const { return scrollbackCount() + m_rows; }

    // visualRow: 0 = 当前视图顶部（含滚回偏移），越大越靠下
    const VtCell &cellAt(int visualRow, int col) const;
    // 绝对行号：0..scrollbackCount()-1 为滚回，之后为屏幕
    const VtCell &cellAtAbsolute(int line, int col) const;
    int absoluteLineOf(int visualRow) const;
    bool lineIsWrapped(int absoluteLine) const;
    QString textInRange(int startLine, int startCol, int endLine, int endCol) const;

    static int charWidth(char32_t ch);

    // ---- 重绘通知 ----
    void setDamageCallback(std::function<void()> cb) { m_onDamage = std::move(cb); }
    void damage() const { if (m_onDamage) m_onDamage(); }
    quint64 revision() const { return m_revision; }
    void touch() const { ++const_cast<VtScreen *>(this)->m_revision; }

private:
    struct Buffer {
        std::vector<VtLine> lines;      // 恰好 rows 行
        std::deque<VtLine> scrollback;  // 滚出屏幕的历史行
        VtPen pen;
        int cx = 0;
        int cy = 0;
        bool pendingWrap = false;
    };

    Buffer &current() { return m_altActive ? m_alt : m_primary; }
    const Buffer &current() const { return m_altActive ? m_alt : m_primary; }
    VtLine &lineAt(int absolute);
    const VtLine &lineAt(int absolute) const;
    VtLine &screenLine(int row);
    void blankLine(VtLine &line);
    void blankCells(VtLine &line, int from, int to);
    void scrollbackPush(const VtLine &line);
    void clampCursor();
    void validateScrollRegion();

    int m_cols;
    int m_rows;
    int m_scrollbackLimit;
    Buffer m_primary;
    Buffer m_alt;
    bool m_altActive = false;

    bool m_cursorVisible = true;
    bool m_cursorBlink = true;
    bool m_insertMode = false;
    bool m_autoWrap = true;
    bool m_originMode = false;
    bool m_appCursorKeys = false;
    bool m_appKeypad = false;
    bool m_bracketedPaste = false;

    int m_scrollTop = 0;
    int m_scrollBottom = 0;
    int m_scrollOffset = 0;
    QString m_title;

    VtPen m_savedPen;
    int m_savedCx = 0;
    int m_savedCy = 0;
    bool m_savedOrigin = false;

    mutable std::function<void()> m_onDamage;
    mutable quint64 m_revision = 0;
};

} // namespace termdock
