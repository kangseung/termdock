#include "term/TerminalWidget.h"

#include "app/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QInputMethod>
#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace termdock {

namespace {
constexpr int kMinCols = 20;
constexpr int kMinRows = 5;
}

TerminalWidget::TerminalWidget(QWidget *parent)
    : QWidget(parent)
    , m_parser(&m_screen)
{
    // 不开 WA_OpaquePaintEvent：125%/150% 这类分数缩放下，Qt 的后备缓冲按设备像素
    // 对齐，未标记重绘的区域会留着上一帧（看起来就是"错位/重复/卡贴图"）。
    setAttribute(Qt::WA_StaticContents, false);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::IBeamCursor);
    setMinimumSize(200, 120);

    m_screen.setDamageCallback([this]() { update(); });
    m_parser.setRespondCallback([this](const QByteArray &bytes) { sendBytes(bytes); });
    m_parser.setBellCallback([this]() { emit bell(); });
    m_parser.setTitleCallback([this](const QString &title) { emit titleChanged(title); });

    updateMetrics();
    recomputeGrid();

    auto *blink = new QTimer(this);
    blink->setInterval(530);
    connect(blink, &QTimer::timeout, this, [this]() {
        m_blinkOn = !m_blinkOn;
        if (m_hasFocus && m_connected)
            update();
    });
    blink->start();
}

TerminalWidget::~TerminalWidget() = default;

void TerminalWidget::updateMetrics()
{
    const Theme &theme = Theme::instance();
    m_font = theme.terminalFont();
    m_boldFont = m_font;
    m_boldFont.setBold(true);

    const QFontMetricsF fm(m_font);
    // 用 10 个数字的平均宽度定格宽，比单字符更稳（等宽字体下完全一致）
    m_cellW = fm.horizontalAdvance(QStringLiteral("0123456789")) / 10.0;
    if (m_cellW <= 0)
        m_cellW = fm.averageCharWidth();
    // 行高比行距多 2px，屏幕看起来不挤
    m_cellH = std::ceil(fm.lineSpacing()) + 2;
    m_ascent = fm.ascent();
    m_descent = fm.descent();
    // 分数缩放（1.25 / 1.5）下把格宽、行高吸附到设备像素，避免每个字符半像素错位
    const qreal dpr = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
    m_cellW = std::round(m_cellW * dpr) / dpr;
    m_cellH = std::round(m_cellH * dpr) / dpr;
    m_baseline = std::round((m_cellH - (m_ascent + m_descent)) / 2.0 + m_ascent) / dpr * dpr;
}

void TerminalWidget::applyFont()
{
    updateMetrics();
    recomputeGrid();
    update();
}

void TerminalWidget::recomputeGrid()
{
    // 还没完成布局（刚 addTab / 刚 show）时尺寸是不可信的（曾出现过 20x5），
    // 拿它去设 PTY 会让远端的 MOTD 按错误宽度排版、右侧内容被截断。
    if (width() < 200 || height() < 120)
        return;
    // 右侧留 10px 给滚动条，避免最后一列被滚动条压住
    const int usableW = std::max(1, width() - 2 * m_padX - 10);
    const int usableH = std::max(1, height() - 2 * m_padY);
    const int cols = std::max(kMinCols, int(usableW / m_cellW));
    const int rows = std::max(kMinRows, int(usableH / m_cellH));
    if (cols != m_screen.cols() || rows != m_screen.rows()) {
        m_screen.resize(cols, rows);
        m_preedit.clear();
        emit gridResized(cols, rows);
        update();
    }
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    recomputeGrid();
    // 必须整窗重绘：WA_OpaquePaintEvent 下 Qt 不会自动清背景，
    // 只补暴露区域会把旧尺寸的画面留在原地，表现就是放大/最大化后错位、重复、卡贴图。
    update();
}

void TerminalWidget::feed(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    m_parser.feed(data);
    update();
}

void TerminalWidget::setConnected(bool connected)
{
    m_connected = connected;
    setCursor(m_screen.mouseMode() == VtScreen::MouseMode::Off ? Qt::IBeamCursor
                                                               : Qt::ArrowCursor);
    if (connected) {
        m_statusText.clear();
    }
    update();
}

