#include "ui/SessionPanel.h"

#include "app/Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace termdock {

namespace {

constexpr int kSubtitleRole = Qt::UserRole + 1;
constexpr int kColorRole = Qt::UserRole + 2;
constexpr int kActiveRole = Qt::UserRole + 3;

class SessionItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        Q_UNUSED(option);
        Q_UNUSED(index);
        return QSize(0, 52);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const QRectF card = QRectF(option.rect).adjusted(3, 3, -3, -3);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool active = index.data(kActiveRole).toBool();

        if (selected) {
            painter->setBrush(QColor(0x26, 0x30, 0x43));
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(card, 7, 7);
        } else if (hovered) {
            painter->setBrush(QColor(0x1d, 0x22, 0x2a));
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(card, 7, 7);
        }

        // 左侧色点：正在连接的会话用主色，其余用会话自己的颜色
        const QColor dotColor = active ? Theme::instance().accent()
                                       : QColor(index.data(kColorRole).toString());
        painter->setBrush(dotColor.isValid() ? dotColor : Theme::instance().mutedText());
        painter->setPen(Qt::NoPen);
        painter->drawEllipse(QPointF(card.left() + 12, card.center().y()), 4.0, 4.0);

        QFont nameFont = option.font;
        nameFont.setBold(true);
        if (nameFont.pointSizeF() > 0)
            nameFont.setPointSizeF(nameFont.pointSizeF() + 0.5);
        else if (nameFont.pixelSize() > 0)
            nameFont.setPixelSize(nameFont.pixelSize() + 1);
        painter->setFont(nameFont);
        painter->setPen(selected ? QColor(0xe7, 0xec, 0xf5) : QColor(0xd6, 0xdb, 0xe4));
        const QRectF nameRect(card.left() + 24, card.top() + 6, card.width() - 30, 18);
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(),
                                                            Qt::ElideRight, int(nameRect.width())));

        QFont subFont = option.font;
        if (subFont.pointSizeF() > 1.0)
            subFont.setPointSizeF(std::max(7.5, subFont.pointSizeF() - 1.0));
        else if (subFont.pixelSize() > 1)
            subFont.setPixelSize(std::max(10, subFont.pixelSize() - 1));
        painter->setFont(subFont);
        painter->setPen(QColor(0x7d, 0x86, 0x96));
        const QRectF subRect(card.left() + 24, card.top() + 24, card.width() - 30, 16);
        painter->drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(index.data(kSubtitleRole).toString(),
                                                            Qt::ElideRight, int(subRect.width())));
        painter->restore();
    }
};

} // namespace

SessionPanel::SessionPanel(QWidget *parent)
    : QWidget(parent)
{
    setupUi();
}

void SessionPanel::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto *header = new QHBoxLayout();
    auto *title = new QLabel(QStringLiteral("会话"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    m_newButton = new QPushButton(QStringLiteral("+"), this);
    m_newButton->setObjectName(QStringLiteral("newSession"));
    m_newButton->setToolTip(QStringLiteral("新建会话"));
    m_newButton->setFixedSize(30, 26);
    m_newButton->setCursor(Qt::PointingHandCursor);
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(m_newButton);
    layout->addLayout(header);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("搜索名称 / 主机 / 账号"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);

    m_list = new QListWidget(this);
    m_list->setItemDelegate(new SessionItemDelegate(m_list));
    m_list->setMouseTracking(true);
    m_list->setUniformItemSizes(true);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_list, 1);

    connect(m_newButton, &QPushButton::clicked, this, &SessionPanel::newRequested);
    connect(m_search, &QLineEdit::textChanged, this, [this]() { rebuild(); });
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (item)
            emit connectRequested(item->data(Qt::UserRole).toString());
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item)
            emit connectRequested(item->data(Qt::UserRole).toString());
    });
    connect(m_list, &QWidget::customContextMenuRequested, this, &SessionPanel::showContextMenu);
}

const Session *SessionPanel::findSession(const QString &id) const
{
    for (const Session &session : m_sessions) {
        if (session.id == id)
            return &session;
    }
    return nullptr;
}

void SessionPanel::setSessions(const SessionList &sessions)
{
    m_sessions = sessions;
    rebuild();
}

void SessionPanel::setActiveSessionId(const QString &id)
{
    m_activeId = id;
    rebuild();
}

QString SessionPanel::currentSessionId() const
{
    QListWidgetItem *item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void SessionPanel::selectSession(const QString &id)
{
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        if (item->data(Qt::UserRole).toString() == id) {
            m_list->setCurrentItem(item);
            return;
        }
    }
}

void SessionPanel::focusSearch()
{
    m_search->setFocus(Qt::ShortcutFocusReason);
    m_search->selectAll();
}

void SessionPanel::rebuild()
{
    const QString filter = m_search->text().trimmed();
    const QString previous = currentSessionId();
    m_list->clear();
    for (const Session &session : m_sessions) {
        if (!filter.isEmpty()) {
            const QString haystack = session.name + QLatin1Char(' ') + session.host
                + QLatin1Char(' ') + session.user + QLatin1Char(' ') + session.note;
            if (!haystack.contains(filter, Qt::CaseInsensitive))
                continue;
        }
        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::DisplayRole, session.name);
        item->setData(kSubtitleRole, session.displaySubtitle());
        item->setData(kColorRole, session.color);
        item->setData(kActiveRole, session.id == m_activeId);
        item->setData(Qt::UserRole, session.id);
        item->setToolTip(session.displaySubtitle());
    }
    if (!previous.isEmpty())
        selectSession(previous);
    else if (m_list->count() > 0)
        m_list->setCurrentRow(0);
}

void SessionPanel::showContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_list->itemAt(pos);
    if (!item)
        return;
    const QString id = item->data(Qt::UserRole).toString();
    QMenu menu(this);
    QAction *connectAction = menu.addAction(QStringLiteral("连接"));
    menu.addSeparator();
    QAction *editAction = menu.addAction(QStringLiteral("编辑…"));
    QAction *duplicateAction = menu.addAction(QStringLiteral("复制一份"));
    QAction *deleteAction = menu.addAction(QStringLiteral("删除"));
    QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
    if (chosen == connectAction)
        emit connectRequested(id);
    else if (chosen == editAction)
        emit editRequested(id);
    else if (chosen == duplicateAction)
        emit duplicateRequested(id);
    else if (chosen == deleteAction)
        emit deleteRequested(id);
}

} // namespace termdock
