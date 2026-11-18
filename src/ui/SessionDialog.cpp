#include "ui/SessionDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace termdock {

SessionDialog::SessionDialog(const Session &session, QWidget *parent)
    : QDialog(parent)
    , m_original(session)
{
    m_session = session;
    if (m_session.id.isEmpty())
        m_session = Session::createEmpty();
    setWindowTitle(session.host.isEmpty() ? QStringLiteral("新建会话") : QStringLiteral("编辑会话"));
    setMinimumWidth(420);
    setupUi();
}

void SessionDialog::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_name = new QLineEdit(m_session.name, this);
    m_name->setPlaceholderText(QStringLiteral("例如：生产环境 / 家里 NAS"));
    form->addRow(QStringLiteral("名称"), m_name);

    m_host = new QLineEdit(m_session.host, this);
    m_host->setPlaceholderText(QStringLiteral("IP 或域名"));
    form->addRow(QStringLiteral("主机"), m_host);

    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(m_session.port > 0 ? m_session.port : 22);
    form->addRow(QStringLiteral("端口"), m_port);

    m_user = new QLineEdit(m_session.user, this);
    form->addRow(QStringLiteral("账号"), m_user);

    m_password = new QLineEdit(m_session.password, this);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(QStringLiteral("只保存在本机，AES-256-GCM 加密"));
    m_showPassword = new QCheckBox(QStringLiteral("显示"), this);
    auto *passwordRow = new QWidget(this);
    auto *passwordLayout = new QHBoxLayout(passwordRow);
    passwordLayout->setContentsMargins(0, 0, 0, 0);
    passwordLayout->addWidget(m_password, 1);
    passwordLayout->addWidget(m_showPassword);
    form->addRow(QStringLiteral("密码"), passwordRow);

    m_color = new QComboBox(this);
    const QStringList colors = {QStringLiteral("#7aa2f7"), QStringLiteral("#9ece6a"),
                                QStringLiteral("#e0af68"), QStringLiteral("#f7768e"),
                                QStringLiteral("#bb9af7"), QStringLiteral("#7dcfff")};
    const QStringList names = {QStringLiteral("蓝"), QStringLiteral("绿"), QStringLiteral("橙"),
                               QStringLiteral("红"), QStringLiteral("紫"), QStringLiteral("青")};
    for (int i = 0; i < colors.size(); ++i) {
        m_color->addItem(names.at(i), colors.at(i));
        if (colors.at(i) == m_session.color)
            m_color->setCurrentIndex(i);
    }
    form->addRow(QStringLiteral("标记色"), m_color);

    m_note = new QLineEdit(m_session.note, this);
    m_note->setPlaceholderText(QStringLiteral("可选，例如“跳板机 / 只读”"));
    form->addRow(QStringLiteral("备注"), m_note);

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primary"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &SessionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SessionDialog::reject);
    connect(m_showPassword, &QCheckBox::toggled, this, [this](bool checked) {
        m_password->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(m_host, &QLineEdit::returnPressed, this, &SessionDialog::accept);
}

void SessionDialog::accept()
{
    if (m_host->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少主机"), QStringLiteral("请填写主机地址。"));
        m_host->setFocus();
        return;
    }
    if (m_user->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少账号"), QStringLiteral("请填写登录账号。"));
        m_user->setFocus();
        return;
    }
    QDialog::accept();
}

Session SessionDialog::session() const
{
    Session result = m_session;
    result.name = m_name->text().trimmed();
    if (result.name.isEmpty())
        result.name = m_host->text().trimmed();
    result.host = m_host->text().trimmed();
    result.port = m_port->value();
    result.user = m_user->text().trimmed();
    result.password = m_password->text();
    result.note = m_note->text().trimmed();
    result.color = m_color->currentData().toString();
    if (result.id.isEmpty())
        result.id = Session::newId();
    return result;
}

} // namespace termdock