void TerminalWidget::setStatusText(const QString &text)
{
    m_statusText = text;
    update();
}

void TerminalWidget::setFallbackTitle(const QString &title)
{
    m_fallbackTitle = title;
}

void TerminalWidget::clearScreen()
{
    m_screen.fullReset();
    m_screen.scrollToBottom();
    update();
}

void TerminalWidget::sendBytes(const QByteArray &bytes)
{
    if (bytes.isEmpty() || !m_connected)
        return;
    emit dataToSend(bytes);
}

void TerminalWidget::sendKeyText(const QString &text)
{
    if (text.isEmpty())
        return;
    sendBytes(text.toUtf8());
}

void TerminalWidget::copySelection()
{
    const QString text = selectionText();
    if (text.isEmpty())
        return;
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(text);
}

QString TerminalWidget::selectionText() const
{
    if (!hasSelection())
        return QString();
    int sl = m_selStartLine;
    int sc = m_selStartCol;
    int el = m_selEndLine;
    int ec = m_selEndCol;
    if (sl > el || (sl == el && sc > ec)) {
        std::swap(sl, el);
        std::swap(sc, ec);
    }
    return m_screen.textInRange(sl, sc, el, ec);
}

void TerminalWidget::pasteClipboard()
{
    QClipboard *clipboard = QApplication::clipboard();
    QString text = clipboard->text();
    if (text.isEmpty())
        return;
    text.replace(QLatin1String("\r\n"), QLatin1String("\r"));
    if (m_screen.bracketedPaste())
        sendBytes(QByteArray("\x1b[200~") + text.toUtf8() + QByteArray("\x1b[201~"));
    else
        sendBytes(text.toUtf8());
}

void TerminalWidget::selectAll()
{
    m_selecting = true;
    m_selStartLine = 0;
    m_selStartCol = 0;
    m_selEndLine = m_screen.totalLines() - 1;
    m_selEndCol = m_screen.cols() - 1;
    update();
}

void TerminalWidget::clampSelection()
{
    const int maxLine = m_screen.totalLines() - 1;
    m_selStartLine = std::clamp(m_selStartLine, 0, maxLine);
    m_selEndLine = std::clamp(m_selEndLine, 0, maxLine);
    m_selStartCol = std::clamp(m_selStartCol, 0, m_screen.cols() - 1);
    m_selEndCol = std::clamp(m_selEndCol, 0, m_screen.cols() - 1);
}

void TerminalWidget::selectWordAt(int line, int col)
{
    const int cols = m_screen.cols();
    auto isWordChar = [](char32_t c) {
        return (c && c != U' ') || c == U'\0';
    };
    int start = col;
    int end = col;
    while (start > 0 && isWordChar(m_screen.cellAtAbsolute(line, start - 1).ch))
        --start;
    while (end < cols - 1 && isWordChar(m_screen.cellAtAbsolute(line, end + 1).ch))
        ++end;
    m_selecting = true;
    m_selStartLine = line;
    m_selStartCol = start;
    m_selEndLine = line;
    m_selEndCol = end;
    update();
}

void TerminalWidget::selectLineAt(int line)
{
    m_selecting = true;
    m_selStartLine = line;
    m_selStartCol = 0;
    m_selEndLine = line;
    m_selEndCol = m_screen.cols() - 1;
    update();
}

int TerminalWidget::rowAt(int y) const
{
    return std::clamp(int((y - m_padY) / m_cellH), 0, m_screen.rows() - 1);
}

int TerminalWidget::colAt(int x) const
{
    return std::clamp(int((x - m_padX) / m_cellW), 0, m_screen.cols() - 1);
}

void TerminalWidget::scrollBy(int lines)
{
    m_screen.setScrollOffset(m_screen.scrollOffset() + lines);
}

QRectF TerminalWidget::scrollBarTrack() const
{
    constexpr qreal kWidth = 6;
    return QRectF(width() - kWidth - 3, m_padY, kWidth, height() - 2 * m_padY);
}

