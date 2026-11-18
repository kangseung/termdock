#pragma once

#include <QWidget>

#include "session/Session.h"

class QLineEdit;
class QListWidget;
class QPushButton;

namespace termdock {

// 左侧会话簿：双击即连。列表用自绘 delegate，显示名称 + 账号@主机 + 颜色点。
class SessionPanel : public QWidget {
    Q_OBJECT

public:
    explicit SessionPanel(QWidget *parent = nullptr);

    void setSessions(const SessionList &sessions);
    void setActiveSessionId(const QString &id);
    QString currentSessionId() const;
    void selectSession(const QString &id);
    void focusSearch();

signals:
    void connectRequested(const QString &id);
    void newRequested();
    void editRequested(const QString &id);
    void deleteRequested(const QString &id);
    void duplicateRequested(const QString &id);

private:
    void setupUi();
    void rebuild();
    const Session *findSession(const QString &id) const;
    void showContextMenu(const QPoint &pos);

    QLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    QPushButton *m_newButton = nullptr;
    SessionList m_sessions;
    QString m_activeId;
};

} // namespace termdock
