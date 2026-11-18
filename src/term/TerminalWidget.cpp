#include "term/TerminalWidget.h"

#include "app/Theme.h"

#include <QApplication>
#include <QClipboard>
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
    setAttribute(Qt::WA_OpaquePaintEvent);
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
    m_cellH = std::ceil(fm.lineSpacing());
    m_ascent = fm.ascent();
}

void TerminalWidget::applyFont()
{
    updateMetrics();
    recomputeGrid();
    update();
}

void TerminalWidget::recomputeGrid()
{
    const int usableW = std::max(1, width() - 2 * m_padX);
    const int usableH = std::max(1, height() - 2 * m_padY);
    const int cols = std::max(kMinCols, int(usableW / m_cellW));
    const int rows = std::max(kMinRows, int(usableH / m_cellH));
    if (cols != m_screen.cols() || rows != m_screen.rows()) {
        m_screen.resize(cols, rows);
        emit gridResized(cols, rows);
    }
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    recomputeGrid();
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

void TerminalWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    const Theme &theme = Theme::instance();
    QPainter p(this);
    p.fillRect(rect(), theme.background());
    p.setRenderHint(QPainter::TextAntialiasing, true);

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
        const qreal y = m_padY + row * m_cellH;
        const qreal baseY = y + m_ascent; // 每行自己的基线，别用统一基线

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
            // 找同色同属性的连续段
            int end = col + width;
            while (end < cols) {
                const VtCell &next = m_screen.cellAtAbsolute(absLine, end);
                QColor nfg = next.fg.resolve(theme.palette(), theme.foreground());
                QColor nbg = next.bg.resolve(theme.palette(), theme.background());
                const quint16 styleBits = next.attr & quint16(AttrBold | AttrDim | AttrItalic
                                                              | AttrUnderline | AttrReverse
                                                              | AttrHidden | AttrStrike);
                if (nbg != bg || nfg != fg || styleBits != (cell.attr & quint16(AttrBold | AttrDim
                                                                                | AttrItalic
                                                                                | AttrUnderline
                                                                                | AttrReverse
                                                                                | AttrHidden
                                                                                | AttrStrike)))
                    break;
                end += (next.attr & AttrWide) ? 2 : 1;
            }

            const qreal x = m_padX + col * m_cellW;
            const qreal w = (end - col) * m_cellW;
            if (bg != theme.background())
                p.fillRect(QRectF(x, y, w, m_cellH), bg);

            // 选区高亮
            if (selVisible) {
                const int from = std::max(col, (absLine == selSl) ? selSc : 0);
                const int to = std::min(end - 1, (absLine == selEl) ? selEc : cols - 1);
                if (absLine >= selSl && absLine <= selEl && to >= from) {
                    p.fillRect(QRectF(m_padX + from * m_cellW, y, (to - from + 1) * m_cellW, m_cellH),
                               theme.selection());
                }
            }

            // 文本：连续的窄字符批量绘制，宽字符单独居中
            if (fg != bg) {
                QString run;
                int runStart = col;
                for (int c = col; c < end; ++c) {
                    const VtCell &cc = m_screen.cellAtAbsolute(absLine, c);
                    if (cc.attr & AttrWideTail)
                        continue;
                    if (cc.attr & AttrWide) {
                        if (!run.isEmpty()) {
                            p.setFont((cell.attr & AttrBold) ? m_boldFont : m_font);
                            p.setPen(fg);
                            p.drawText(QPointF(m_padX + runStart * m_cellW, baseY), run);
                            run.clear();
                        }
                        p.setFont((cell.attr & AttrBold) ? m_boldFont : m_font);
                        p.setPen(fg);
                        p.drawText(QPointF(m_padX + c * m_cellW, baseY),
                                   QString::fromUcs4(&cc.ch, 1));
                        runStart = c + 2;
                        continue;
                    }
                    if (run.isEmpty())
                        runStart = c;
                    run.append(cc.ch ? QString::fromUcs4(&cc.ch, 1) : QStringLiteral(" "));
                }
                if (!run.isEmpty()) {
                    p.setFont((cell.attr & AttrBold) ? m_boldFont : m_font);
                    p.setPen(fg);
                    p.drawText(QPointF(m_padX + runStart * m_cellW, baseY), run);
                }
                if (cell.attr & AttrUnderline) {
                    p.setPen(fg);
                    p.drawLine(QPointF(x, y + m_cellH - 1.5), QPointF(x + w, y + m_cellH - 1.5));
                }
                if (cell.attr & AttrStrike) {
                    p.setPen(fg);
                    p.drawLine(QPointF(x, y + m_cellH * 0.55), QPointF(x + w, y + m_cellH * 0.55));
                }
            }
            col = end;
        }
    }

    // 光标
    const bool showCursor = m_connected && m_hasFocus && m_screen.cursorVisible()
        && m_screen.scrollOffset() == 0 && m_blinkOn;
    if (showCursor) {
        const qreal x = m_padX + m_screen.cursorCol() * m_cellW;
        const qreal y = m_padY + m_screen.cursorRow() * m_cellH;
        QColor cursorColor = theme.cursor();
        cursorColor.setAlpha(200);
        p.fillRect(QRectF(x, y, m_cellW, m_cellH), cursorColor);
        const VtCell &cell = m_screen.cellAtAbsolute(m_screen.absoluteLineOf(m_screen.cursorRow()),
                                                    m_screen.cursorCol());
        if (cell.ch && cell.ch != U' ') {
            p.setFont(m_font);
            p.setPen(theme.background());
            p.drawText(QPointF(x, y + m_ascent), QString::fromUcs4(&cell.ch, 1));
        }
    }

    // 输入法预编辑
    if (!m_preedit.isEmpty() && m_connected) {
        const qreal x = m_padX + m_screen.cursorCol() * m_cellW;
        const qreal y = m_padY + m_screen.cursorRow() * m_cellH;
        const QFontMetricsF fm(m_font);
        const qreal w = fm.horizontalAdvance(m_preedit) + 4;
        p.fillRect(QRectF(x, y, w, m_cellH), theme.panelBackground());
        p.setFont(m_font);
        p.setPen(theme.accent());
        p.drawText(QPointF(x + 2, y + m_ascent), m_preedit);
        p.drawLine(QPointF(x + 2, y + m_cellH - 2), QPointF(x + w - 2, y + m_cellH - 2));
    }

    // 未连接提示
    if (!m_connected) {
        p.setFont(QFont(theme.terminalFont().family(), 12));
        p.setPen(theme.mutedText());
        const QString text = m_statusText.isEmpty() ? QStringLiteral("未连接") : m_statusText;
        p.drawText(rect(), Qt::AlignCenter, text);
    } else if (m_screen.scrollOffset() > 0) {
        p.setFont(QFont(theme.terminalFont().family(), 9));
        p.setPen(theme.mutedText());
        p.drawText(QRect(m_padX, height() - m_padY - 14, width() - 2 * m_padX, 14),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("滚回 %1 行 · 滚到底部继续").arg(m_screen.scrollOffset()));
    }
}

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    const bool alt = mods.testFlag(Qt::AltModifier);

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
    if (query == Qt::ImCursorRectangle) {
        const QRect r(int(m_padX + m_screen.cursorCol() * m_cellW),
                      int(m_padY + m_screen.cursorRow() * m_cellH), int(m_cellW), int(m_cellH));
        return r;
    }
    if (query == Qt::ImEnabled)
        return m_connected;
    return QWidget::inputMethodQuery(query);
}

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
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

void TerminalWidget::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        if (delta != 0)
            emit zoomRequested(delta > 0 ? 1 : -1);
        event->accept();
        return;
    }
    if (delta > 0)
        scrollBy(3);
    else if (delta < 0)
        scrollBy(-3);
    event->accept();
}

void TerminalWidget::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    m_hasFocus = true;
    update();
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