bool TerminalWidget::scrollBarVisible() const
{
    return m_screen.scrollbackCount() > 0
        && (m_screen.scrollOffset() > 0 || m_scrollBarHovered || m_draggingThumb);
}

QRectF TerminalWidget::scrollBarThumb() const
{
    const QRectF track = scrollBarTrack();
    const int total = m_screen.totalLines();
    if (total <= 0)
        return QRectF();
    const qreal ratio = qreal(m_screen.rows()) / qreal(total);
    const qreal thumbHeight = std::clamp(track.height() * ratio, 28.0, track.height());
    const int maxOffset = m_screen.scrollbackCount();
    const qreal pos = maxOffset > 0 ? qreal(maxOffset - m_screen.scrollOffset()) / qreal(maxOffset) : 0.0;
    const qreal thumbTop = track.top() + (track.height() - thumbHeight) * pos;
    return QRectF(track.left(), thumbTop, track.width(), thumbHeight);
}

void TerminalWidget::scrollToOffsetFromY(qreal y)
{
    const QRectF track = scrollBarTrack();
    const QRectF thumb = scrollBarThumb();
    const qreal span = track.height() - thumb.height();
    if (span <= 0)
        return;
    const qreal ratio = std::clamp((y - m_dragGrabOffset - track.top()) / span, 0.0, 1.0);
    m_screen.setScrollOffset(int(std::lround((1.0 - ratio) * m_screen.scrollbackCount())));
    update();
}

void TerminalWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    const Theme &theme = Theme::instance();
    QPainter p(this);
    // 强行把剪辑区扩到整个控件：WA_OpaquePaintEvent 下如果只按 Qt 给的脏区绘制，
    // 缩放/最大化后未被标记的旧画面会留下来（错位、重复、卡贴图）。
    p.setClipRect(rect(), Qt::ReplaceClip);
    p.fillRect(rect(), theme.background());
    // 所有格点坐标吸附到设备像素：分数缩放下否则会出现"重影/贴图错位"
    const qreal dpr = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
    const auto snap = [dpr](qreal v) { return std::round(v * dpr) / dpr; };
    const qreal padX = snap(m_padX);
    const qreal padY = snap(m_padY);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int cols = m_screen.cols();
    const int rows = m_screen.rows();

    // 选区范围（已排序）
    int selSl = m_selStartLine;
    int selSc = m_selStartCol;
    int selEl = m_selEndLine;
    int selEc = m_selEndCol;
    if (selSl > selEl || (selSl == selEl && selSc > selEc)) {
        std::swap(selSl, selEl);
        std::swap(selSc, selEc);
    }
    const bool selVisible = hasSelection();

    for (int row = 0; row < rows; ++row) {
        const int absLine = m_screen.absoluteLineOf(row);
        const qreal y = padY + row * m_cellH;
        const qreal baseY = snap(y + m_baseline); // 每行自己的基线，吸附到设备像素

        int col = 0;
        while (col < cols) {
            const VtCell &cell = m_screen.cellAtAbsolute(absLine, col);
            if (cell.attr & AttrWideTail) {
                ++col;
                continue;
            }
            QColor fg = cell.fg.resolve(theme.palette(), theme.foreground());
            QColor bg = cell.bg.resolve(theme.palette(), theme.background());
            if (cell.attr & AttrReverse)
                std::swap(fg, bg);
            if (cell.attr & AttrBold) {
                if (!cell.fg.isDefault() && cell.fg.isIndexed() && cell.fg.index() < 8)
                    fg = theme.palette()[size_t(cell.fg.index() + 8)];
                else if (cell.fg.isDefault())
                    fg = theme.boldText();
            }
            if (cell.attr & AttrDim)
                fg = QColor((fg.red() + bg.red()) / 2, (fg.green() + bg.green()) / 2,
                            (fg.blue() + bg.blue()) / 2);
            if (cell.attr & AttrHidden)
                fg = bg;

            const int width = (cell.attr & AttrWide) ? 2 : 1;
            const quint16 styleMask = quint16(AttrBold | AttrDim | AttrItalic | AttrUnderline
                                              | AttrReverse | AttrHidden | AttrStrike);
            // 找同色同属性的连续段
            int end = col + width;
            while (end < cols) {
                const VtCell &next = m_screen.cellAtAbsolute(absLine, end);
                const QColor nfg = next.fg.resolve(theme.palette(), theme.foreground());
                const QColor nbg = next.bg.resolve(theme.palette(), theme.background());
                if (nbg != bg || nfg != fg
                    || (next.attr & styleMask) != (cell.attr & styleMask))
                    break;
                end += (next.attr & AttrWide) ? 2 : 1;
            }

            const qreal x = snap(padX + col * m_cellW);
            const qreal w = (end - col) * m_cellW;
            if (bg != theme.background())
                p.fillRect(QRectF(x, y, w, m_cellH), bg);

            // 选区高亮
            if (selVisible && absLine >= selSl && absLine <= selEl) {
                const int from = std::max(col, (absLine == selSl) ? selSc : 0);
                const int to = std::min(end - 1, (absLine == selEl) ? selEc : cols - 1);
                if (to >= from) {
                    p.fillRect(QRectF(snap(padX + from * m_cellW), y,
                                      (to - from + 1) * m_cellW, m_cellH),
                               theme.selection());
                }
            }

            // 文本：逐格按 cellW 精确定位绘制。
            // 早期用「连续字符批量 drawText」在字体 advance 与 cellW 不完全相等时
            // 会产生累积漂移（长行越到后面越错位），改成逐格定位后永不错位。
            if (fg != bg) {
                p.setFont((cell.attr & AttrBold) ? m_boldFont : m_font);
                p.setPen(fg);
                for (int c = col; c < end; ++c) {
                    const VtCell &cc = m_screen.cellAtAbsolute(absLine, c);
                    if (cc.ch == 0 || cc.ch == U' ')
                        continue;
                    p.drawText(QPointF(snap(padX + c * m_cellW), baseY),
                               QString::fromUcs4(&cc.ch, 1));
                }
                if (cell.attr & AttrUnderline) {
                    p.setPen(fg);
                    p.drawLine(QPointF(x, y + m_cellH - 2), QPointF(x + w - 1, y + m_cellH - 2));
                }
                if (cell.attr & AttrStrike) {
                    p.setPen(fg);
                    p.drawLine(QPointF(x, y + m_cellH * 0.55), QPointF(x + w - 1, y + m_cellH * 0.55));
                }
            }
            col = end;
        }
    }

    // 光标
    const bool showCursor = m_connected && m_hasFocus && m_screen.cursorVisible()
        && m_screen.scrollOffset() == 0 && m_blinkOn;
    if (showCursor) {
        const qreal x = snap(padX + m_screen.cursorCol() * m_cellW);
        const qreal y = snap(padY + m_screen.cursorRow() * m_cellH);
        const VtCell &cell = m_screen.cellAtAbsolute(m_screen.absoluteLineOf(m_screen.cursorRow()),
                                                    m_screen.cursorCol());
        QColor cursorColor = theme.cursor();
        cursorColor.setAlpha(m_hasFocus ? 190 : 90);
        p.setPen(Qt::NoPen);
        p.setBrush(cursorColor);
        const qreal cursorW = (cell.attr & AttrWide) ? m_cellW * 2 : m_cellW;
        p.drawRoundedRect(QRectF(x, y + 1.5, cursorW, m_cellH - 3), 2.5, 2.5);
        if (cell.ch && cell.ch != U' ' && m_hasFocus) {
            p.setFont(m_font);
            p.setPen(theme.background());
            p.drawText(QPointF(std::round(x), y + m_baseline), QString::fromUcs4(&cell.ch, 1));
        }
    }

    // 输入法预编辑
    if (!m_preedit.isEmpty() && m_connected) {
        const qreal x = snap(padX + m_screen.cursorCol() * m_cellW);
        const qreal y = snap(padY + m_screen.cursorRow() * m_cellH);
        const QFontMetricsF fm(m_font);
        const qreal w = fm.horizontalAdvance(m_preedit) + 6;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x10, 0x13, 0x1a, 235));
        p.drawRoundedRect(QRectF(x, y, w, m_cellH), 3, 3);
        p.setFont(m_font);
        p.setPen(theme.accent());
        p.drawText(QPointF(x + 3, y + m_baseline), m_preedit);
        p.drawLine(QPointF(x + 3, y + m_cellH - 2), QPointF(x + w - 3, y + m_cellH - 2));
    }

    // 未连接提示
    if (!m_connected) {
        p.setFont(QFont(theme.terminalFont().family(), 12));
        p.setPen(theme.mutedText());
        const QString text = m_statusText.isEmpty() ? QStringLiteral("未连接") : m_statusText;
        p.drawText(rect(), Qt::AlignCenter, text);
    }

    // 顶部渐隐阴影：提示上面还有历史可滚
    if (m_screen.scrollOffset() > 0) {
        QLinearGradient shadow(0, 0, 0, m_cellH);
        shadow.setColorAt(0.0, QColor(0, 0, 0, 110));
        shadow.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.fillRect(QRectF(0, 0, width(), m_cellH), shadow);
    }

    // 自绘滚动条
    if (scrollBarVisible()) {
        p.setPen(Qt::NoPen);
        QColor trackColor = theme.mutedText();
        trackColor.setAlpha(28);
        p.setBrush(trackColor);
        p.drawRoundedRect(scrollBarTrack(), 3, 3);
        QColor thumbColor = theme.foreground();
        thumbColor.setAlpha(m_draggingThumb ? 190 : 120);
        p.setBrush(thumbColor);
        p.drawRoundedRect(scrollBarThumb(), 3, 3);
    }

    // 滚回位置小提示
    if (m_connected && m_screen.scrollOffset() > 0) {
        const QString text = QStringLiteral("↑ 滚回 %1 / %2 行")
                                 .arg(m_screen.scrollOffset())
                                 .arg(m_screen.scrollbackCount());
        QFont chipFont = m_font;
        chipFont.setPointSizeF(std::max(7.0, chipFont.pointSizeF() - 2.0));
        p.setFont(chipFont);
        const QFontMetricsF fm(chipFont);
        const qreal chipW = fm.horizontalAdvance(text) + 18;
        const QRectF chip(width() - chipW - 14, height() - 26, chipW, 18);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 120));
        p.drawRoundedRect(chip, 9, 9);
        p.setPen(theme.mutedText());
        p.drawText(chip, Qt::AlignCenter, text);
    }
}

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    const bool alt = mods.testFlag(Qt::AltModifier);

    // 正在回看历史时一敲键就回到最新输出，符合直觉
    if (m_screen.scrollOffset() > 0) {
        m_screen.scrollToBottom();
        update();
    }

    if (ctrl && shift && key == Qt::Key_C) {
        copySelection();
        return;
    }
    if (ctrl && shift && key == Qt::Key_V) {
        pasteClipboard();
        return;
    }
    if (ctrl && shift && key == Qt::Key_A) {
        selectAll();
        return;
    }
    if (!m_connected) {
        event->ignore();
        return;
    }

    const bool appCursor = m_screen.applicationCursorKeys();
    QByteArray out;
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        out = "\r";
        break;
    case Qt::Key_Tab:
        out = "\t";
        break;
    case Qt::Key_Backtab:
        out = "\x1b[Z";
        break;
    case Qt::Key_Backspace:
        out = ctrl ? QByteArray("\x08") : QByteArray("\x7f");
        break;
    case Qt::Key_Escape:
        out = "\x1b";
        break;
    case Qt::Key_Up:
        out = appCursor ? "\x1bOA" : "\x1b[A";
        break;
    case Qt::Key_Down:
        out = appCursor ? "\x1bOB" : "\x1b[B";
        break;
    case Qt::Key_Right:
        out = appCursor ? "\x1bOC" : "\x1b[C";
        break;
    case Qt::Key_Left:
        out = appCursor ? "\x1bOD" : "\x1b[D";
        break;
    case Qt::Key_Home:
        out = appCursor ? "\x1bOH" : "\x1b[H";
        break;
    case Qt::Key_End:
        out = appCursor ? "\x1bOF" : "\x1b[F";
        break;
    case Qt::Key_PageUp:
        out = "\x1b[5~";
        break;
    case Qt::Key_PageDown:
        out = "\x1b[6~";
        break;
    case Qt::Key_Insert:
        out = "\x1b[2~";
        break;
    case Qt::Key_Delete:
        out = "\x1b[3~";
        break;
    case Qt::Key_F1:
        out = "\x1bOP";
        break;
    case Qt::Key_F2:
        out = "\x1bOQ";
        break;
    case Qt::Key_F3:
        out = "\x1bOR";
        break;
    case Qt::Key_F4:
        out = "\x1bOS";
        break;
    case Qt::Key_F5:
        out = "\x1b[15~";
        break;
    case Qt::Key_F6:
        out = "\x1b[17~";
        break;
    case Qt::Key_F7:
        out = "\x1b[18~";
        break;
    case Qt::Key_F8:
        out = "\x1b[19~";
        break;
    case Qt::Key_F9:
        out = "\x1b[20~";
        break;
    case Qt::Key_F10:
        out = "\x1b[21~";
        break;
    case Qt::Key_F11:
        out = "\x1b[23~";
        break;
    case Qt::Key_F12:
        out = "\x1b[24~";
        break;
    default:
        break;
    }
    if (!out.isEmpty() && !(alt && key >= Qt::Key_A && key <= Qt::Key_Z && out.size() > 1)) {
        sendBytes(out);
        return;
    }

    if (ctrl) {
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            sendBytes(QByteArray(1, char(key - Qt::Key_A + 1)));
            return;
        }
        if (key == Qt::Key_Space)
            sendBytes(QByteArray(1, '\0'));
        return;
    }

    QString text = event->text();
    if (text.isEmpty())
        return;
    if (alt)
        sendBytes(QByteArray("\x1b") + text.toUtf8());
    else
        sendBytes(text.toUtf8());
}

