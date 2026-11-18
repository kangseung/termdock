#include "ui/DataDirDialog.h"

#include "app/AppConfig.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace termdock {

DataDirDialog::DataDirDialog(const QString &defaultDir, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("选择数据保存目录"));
    setMinimumWidth(560);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("termdock 把会话、密钥和主机指纹存在哪？"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.5);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *description = new QLabel(
        QStringLiteral("默认放在软件自身目录下的 termdock-data 里，这样整个软件目录可以直接拷到 U 盘"
                       "或另一台机器继续用，不用重新录入账号密码。\n"
                       "目录必须可写；会话密码会用 AES-256-GCM 加密后保存。"),
        this);
    description->setWordWrap(true);
    description->setObjectName(QStringLiteral("hint"));
    layout->addWidget(description);

    auto *pathRow = new QHBoxLayout();
    m_path = new QLineEdit(defaultDir, this);
    auto *browseButton = new QPushButton(QStringLiteral("浏览…"), this);
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browseButton);
    layout->addLayout(pathRow);

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    m_hint->setObjectName(QStringLiteral("hint"));
    layout->addWidget(m_hint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("就用这个目录"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primary"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("使用默认"));
    layout->addWidget(buttons);

    connect(browseButton, &QPushButton::clicked, this, &DataDirDialog::browse);
    connect(m_path, &QLineEdit::textChanged, this, [this]() { updateHint(); });
    connect(buttons, &QDialogButtonBox::accepted, this, &DataDirDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &DataDirDialog::reject);

    updateHint();
}

QString DataDirDialog::dataDir() const
{
    const QString text = m_path->text().trimmed();
    return text.isEmpty() ? QString() : QDir::cleanPath(text);
}

void DataDirDialog::browse()
{
    const QString chosen = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择数据保存目录"), m_path->text().trimmed());
    if (!chosen.isEmpty())
        m_path->setText(QDir::cleanPath(chosen));
}

void DataDirDialog::updateHint()
{
    const QString dir = dataDir();
    if (dir.isEmpty()) {
        m_hint->setText(QStringLiteral("请填写一个目录。"));
        return;
    }
    const bool writable = AppConfig::isDirWritable(dir);
    m_hint->setText(writable ? QStringLiteral("✓ 目录可用：%1").arg(dir)
                             : QStringLiteral("✗ 目录不可写，请换一个：%1").arg(dir));
}

void DataDirDialog::accept()
{
    if (!AppConfig::isDirWritable(dataDir())) {
        QMessageBox::warning(this, QStringLiteral("目录不可写"),
                             QStringLiteral("这个目录写不进去，请换一个位置。"));
        return;
    }
    QDialog::accept();
}

} // namespace termdock
