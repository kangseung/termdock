#pragma once

#include <QByteArray>
#include <QString>
#include <QWidget>

#include "term/VtParser.h"
#include "term/VtScreen.h"

class QTimer;

namespace termdock {

// 终端视图：负责绘制 VtScreen、处理键鼠输入和选区复制。
class TerminalWidget : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget() override;

    VtScreen *screen() { return &m_screen; }
    void feed(const QByteArray &data);

    void setConnected(bool connected);
    void setStatusText(const QString &text);
    void setFallbackTitle(const QString &title);

    void copySelection();
    void pasteClipboard();
    void selectAll();
    void clearScreen();

    void applyFont(); // 字号/字体变化后重算度量

    QSize sizeHint() const override { return QSize(800, 480); }

signals:
    void dataToSend(const QByteArray &data);
    void gridResized(int cols, int rows);
    void titleChanged(const QString &title);
    void bell();
    void zoomRequested(int delta);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    // Tab 必须留给远端 shell 做补全，不能被 Qt 拿去切焦点
    bool focusNextPrevChild(bool next) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void recomputeGrid();
    void updateMetrics();
    void sendKeyText(const QString &text);
    void sendBytes(const QByteArray &bytes);
    int rowAt(int y) const;
    int colAt(int x) const;
    void clampSelection();
    QString selectionText() const;
    bool hasSelection() const { return m_selecting && (m_selStartLine != m_selEndLine || m_selStartCol != m_selEndCol); }
    void selectWordAt(int line, int col);
    void selectLineAt(int line);
    void scrollBy(int lines);
    // 自绘滚动条
    QRectF scrollBarTrack() const;
    QRectF scrollBarThumb() const;
    bool scrollBarVisible() const;
    void scrollToOffsetFromY(qreal y);
    // 鼠标上报（xterm 1000/1002/1003 + SGR 1006）
    bool reportMouse(QMouseEvent *event, bool isRelease);
    bool reportWheel(QWheelEvent *event);

    VtScreen m_screen;
    VtParser m_parser;

    QFont m_font;
    QFont m_boldFont;
    qreal m_cellW = 8;
    qreal m_cellH = 16;
    qreal m_ascent = 12;
    qreal m_descent = 4;
    qreal m_baseline = 15; // 行内基线偏移（含垂直居中补偿）
    int m_padX = 10;
    int m_padY = 8;

    bool m_connected = false;
    bool m_blinkOn = true;
    bool m_hasFocus = false;
    QString m_statusText;
    QString m_fallbackTitle;
    QString m_preedit;

    // 选区（绝对行号 + 列号）
    bool m_selecting = false;
    int m_selStartLine = 0;
    int m_selStartCol = 0;
    int m_selEndLine = 0;
    int m_selEndCol = 0;
    QPoint m_dragOrigin;
    bool m_mouseReportedPress = false;

    bool m_draggingThumb = false;
    bool m_scrollBarHovered = false;
    qreal m_dragGrabOffset = 0;
};

} // namespace termdock
