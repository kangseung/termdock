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
    // 粘 “用户@主机” 这种（ssh 命令里那一段）时，离开主机栏就自动拆好
    connect(m_host, &QLineEdit::editingFinished, this, &SessionDialog::parseHostField);
    parseHostField();
}

// 主机栏支持三种写法，都会拆成 账号 / 主机 / 端口：
//   用户@主机        用户@主机:2222        ssh://root@example.com:2200
// 名称留空时自动填主机，账号留空时用拆出来的账号
void SessionDialog::parseHostField()
{
    QString text = m_host->text().trimmed();
    if (text.isEmpty())
        return;
    if (text.startsWith(QLatin1String("ssh://"), Qt::CaseInsensitive))
        text = text.mid(6);
    if (text.isEmpty())
        return;

    QString user;
    const int at = text.lastIndexOf(QLatin1Char('@'));
    if (at >= 0) {
        user = text.left(at);
        text = text.mid(at + 1);
    }

    QString host = text;
    int port = 0;
    if (host.startsWith(QLatin1Char('['))) {           // [::1]:2222
        const int close = host.indexOf(QLatin1Char(']'));
        if (close > 0) {
            const QString rest = host.mid(close + 1);
            host = host.mid(1, close - 1);
            if (rest.startsWith(QLatin1Char(':')))
                port = rest.mid(1).toInt();
        }
    } else if (host.count(QLatin1Char(':')) == 1) {    // host:2222（不加括号的是 IPv6，不动它）
        const int colon = host.indexOf(QLatin1Char(':'));
        const QString portText = host.mid(colon + 1);
        bool ok = false;
        const int parsed = portText.toInt(&ok);
        if (ok && parsed > 0 && parsed <= 65535) {
            port = parsed;
            host = host.left(colon);
        }
    }

    host = host.trimmed();
    if (host.isEmpty())
        return;

    const QString oldHost = m_host->text().trimmed();
    m_host->setText(host);
    if (port > 0)
        m_port->setValue(port);
    if (!user.trimmed().isEmpty())
        m_user->setText(user.trimmed());
    // 名称没自己起过（空的 / 还是默认的“新会话” / 就是原来那串主机）→ 用纯主机地址；
    // 用户自己起过名字的会话不动它，免得改个主机把名字冲掉
    const QString name = m_name->text().trimmed();
    const bool untouchedName = name.isEmpty() || name == QStringLiteral("新会话")
                               || name == oldHost || name == m_session.host
                               || name == m_session.name;
    if (untouchedName)
        m_name->setText(host);
}

void SessionDialog::accept()
{
    parseHostField();
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
