#pragma once

#include <QDialog>

#include "session/Session.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

namespace termdock {

// 新建 / 编辑会话。
class SessionDialog : public QDialog {
    Q_OBJECT

public:
    explicit SessionDialog(const Session &session, QWidget *parent = nullptr);

    Session session() const;

private:
    // 主机栏里粘 “账号@主机:端口” 时自动拆到账号/端口/名称里
    void parseHostField();
    void setupUi();
    void accept() override;

    Session m_session;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_host = nullptr;
    QSpinBox *m_port = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_password = nullptr;
    QCheckBox *m_showPassword = nullptr;
    QLineEdit *m_note = nullptr;
    QComboBox *m_color = nullptr;
    Session m_original;
};

} // namespace termdock
