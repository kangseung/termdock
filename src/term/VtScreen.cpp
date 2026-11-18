#include "term/VtScreen.h"

#include <QString>
#include <algorithm>

namespace termdock {

namespace {

inline VtCell makeBlankCell(const VtColor &bg)
{
    VtCell c;
    c.ch = U' ';
    c.bg = bg;
    return c;
}

} // namespace

QColor VtColor::resolve(const std::vector<QColor> &palette, const QColor &fallback) const
{
    if (isDefault() || (!isIndexed() && !isRgb()))
        return fallback;
    if (isRgb())
        return QColor::fromRgb(rgbValue());
    const int idx = index();
    if (idx < 16) {
        if (idx < int(palette.size()))
            return palette[size_t(idx)];
        return fallback;
    }
    if (idx < 232) {
        const int n = idx - 16;
        const int r = n / 36;
        const int g = (n % 36) / 6;
        const int b = n % 6;
        auto level = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
        return QColor(level(r), level(g), level(b));
    }
    const int gray = 8 + (idx - 232) * 10;
    return QColor(gray, gray, gray);
}

VtScreen::VtScreen(int cols, int rows, int scrollbackLimit)
    : m_cols(cols)
    , m_rows(rows)
    , m_scrollbackLimit(scrollbackLimit)
{
    m_scrollBottom = m_rows - 1;
    for (Buffer *buf : {&m_primary, &m_alt}) {
        buf->lines.resize(size_t(m_rows));
        for (auto &line : buf->lines)
            line.cells.assign(size_t(m_cols), VtCell());
    }
}

void VtScreen::resize(int cols, int rows)
{
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    if (cols == m_cols && rows == m_rows)
        return;

    const int oldRows = m_rows;
    for (Buffer *buf : {&m_primary, &m_alt}) {
        for (auto &line : buf->lines)
            line.cells.resize(size_t(cols));

        if (rows > oldRows) {
            // 变大：只在下面加空白行。内容位置一动不动，
            // 绝不从滚回里往回拉——那样会把同一行复制到屏幕上（重复的提示符、错位）。
            buf->lines.resize(size_t(rows));
        } else if (rows < oldRows) {
            // 变小：从底部裁掉。被裁掉的行如果有内容，进滚回，不丢东西。
            for (int i = 0; i < oldRows - rows; ++i) {
                VtLine tail = std::move(buf->lines.back());
                buf->lines.pop_back();
                bool hasContent = false;
                for (const VtCell &cell : tail.cells) {
                    if (cell.ch != U' ' && cell.ch != 0) {
                        hasContent = true;
                        break;
                    }
                }
                if (hasContent && buf == &m_primary && !m_altActive) {
                    buf->scrollback.push_back(std::move(tail));
                    while (int(buf->scrollback.size()) > m_scrollbackLimit)
                        buf->scrollback.pop_front();
                }
            }
        }
        buf->lines.resize(size_t(rows));
        for (auto &line : buf->lines)
            line.cells.resize(size_t(cols), VtCell());
    }

    m_cols = cols;
    m_rows = rows;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_scrollOffset = 0;
    clampCursor();
    damage();
}

void VtScreen::resetPen()
{
    current().pen = VtPen{};
}

void VtScreen::setCursor(int row, int col)
{
    Buffer &b = current();
    b.cy = std::clamp(row, 0, m_rows - 1);
    b.cx = std::clamp(col, 0, m_cols - 1);
    b.pendingWrap = false;
    damage();
}

void VtScreen::setOriginMode(bool on)
{
    m_originMode = on;
    setCursor(0, 0);
}

void VtScreen::setScrollRegion(int top, int bottom)
{
    m_scrollTop = std::max(0, top);
    m_scrollBottom = std::min(m_rows - 1, bottom);
    validateScrollRegion();
    Buffer &b = current();
    b.cx = 0;
    b.cy = m_originMode ? m_scrollTop : 0;
    b.pendingWrap = false;
    damage();
}

void VtScreen::validateScrollRegion()
{
    if (m_scrollTop < 0)
        m_scrollTop = 0;
    if (m_scrollBottom > m_rows - 1)
        m_scrollBottom = m_rows - 1;
    if (m_scrollTop >= m_scrollBottom) {
        m_scrollTop = 0;
        m_scrollBottom = m_rows - 1;
    }
}

VtLine &VtScreen::screenLine(int row)
{
    return current().lines[size_t(std::clamp(row, 0, m_rows - 1))];
}

void VtScreen::blankLine(VtLine &line)
{
    // 注意：区域滚动用 std::move 搬行，被搬空的行 cells 会变成空 vector，
    // 所以这里必须 reassign 成完整宽度，不能只遍历清零。
    line.wrapped = false;
    line.cells.assign(size_t(m_cols), VtCell());
}

void VtScreen::blankCells(VtLine &line, int from, int to)
{
    from = std::clamp(from, 0, m_cols);
    to = std::clamp(to, 0, m_cols);
    const VtCell blank = makeBlankCell(current().pen.bg);
    for (int c = from; c < to; ++c)
        line.cells[size_t(c)] = blank;
}

void VtScreen::scrollbackPush(const VtLine &line)
{
    Buffer &b = current();
    b.scrollback.push_back(line);
    while (int(b.scrollback.size()) > m_scrollbackLimit)
        b.scrollback.pop_front();
}

void VtScreen::clampCursor()
{
    Buffer &b = current();
    b.cy = std::clamp(b.cy, 0, m_rows - 1);
    b.cx = std::clamp(b.cx, 0, m_cols - 1);
}

void VtScreen::putChar(char32_t ch)
{
    Buffer &b = current();
    int width = charWidth(ch);
    if (width == 0)
        return; // 组合字符（暂不支持叠加渲染）
    if (ch < 0x20)
        return;

    if (b.pendingWrap && m_autoWrap) {
        b.pendingWrap = false;
        b.cx = 0;
        lineFeed();
    }

    if (width == 2 && b.cx >= m_cols - 1) {
        if (m_autoWrap) {
            b.cx = 0;
            lineFeed();
        } else {
            b.cx = std::max(0, m_cols - 2);
        }
    }
    if (width > m_cols)
        width = 1;

    if (m_insertMode)
        insertChars(width);

    VtLine &line = screenLine(b.cy);
    // 覆盖宽字符的一半时，把另一半清掉，避免出现残影
    auto clearIfWide = [&line](int col) {
        if (col < 0 || col >= int(line.cells.size()))
            return;
        VtCell &c = line.cells[size_t(col)];
        if (c.attr & AttrWide) {
            if (col + 1 < int(line.cells.size()))
                line.cells[size_t(col + 1)] = VtCell();
            c = VtCell();
        } else if (c.attr & AttrWideTail) {
            if (col - 1 >= 0)
                line.cells[size_t(col - 1)] = VtCell();
            c = VtCell();
        }
    };
    clearIfWide(b.cx);
    if (width == 2)
        clearIfWide(b.cx + 1);

    VtCell cell;
    cell.ch = ch;
    cell.fg = b.pen.fg;
    cell.bg = b.pen.bg;
    cell.attr = quint16(b.pen.attr | (width == 2 ? AttrWide : 0));
    line.cells[size_t(b.cx)] = cell;

    if (width == 2 && b.cx + 1 < m_cols) {
        VtCell tail;
        tail.ch = 0;
        tail.fg = b.pen.fg;
        tail.bg = b.pen.bg;
        tail.attr = quint16(b.pen.attr | AttrWideTail);
        line.cells[size_t(b.cx + 1)] = tail;
    }
    line.wrapped = false;

    b.cx += width;
    if (b.cx >= m_cols) {
        b.cx = m_cols - 1;
        if (m_autoWrap)
            b.pendingWrap = true;
    }
    damage();
}

void VtScreen::lineFeed()
{
    Buffer &b = current();
    b.pendingWrap = false;
    if (b.cy == m_scrollBottom)
        scrollUpRegion(1);
    else if (b.cy < m_rows - 1)
        ++b.cy;
    damage();
}

void VtScreen::reverseIndex()
{
    Buffer &b = current();
    b.pendingWrap = false;
    if (b.cy == m_scrollTop)
        scrollDownRegion(1);
    else if (b.cy > 0)
        --b.cy;
    damage();
}

void VtScreen::carriageReturn()
{
    Buffer &b = current();
    b.cx = 0;
    b.pendingWrap = false;
    damage();
}

void VtScreen::backspace()
{
    Buffer &b = current();
    b.pendingWrap = false;
    if (b.cx > 0)
        --b.cx;
    damage();
}

void VtScreen::tab(int count)
{
    Buffer &b = current();
    b.pendingWrap = false;
    for (int i = 0; i < count; ++i) {
        b.cx = std::min(m_cols - 1, ((b.cx / 8) + 1) * 8);
    }
    damage();
}

void VtScreen::backTab(int count)
{
    Buffer &b = current();
    b.pendingWrap = false;
    for (int i = 0; i < count; ++i) {
        if (b.cx == 0)
            break;
        b.cx = ((b.cx - 1) / 8) * 8;
    }
    damage();
}

void VtScreen::moveUp(int n)
{
    Buffer &b = current();
    b.pendingWrap = false;
    const int top = (b.cy >= m_scrollTop && b.cy <= m_scrollBottom) ? m_scrollTop : 0;
    b.cy = std::max(top, b.cy - std::max(1, n));
    damage();
}

void VtScreen::moveDown(int n)
{
    Buffer &b = current();
    b.pendingWrap = false;
    const int bottom = (b.cy >= m_scrollTop && b.cy <= m_scrollBottom) ? m_scrollBottom : m_rows - 1;
    b.cy = std::min(bottom, b.cy + std::max(1, n));
    damage();
}

void VtScreen::moveLeft(int n)
{
    Buffer &b = current();
    b.pendingWrap = false;
    b.cx = std::max(0, b.cx - std::max(1, n));
    damage();
}

void VtScreen::moveRight(int n)
{
    Buffer &b = current();
    b.pendingWrap = false;
    b.cx = std::min(m_cols - 1, b.cx + std::max(1, n));
    damage();
}

void VtScreen::moveToRow(int row)
{
    Buffer &b = current();
    b.pendingWrap = false;
    int target = row;
    if (m_originMode)
        target += m_scrollTop;
    b.cy = std::clamp(target, 0, m_rows - 1);
    damage();
}

void VtScreen::moveToCol(int col)
{
    Buffer &b = current();
    b.pendingWrap = false;
    b.cx = std::clamp(col, 0, m_cols - 1);
    damage();
}

void VtScreen::moveToLineStart()
{
    current().cx = 0;
    damage();
}

void VtScreen::nextLine(int n)
{
    carriageReturn();
    moveDown(n);
}

void VtScreen::prevLine(int n)
{
    carriageReturn();
    moveUp(n);
}

void VtScreen::eraseInDisplay(int mode)
{
    Buffer &b = current();
    switch (mode) {
    case 0: {
        blankCells(b.lines[size_t(b.cy)], b.cx, m_cols);
        for (int r = b.cy + 1; r < m_rows; ++r)
            blankLine(b.lines[size_t(r)]);
        break;
    }
    case 1: {
        for (int r = 0; r < b.cy; ++r)
            blankLine(b.lines[size_t(r)]);
        blankCells(b.lines[size_t(b.cy)], 0, b.cx + 1);
        break;
    }
    case 2:
        for (int r = 0; r < m_rows; ++r)
            blankLine(b.lines[size_t(r)]);
        break;
    case 3:
        for (int r = 0; r < m_rows; ++r)
            blankLine(b.lines[size_t(r)]);
        if (!m_altActive) {
            b.scrollback.clear();
            m_scrollOffset = 0;
        }
        break;
    default:
        break;
    }
    b.pendingWrap = false;
    damage();
}

void VtScreen::eraseInLine(int mode)
{
    Buffer &b = current();
    VtLine &line = b.lines[size_t(b.cy)];
    switch (mode) {
    case 0:
        blankCells(line, b.cx, m_cols);
        break;
    case 1:
        blankCells(line, 0, b.cx + 1);
        break;
    case 2:
        blankCells(line, 0, m_cols);
        break;
    default:
        break;
    }
    b.pendingWrap = false;
    damage();
}

void VtScreen::eraseChars(int n)
{
    Buffer &b = current();
    blankCells(b.lines[size_t(b.cy)], b.cx, b.cx + std::max(1, n));
    damage();
}

void VtScreen::insertChars(int n)
{
    Buffer &b = current();
    n = std::clamp(n, 1, m_cols);
    VtLine &line = b.lines[size_t(b.cy)];
    for (int c = m_cols - 1; c >= b.cx + n; --c)
        line.cells[size_t(c)] = line.cells[size_t(c - n)];
    blankCells(line, b.cx, b.cx + n);
    damage();
}

void VtScreen::deleteChars(int n)
{
    Buffer &b = current();
    n = std::clamp(n, 1, m_cols);
    VtLine &line = b.lines[size_t(b.cy)];
    for (int c = b.cx; c < m_cols - n; ++c)
        line.cells[size_t(c)] = line.cells[size_t(c + n)];
    blankCells(line, std::max(b.cx, m_cols - n), m_cols);
    damage();
}

void VtScreen::scrollUpRegion(int n)
{
    Buffer &b = current();
    if (n <= 0)
        return;
    const int regionHeight = m_scrollBottom - m_scrollTop + 1;
    n = std::min(n, regionHeight);
    const bool toScrollback = !m_altActive && m_scrollTop == 0 && m_scrollBottom == m_rows - 1;
    for (int i = 0; i < n; ++i) {
        if (toScrollback)
            scrollbackPush(b.lines[0]);
        for (int r = m_scrollTop; r < m_scrollBottom; ++r)
            b.lines[size_t(r)] = std::move(b.lines[size_t(r + 1)]);
        blankLine(b.lines[size_t(m_scrollBottom)]);
    }
    damage();
}

void VtScreen::scrollDownRegion(int n)
{
    Buffer &b = current();
    if (n <= 0)
        return;
    const int regionHeight = m_scrollBottom - m_scrollTop + 1;
    n = std::min(n, regionHeight);
    for (int i = 0; i < n; ++i) {
        for (int r = m_scrollBottom; r > m_scrollTop; --r)
            b.lines[size_t(r)] = std::move(b.lines[size_t(r - 1)]);
        blankLine(b.lines[size_t(m_scrollTop)]);
    }
    damage();
}

void VtScreen::insertLines(int n)
{
    Buffer &b = current();
    if (b.cy < m_scrollTop || b.cy > m_scrollBottom)
        return;
    n = std::clamp(n, 1, m_scrollBottom - b.cy + 1);
    for (int i = 0; i < n; ++i) {
        for (int r = m_scrollBottom; r > b.cy; --r)
            b.lines[size_t(r)] = std::move(b.lines[size_t(r - 1)]);
        blankLine(b.lines[size_t(b.cy)]);
    }
    b.cx = 0;
    b.pendingWrap = false;
    damage();
}

void VtScreen::deleteLines(int n)
{
    Buffer &b = current();
    if (b.cy < m_scrollTop || b.cy > m_scrollBottom)
        return;
    n = std::clamp(n, 1, m_scrollBottom - b.cy + 1);
    for (int i = 0; i < n; ++i) {
        for (int r = b.cy; r < m_scrollBottom; ++r)
            b.lines[size_t(r)] = std::move(b.lines[size_t(r + 1)]);
        blankLine(b.lines[size_t(m_scrollBottom)]);
    }
    b.cx = 0;
    b.pendingWrap = false;
    damage();
}

void VtScreen::saveCursor()
{
    Buffer &b = current();
    m_savedPen = b.pen;
    m_savedCx = b.cx;
    m_savedCy = b.cy;
    m_savedOrigin = m_originMode;
}

void VtScreen::restoreCursor()
{
    Buffer &b = current();
    b.pen = m_savedPen;
    m_originMode = m_savedOrigin;
    b.cx = m_savedCx;
    b.cy = m_savedCy;
    b.pendingWrap = false;
    clampCursor();
    damage();
}

void VtScreen::enterAltScreen()
{
    if (m_altActive)
        return;
    m_altActive = true;
    m_alt = Buffer();
    m_alt.lines.resize(size_t(m_rows));
    for (auto &line : m_alt.lines)
        line.cells.assign(size_t(m_cols), VtCell());
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_scrollOffset = 0;
    damage();
}

void VtScreen::exitAltScreen()
{
    if (!m_altActive)
        return;
    m_altActive = false;
    m_alt = Buffer();
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_scrollOffset = 0;
    clampCursor();
    damage();
}

void VtScreen::fullReset()
{
    m_primary = Buffer();
    m_alt = Buffer();
    for (Buffer *buf : {&m_primary, &m_alt}) {
        buf->lines.resize(size_t(m_rows));
        for (auto &line : buf->lines)
            line.cells.assign(size_t(m_cols), VtCell());
    }
    m_altActive = false;
    m_cursorVisible = true;
    m_insertMode = false;
    m_autoWrap = true;
    m_originMode = false;
    m_appCursorKeys = false;
    m_appKeypad = false;
    m_bracketedPaste = false;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_scrollOffset = 0;
    m_title.clear();
    damage();
}

void VtScreen::setTitle(const QString &title)
{
    m_title = title;
    damage();
}

void VtScreen::setScrollOffset(int offset)
{
    const int maxOffset = scrollbackCount();
    const int clamped = std::clamp(offset, 0, maxOffset);
    if (clamped != m_scrollOffset) {
        m_scrollOffset = clamped;
        damage();
    }
}

int VtScreen::absoluteLineOf(int visualRow) const
{
    return scrollbackCount() - m_scrollOffset + visualRow;
}

const VtLine &VtScreen::lineAt(int absolute) const
{
    static const VtLine blank;
    const Buffer &b = current();
    if (absolute < 0)
        return blank;
    const int sb = int(b.scrollback.size());
    if (absolute < sb)
        return b.scrollback[size_t(absolute)];
    const int row = absolute - sb;
    if (row >= int(b.lines.size()))
        return blank;
    return b.lines[size_t(row)];
}

VtLine &VtScreen::lineAt(int absolute)
{
    const int sb = int(current().scrollback.size());
    if (absolute < sb)
        return current().scrollback[size_t(std::max(0, absolute))];
    return current().lines[size_t(std::clamp(absolute - sb, 0, m_rows - 1))];
}

const VtCell &VtScreen::cellAtAbsolute(int line, int col) const
{
    static const VtCell blank;
    const VtLine &l = lineAt(line);
    if (col < 0 || col >= int(l.cells.size()))
        return blank;
    return l.cells[size_t(col)];
}

const VtCell &VtScreen::cellAt(int visualRow, int col) const
{
    return cellAtAbsolute(absoluteLineOf(visualRow), col);
}

bool VtScreen::lineIsWrapped(int absoluteLine) const
{
    return lineAt(absoluteLine).wrapped;
}

QString VtScreen::textInRange(int startLine, int startCol, int endLine, int endCol) const
{
    if (startLine > endLine || (startLine == endLine && startCol > endCol))
        return QString();
    QString out;
    for (int line = startLine; line <= endLine; ++line) {
        const VtLine &l = lineAt(line);
        const int from = (line == startLine) ? startCol : 0;
        const int to = (line == endLine) ? endCol : int(l.cells.size()) - 1;
        QString row;
        for (int c = std::max(0, from); c <= to && c < int(l.cells.size()); ++c) {
            const VtCell &cell = l.cells[size_t(c)];
            if (cell.attr & AttrWideTail)
                continue;
            row.append(cell.ch ? QString::fromUcs4(&cell.ch, 1) : QStringLiteral(" "));
        }
        while (row.endsWith(QLatin1Char(' ')))
            row.chop(1);
        out.append(row);
        if (line != endLine)
            out.append(l.wrapped ? QStringLiteral(" ") : QStringLiteral("\n"));
    }
    return out;
}

int VtScreen::charWidth(char32_t ch)
{
    if (ch == 0)
        return 0;
    if (ch < 0x20 || (ch >= 0x7F && ch < 0xA0))
        return 0;
    // 组合符号 / 变体选择符
    if ((ch >= 0x0300 && ch <= 0x036F) || (ch >= 0x0483 && ch <= 0x0489)
        || (ch >= 0x0591 && ch <= 0x05BD) || (ch >= 0x0610 && ch <= 0x061A)
        || (ch >= 0x064B && ch <= 0x065F) || (ch >= 0x0670 && ch <= 0x0670)
        || (ch >= 0x06D6 && ch <= 0x06DC) || (ch >= 0x0730 && ch <= 0x074A)
        || (ch >= 0x07A6 && ch <= 0x07B0) || (ch >= 0x0900 && ch <= 0x0903)
        || (ch >= 0x093A && ch <= 0x094F) || (ch >= 0x0E31 && ch <= 0x0E3A)
        || (ch >= 0x0E47 && ch <= 0x0E4E) || (ch >= 0x1AB0 && ch <= 0x1AFF)
        || (ch >= 0x1DC0 && ch <= 0x1DFF) || (ch >= 0x200B && ch <= 0x200F)
        || (ch >= 0x20D0 && ch <= 0x20F0) || (ch >= 0xFE00 && ch <= 0xFE0F)
        || (ch >= 0xFE20 && ch <= 0xFE2F) || (ch >= 0xFEFF && ch <= 0xFEFF)
        || (ch >= 0xE0100 && ch <= 0xE01EF) || ch == 0x200D)
        return 0;
    // 东亚宽字符
    if ((ch >= 0x1100 && ch <= 0x115F) || (ch >= 0x2E80 && ch <= 0x303E)
        || (ch >= 0x3041 && ch <= 0x33FF) || (ch >= 0x3400 && ch <= 0x4DBF)
        || (ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0xA000 && ch <= 0xA4CF)
        || (ch >= 0xA960 && ch <= 0xA97F) || (ch >= 0xAC00 && ch <= 0xD7A3)
        || (ch >= 0xF900 && ch <= 0xFAFF) || (ch >= 0xFE10 && ch <= 0xFE19)
        || (ch >= 0xFE30 && ch <= 0xFE6F) || (ch >= 0xFF00 && ch <= 0xFF60)
        || (ch >= 0xFFE0 && ch <= 0xFFE6) || (ch >= 0x1F004 && ch <= 0x1F004)
        || (ch >= 0x1F0CF && ch <= 0x1F0CF) || (ch >= 0x1F18E && ch <= 0x1F18E)
        || (ch >= 0x1F191 && ch <= 0x1F19A) || (ch >= 0x1F200 && ch <= 0x1F320)
        || (ch >= 0x1F32D && ch <= 0x1F335) || (ch >= 0x1F337 && ch <= 0x1F37C)
        || (ch >= 0x1F37E && ch <= 0x1F393) || (ch >= 0x1F3A0 && ch <= 0x1F3CA)
        || (ch >= 0x1F3CF && ch <= 0x1F3D3) || (ch >= 0x1F3E0 && ch <= 0x1F3F0)
        || (ch >= 0x1F3F4 && ch <= 0x1F3F4) || (ch >= 0x1F3F8 && ch <= 0x1F43E)
        || (ch >= 0x1F440 && ch <= 0x1F440) || (ch >= 0x1F442 && ch <= 0x1F4FC)
        || (ch >= 0x1F4FF && ch <= 0x1F53D) || (ch >= 0x1F54B && ch <= 0x1F54E)
        || (ch >= 0x1F550 && ch <= 0x1F567) || (ch >= 0x1F57A && ch <= 0x1F57A)
        || (ch >= 0x1F595 && ch <= 0x1F596) || (ch >= 0x1F5A4 && ch <= 0x1F5A4)
        || (ch >= 0x1F5FB && ch <= 0x1F64F) || (ch >= 0x1F680 && ch <= 0x1F6C5)
        || (ch >= 0x1F6CC && ch <= 0x1F6CC) || (ch >= 0x1F6D0 && ch <= 0x1F6D2)
        || (ch >= 0x1F6EB && ch <= 0x1F6EC) || (ch >= 0x1F6F4 && ch <= 0x1F6FC)
        || (ch >= 0x1F7E0 && ch <= 0x1F7EB) || (ch >= 0x1F90C && ch <= 0x1F93A)
        || (ch >= 0x1F93C && ch <= 0x1F945) || (ch >= 0x1F947 && ch <= 0x1F9FF)
        || (ch >= 0x1FA70 && ch <= 0x1FAFF) || (ch >= 0x20000 && ch <= 0x3FFFD))
        return 2;
    return 1;
}

} // namespace termdock
