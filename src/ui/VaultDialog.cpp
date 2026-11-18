#include "ui/VaultDialog.h"

#include "app/AppConfig.h"
#include "session/Crypto.h"
#include "session/Recovery.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace termdock {

namespace {

QJsonArray loadRawSessions()
{
    QFile file(AppConfig::sessionsFile());
    if (!file.open(QIODevice::ReadOnly))
        return QJsonArray();
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.object().value(QStringLiteral("sessions")).toArray();
}

} // namespace

VaultDialog::VaultDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("已保存的密码"));
    setMinimumSize(720, 460);
    setupUi();
    refreshState();
}

void VaultDialog::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    m_stateLabel = new QLabel(this);
    QFont stateFont = m_stateLabel->font();
    stateFont.setBold(true);
    m_stateLabel->setFont(stateFont);
    layout->addWidget(m_stateLabel);

    auto *unlockRow = new QHBoxLayout();
    m_passphrase = new QLineEdit(this);
    m_passphrase->setEchoMode(QLineEdit::Password);
    m_passphrase->setPlaceholderText(QStringLiteral("恢复口令"));
    m_unlockButton = new QPushButton(QStringLiteral("解锁查看"), this);
    m_unlockButton->setObjectName(QStringLiteral("primary"));
    m_enableButton = new QPushButton(QStringLiteral("启用恢复保险…"), this);
    m_disableButton = new QPushButton(QStringLiteral("关闭恢复保险"), this);
    unlockRow->addWidget(m_passphrase, 1);
    unlockRow->addWidget(m_unlockButton);
    unlockRow->addWidget(m_enableButton);
    unlockRow->addWidget(m_disableButton);
    layout->addLayout(unlockRow);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("hint"));
    m_hint->setWordWrap(true);
    m_hint->setText(QStringLiteral(
        "说明：会话密码以 AES-256-GCM 加密保存在数据目录，日常使用不需要口令。"
        "这里只在你想翻出「当时存的明文密码」时用；解锁结果只留在内存，不写盘。"));
    layout->addWidget(m_hint);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("账号 @ 主机"),
                                        QStringLiteral("密码"), QStringLiteral("备注")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    layout->addWidget(m_table, 1);

    auto *bottom = new QHBoxLayout();
    m_showPlain = new QCheckBox(QStringLiteral("显示明文（默认遮罩）"), this);
    auto *copyButton = new QPushButton(QStringLiteral("复制所选密码"), this);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    bottom->addWidget(m_showPlain);
    bottom->addWidget(copyButton);
    bottom->addStretch(1);
    bottom->addWidget(buttons);
    layout->addLayout(bottom);

    connect(m_enableButton, &QPushButton::clicked, this, &VaultDialog::enableRecovery);
    connect(m_disableButton, &QPushButton::clicked, this, &VaultDialog::disableRecovery);
    connect(m_unlockButton, &QPushButton::clicked, this, &VaultDialog::unlock);
    connect(m_passphrase, &QLineEdit::returnPressed, this, &VaultDialog::unlock);
    connect(m_showPlain, &QCheckBox::toggled, this, [this]() { applyMask(); });
    connect(copyButton, &QPushButton::clicked, this, [this]() {
        const int row = m_table->currentRow();
        if (row >= 0 && row < m_secrets.size()) {
            QGuiApplication::clipboard()->setText(m_secrets.at(row));
            m_hint->setText(QStringLiteral("已复制第 %1 行的密码到剪贴板。").arg(row + 1));
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

bool VaultDialog::promptEnable(QWidget *parent)
{
    bool ok = false;
    const QString first = QInputDialog::getText(
        parent, QStringLiteral("启用恢复保险"),
        QStringLiteral("设置一个恢复口令（至少 6 位）。\n以后凭它就能导出已保存的明文密码，请自己记牢："),
        QLineEdit::Password, QString(), &ok);
    if (!ok || first.isEmpty())
        return false;
    const QString second = QInputDialog::getText(parent, QStringLiteral("确认口令"),
                                                 QStringLiteral("再输入一次："), QLineEdit::Password,
                                                 QString(), &ok);
    if (!ok)
        return false;
    if (first != second) {
        QMessageBox::warning(parent, QStringLiteral("口令不一致"),
                             QStringLiteral("两次输入的口令不一致，未开启。"));
        return false;
    }
    QString error;
    if (!Recovery::enable(first, &error)) {
        QMessageBox::warning(parent, QStringLiteral("开启失败"), error);
        return false;
    }
    QMessageBox::information(parent, QStringLiteral("已开启"),
                             QStringLiteral("恢复保险已开启，恢复数据保存在：\n%1").arg(Recovery::filePath()));
    return true;
}

void VaultDialog::refreshState()
{
    const bool on = Recovery::enabled();
    m_stateLabel->setText(on ? QStringLiteral("恢复保险：已开启")
                             : QStringLiteral("恢复保险：未开启"));
    m_passphrase->setEnabled(on);
    m_unlockButton->setEnabled(on);
    m_enableButton->setVisible(!on);
    m_disableButton->setVisible(on);
    if (!on) {
        m_hint->setText(QStringLiteral(
            "当前未开启恢复保险：密码仍然加密保存，但主密钥文件（key）一旦丢失，历史密码就无法解开。"
            "点「启用恢复保险…」设置一个恢复口令即可多一层保障，随时可关闭。"));
        clearTable();
    }
}

void VaultDialog::enableRecovery()
{
    if (promptEnable(this)) {
        refreshState();
        unlock();
    }
}

void VaultDialog::disableRecovery()
{
    if (QMessageBox::question(this, QStringLiteral("关闭恢复保险"),
                              QStringLiteral("关闭后会删除恢复数据（recovery.json），之后只能靠 key 文件解密。确定？"))
        != QMessageBox::Yes)
        return;
    Recovery::disable();
    clearTable();
    refreshState();
    m_hint->setText(QStringLiteral("恢复保险已关闭。"));
}

void VaultDialog::clearTable()
{
    m_table->setRowCount(0);
    m_secrets.clear();
}

void VaultDialog::unlock()
{
    QString error;
    QByteArray masterKey;
    if (!Recovery::unlock(m_passphrase->text(), &masterKey, &error)) {
        QMessageBox::warning(this, QStringLiteral("解锁失败"), error);
        return;
    }
    loadTable(masterKey);
    m_hint->setText(QStringLiteral("已解锁，共 %1 条会话。密码默认遮罩显示，勾选下方即可看明文。")
                        .arg(m_table->rowCount()));
}

void VaultDialog::loadTable(const QByteArray &masterKey)
{
    clearTable();
    const QJsonArray sessions = loadRawSessions();
    for (const QJsonValue &value : sessions) {
        if (!value.isObject())
            continue;
        const QJsonObject entry = value.toObject();
        QString plain;
        const QString encrypted = entry.value(QStringLiteral("password")).toString();
        if (!encrypted.isEmpty()) {
            plain = QString::fromUtf8(
                Crypto::decryptWithKey(masterKey, encrypted, nullptr));
        }
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(entry.value(QStringLiteral("name")).toString()));
        const QString target = QStringLiteral("%1@%2:%3")
                                   .arg(entry.value(QStringLiteral("user")).toString(),
                                        entry.value(QStringLiteral("host")).toString(),
                                        QString::number(entry.value(QStringLiteral("port")).toInt(22)));
        m_table->setItem(row, 1, new QTableWidgetItem(target));
        m_table->setItem(row, 2, new QTableWidgetItem(plain.isEmpty() ? QStringLiteral("—") : plain));
        m_table->setItem(row, 3, new QTableWidgetItem(entry.value(QStringLiteral("note")).toString()));
        m_secrets.append(plain);
    }
    applyMask();
}

void VaultDialog::applyMask()
{
    const bool show = m_showPlain->isChecked();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem *item = m_table->item(row, 2);
        if (!item)
            continue;
        const QString plain = m_secrets.value(row);
        if (plain.isEmpty())
            item->setText(QStringLiteral("—"));
        else
            item->setText(show ? plain : QStringLiteral("••••••••"));
    }
}

} // namespace termdock