bool TerminalWidget::focusNextPrevChild(bool next)
{
    // Qt 在把 Tab 交给 keyPressEvent 之前，会先试 focusNextPrevChild()；
    // 返回 true 就把 Tab 当成“切换焦点”吃掉了（远端 shell 根本收不到 \t，
    // 于是 cd <Tab> 永远补全不了）。终端里 Tab/Shift+Tab 属于 shell：
    // 正向按 cd/dir 补全，Shift+Tab 发 ESC[Z 做反向补全。
    Q_UNUSED(next);
    return false;
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent *event)
{
    if (event->commitString().isEmpty()) {
        m_preedit = event->preeditString();
        update();
        return;
    }
    m_preedit.clear();
    sendBytes(event->commitString().toUtf8());
    update();
}

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
    const QRect cursorRect(int(m_padX + m_screen.cursorCol() * m_cellW),
                           int(m_padY + m_screen.cursorRow() * m_cellH), int(m_cellW), int(m_cellH));
    switch (query) {
    case Qt::ImCursorRectangle:
    case Qt::ImAnchorRectangle:
    case Qt::ImCursorPosition:
        return query == Qt::ImCursorPosition ? QVariant(0) : QVariant(cursorRect);
    case Qt::ImEnabled:
        return m_connected && isVisible();
    case Qt::ImReadOnly:
        return false;
    case Qt::ImHints:
        return int(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    case Qt::ImSurroundingText:
        return QString();
    case Qt::ImCurrentSelection:
        return QString();
    default:
        break;
    }
    return QWidget::inputMethodQuery(query);
}

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);

    // 先处理滚动条拖动
    if (event->button() == Qt::LeftButton && scrollBarVisible()
        && scrollBarTrack().adjusted(-6, 0, 4, 0).contains(event->position())) {
        const QRectF thumb = scrollBarThumb();
        m_draggingThumb = true;
        if (thumb.contains(event->position()))
            m_dragGrabOffset = event->position().y() - thumb.top();
        else
            m_dragGrabOffset = thumb.height() / 2.0;
        scrollToOffsetFromY(event->position().y());
        return;
    }

    if (event->button() != Qt::LeftButton && m_screen.mouseMode() != VtScreen::MouseMode::Off) {
        if (reportMouse(event, false)) {
            m_mouseReportedPress = true;
            return;
        }
    }
    if (event->button() == Qt::LeftButton) {
        if (reportMouse(event, false)) {
            m_mouseReportedPress = true;
            return;
        }
    }

    const int absLine = m_screen.absoluteLineOf(rowAt(int(event->position().y())));
    const int col = colAt(int(event->position().x()));

    if (event->button() == Qt::MiddleButton) {
        QClipboard *clipboard = QApplication::clipboard();
        QString text = clipboard->text(QClipboard::Selection);
        if (text.isEmpty())
            text = clipboard->text();
        if (!text.isEmpty()) {
            text.replace(QLatin1String("\r\n"), QLatin1String("\r"));
            sendBytes(text.toUtf8());
        }
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_selecting = true;
        m_selStartLine = absLine;
        m_selStartCol = col;
        m_selEndLine = absLine;
        m_selEndCol = col;
        update();
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_draggingThumb) {
        scrollToOffsetFromY(event->position().y());
        return;
    }
    const VtScreen::MouseMode mouseMode = m_screen.mouseMode();
    if (mouseMode == VtScreen::MouseMode::Move
        || (mouseMode == VtScreen::MouseMode::Drag && event->buttons() != Qt::NoButton)) {
        if (reportMouse(event, false))
            return;
    }

    const bool hovered = m_screen.scrollbackCount() > 0
        && scrollBarTrack().adjusted(-6, 0, 4, 0).contains(event->position());
    if (hovered != m_scrollBarHovered) {
        m_scrollBarHovered = hovered;
        update();
    }
    if (!m_selecting)
        return;
    const int y = int(event->position().y());
    const int x = int(event->position().x());
    if (y < m_padY) {
        scrollBy(1);
    } else if (y > height() - m_padY) {
        scrollBy(-1);
    }
    const int row = rowAt(y);
    m_selEndLine = m_screen.absoluteLineOf(row);
    m_selEndCol = colAt(x);
    clampSelection();
    update();
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    if (m_draggingThumb) {
        m_draggingThumb = false;
        update();
        return;
    }
    if (m_mouseReportedPress) {
        m_mouseReportedPress = false;
        reportMouse(event, true);
        return;
    }
    if (!m_selecting)
        return;
    // 单击（无拖拽）视为取消选区
    if (m_selStartLine == m_selEndLine && m_selStartCol == m_selEndCol) {
        m_selecting = false;
        update();
        return;
    }
    m_selecting = true;
    if (!selectionText().isEmpty())
        QApplication::clipboard()->setText(selectionText(), QClipboard::Selection);
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int absLine = m_screen.absoluteLineOf(rowAt(int(event->position().y())));
    const int col = colAt(int(event->position().x()));
    selectWordAt(absLine, col);
}

bool TerminalWidget::reportMouse(QMouseEvent *event, bool isRelease)
{
    const VtScreen::MouseMode mode = m_screen.mouseMode();
    if (mode == VtScreen::MouseMode::Off || !m_connected)
        return false;
    // 按住 Shift 时强制本地选区，这是 xterm 的惯例
    if (event->modifiers().testFlag(Qt::ShiftModifier))
        return false;

    int button = 0;
    switch (event->button()) {
    case Qt::LeftButton:
        button = 0;
        break;
    case Qt::MiddleButton:
        button = 1;
        break;
    case Qt::RightButton:
        button = 2;
        break;
    default:
        if (event->type() == QEvent::MouseMove)
            button = 3; // 纯移动（无按键）
        break;
    }

    int modifiers = 0;
    if (event->modifiers().testFlag(Qt::ShiftModifier))
        modifiers |= 4;
    if (event->modifiers().testFlag(Qt::AltModifier))
        modifiers |= 8;
    if (event->modifiers().testFlag(Qt::ControlModifier))
        modifiers |= 16;

    const int row = rowAt(int(event->position().y()));
    const int col = colAt(int(event->position().x()));
    const quint64 code = quint64(button | modifiers | (isRelease && !m_screen.sgrMouse() ? 3 : 0));

    if (m_screen.sgrMouse()) {
        const QByteArray sequence = "\x1b[<" + QByteArray::number(code) + ";"
            + QByteArray::number(col + 1) + ";" + QByteArray::number(row + 1)
            + (isRelease ? "m" : "M");
        sendBytes(sequence);
    } else {
        QByteArray sequence = "\x1b[M";
        sequence.append(char(32 + int(code & 0xFF)));
        sequence.append(char(32 + std::min(255, col + 1)));
        sequence.append(char(32 + std::min(255, row + 1)));
        sendBytes(sequence);
    }
    return true;
}

bool TerminalWidget::reportWheel(QWheelEvent *event)
{
    if (m_screen.mouseMode() == VtScreen::MouseMode::Off || !m_connected)
        return false;
    if (event->modifiers().testFlag(Qt::ShiftModifier))
        return false;
    const int delta = event->angleDelta().y();
    if (delta == 0)
        return false;
    const int button = delta > 0 ? 64 : 65;
    int modifiers = 0;
    if (event->modifiers().testFlag(Qt::AltModifier))
        modifiers |= 8;
    if (event->modifiers().testFlag(Qt::ControlModifier))
        modifiers |= 16;
    const int row = rowAt(int(event->position().y()));
    const int col = colAt(int(event->position().x()));
    if (m_screen.sgrMouse()) {
        sendBytes("\x1b[<" + QByteArray::number(button | modifiers) + ";"
                  + QByteArray::number(col + 1) + ";" + QByteArray::number(row + 1) + "M");
    } else {
        QByteArray sequence = "\x1b[M";
        sequence.append(char(32 + (button | modifiers)));
        sequence.append(char(32 + std::min(255, col + 1)));
        sequence.append(char(32 + std::min(255, row + 1)));
        sendBytes(sequence);
    }
    return true;
}

void TerminalWidget::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        if (delta != 0)
            emit zoomRequested(delta > 0 ? 1 : -1);
        event->accept();
        return;
    }
    if (reportWheel(event)) {
        event->accept();
        return;
    }
    const QPoint pixel = event->pixelDelta();
    if (!pixel.isNull()) {
        // 触控板：按像素滚动，手感更顺
        if (pixel.y() != 0)
            scrollBy(int(std::lround(-pixel.y() / m_cellH)) != 0
                         ? int(std::lround(-pixel.y() / m_cellH))
                         : (pixel.y() > 0 ? -1 : 1));
    } else if (delta != 0) {
        scrollBy(delta > 0 ? 3 : -3);
    }
    event->accept();
}

void TerminalWidget::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    m_hasFocus = true;
    // 告诉输入法框架（fcitx5 / ibus）这个控件现在可以输入，并把光标位置给它，
    // 否则中文候选框不知道在哪、甚至根本不激活。
    if (QGuiApplication::inputMethod()) {
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        QGuiApplication::inputMethod()->setVisible(m_connected);
    }
    update();
}

void TerminalWidget::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_scrollBarHovered) {
        m_scrollBarHovered = false;
        update();
    }
}

void TerminalWidget::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    m_hasFocus = false;
    update();
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *copyAction = menu.addAction(QStringLiteral("复制"));
    copyAction->setEnabled(hasSelection());
    QAction *pasteAction = menu.addAction(QStringLiteral("粘贴"));
    menu.addSeparator();
    menu.addAction(QStringLiteral("全选"), this, &TerminalWidget::selectAll);
    menu.addAction(QStringLiteral("清屏"), this, &TerminalWidget::clearScreen);
    QAction *chosen = menu.exec(event->globalPos());
    if (chosen == copyAction)
        copySelection();
    else if (chosen == pasteAction)
        pasteClipboard();
}

} // namespace termdock
