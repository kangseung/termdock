#include "ui/FileTreePanel.h"

#include "ssh/SftpModel.h"
#include "ssh/SftpWorker.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QStandardPaths>
#include <QToolBar>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace termdock {

FileTreePanel::FileTreePanel(QWidget *parent)
    : QWidget(parent)
{
    setupUi();
}

void FileTreePanel::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_toolbar = new QToolBar(this);
    m_toolbar->setIconSize(QSize(16, 16));
    m_toolbar->addAction(QStringLiteral("刷新"), this, &FileTreePanel::refresh);
    m_toolbar->addAction(QStringLiteral("新建目录"), this, &FileTreePanel::makeDirectory);
    m_toolbar->addAction(QStringLiteral("上传"), this, &FileTreePanel::uploadFiles);
    m_toolbar->addAction(QStringLiteral("下载"), this, &FileTreePanel::downloadSelection);
    m_toolbar->addAction(QStringLiteral("重命名"), this, &FileTreePanel::renameSelection);
    m_toolbar->addAction(QStringLiteral("删除"), this, &FileTreePanel::removeSelection);
    layout->addWidget(m_toolbar);

    m_view = new QTreeView(this);
    m_view->setUniformRowHeights(true);
    m_view->setAnimated(false);
    m_view->setAlternatingRowColors(false);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->header()->setStretchLastSection(false);
    layout->addWidget(m_view, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("hint"));
    m_status->setContentsMargins(8, 4, 8, 6);
    m_status->setText(QStringLiteral("未连接"));
    layout->addWidget(m_status);

    connect(m_view, &QTreeView::doubleClicked, this, &FileTreePanel::openSelection);

    updateActions();
}

void FileTreePanel::attach(SftpModel *model, SftpWorker *worker)
{
    detach();
    m_model = model;
    m_worker = worker;
    m_view->setModel(model);
    if (model) {
        // 必须在设置 model 之后再配列宽，否则 Qt 会因为 section 数为 0 而崩溃
        m_view->header()->setSectionResizeMode(SftpModel::NameColumn, QHeaderView::Stretch);
        m_view->header()->setSectionResizeMode(SftpModel::SizeColumn, QHeaderView::ResizeToContents);
        m_view->header()->setSectionResizeMode(SftpModel::ModifiedColumn,
                                              QHeaderView::ResizeToContents);
        m_view->header()->setSectionResizeMode(SftpModel::ModeColumn, QHeaderView::ResizeToContents);
        m_view->expandAll();
        if (QItemSelectionModel *selection = m_view->selectionModel()) {
            connect(selection, &QItemSelectionModel::currentChanged, this,
                    [this](const QModelIndex &, const QModelIndex &) { updateActions(); });
        }
    }
    if (model && worker) {
        connect(worker, &SftpWorker::progressed, this,
                [this](quint64 id, qint64 done, qint64 total) {
                    Q_UNUSED(id);
                    if (total > 0) {
                        setStatus(QStringLiteral("传输中 %1 / %2 (%3%)")
                                      .arg(done / 1024)
                                      .arg(total / 1024)
                                      .arg(int(done * 100 / total)));
                    }
                });
        connect(worker, &SftpWorker::succeeded, this,
                [this](quint64 id, const QString &message) {
                    setStatus(message);
                    const QString localPath = m_pendingDownloads.take(id);
                    if (!localPath.isEmpty())
                        QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
                });
        connect(worker, &SftpWorker::failedOp, this, [this](quint64, const QString &message) {
            setStatus(message);
        });
        setStatus(QStringLiteral("已连接，等待目录加载…"));
    }
    updateActions();
}

void FileTreePanel::detach()
{
    m_view->setModel(nullptr);
    m_model = nullptr;
    m_worker = nullptr;
    m_pendingDownloads.clear();
    setStatus(QStringLiteral("未连接"));
    updateActions();
}

void FileTreePanel::setStatus(const QString &message)
{
    m_status->setText(message);
}

void FileTreePanel::updateActions()
{
    const bool enabled = m_model != nullptr && m_worker != nullptr;
    for (QAction *action : m_toolbar->actions())
        action->setEnabled(enabled);
    const bool hasSelection = enabled && m_view->currentIndex().isValid();
    if (m_toolbar->actions().size() >= 7) {
        m_toolbar->actions().at(3)->setEnabled(hasSelection);
        m_toolbar->actions().at(4)->setEnabled(hasSelection);
        m_toolbar->actions().at(5)->setEnabled(hasSelection);
    }
}

QString FileTreePanel::currentDirectory() const
{
    if (!m_model)
        return QString();
    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid())
        return m_model->homePath();
    const QString path = m_model->pathForIndex(current);
    if (m_model->isDirIndex(current))
        return path;
    return QFileInfo(path).path();
}

void FileTreePanel::refresh()
{
    if (!m_model)
        return;
    const QModelIndex current = m_view->currentIndex();
    if (current.isValid() && m_model->isDirIndex(current))
        m_model->refresh(current);
    else
        m_model->refresh(QModelIndex());
    setStatus(QStringLiteral("正在刷新…"));
}

void FileTreePanel::makeDirectory()
{
    if (!m_model || !m_worker)
        return;
    const QString base = currentDirectory();
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("新建目录"),
                                               QStringLiteral("目录名："), QLineEdit::Normal,
                                               QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    m_worker->enqueue(SftpWorker::Op::Mkdir, base + QLatin1Char('/') + name.trimmed());
    setStatus(QStringLiteral("已请求创建 %1，稍后刷新查看").arg(name.trimmed()));
}

void FileTreePanel::uploadFiles()
{
    if (!m_model || !m_worker)
        return;
    const QString base = currentDirectory();
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("上传到 %1").arg(base));
    for (const QString &local : files) {
        const QString remote = base + QLatin1Char('/') + QFileInfo(local).fileName();
        m_worker->enqueue(SftpWorker::Op::Upload, local, remote);
    }
    if (!files.isEmpty())
        setStatus(QStringLiteral("已加入 %1 个上传任务").arg(files.size()));
}

void FileTreePanel::downloadSelection()
{
    if (!m_model || !m_worker)
        return;
    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid())
        return;
    const QString remote = m_model->pathForIndex(current);
    if (m_model->isDirIndex(current)) {
        setStatus(QStringLiteral("暂不支持整目录下载，请逐个文件下载"));
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("下载到本地目录"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (dir.isEmpty())
        return;
    const QString local = dir + QLatin1Char('/') + m_model->nameForIndex(current);
    const quint64 id = m_worker->enqueue(SftpWorker::Op::Download, remote, local);
    m_pendingDownloads.insert(id, local);
    setStatus(QStringLiteral("下载中…"));
}

void FileTreePanel::renameSelection()
{
    if (!m_model || !m_worker)
        return;
    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid())
        return;
    const QString path = m_model->pathForIndex(current);
    bool ok = false;
    const QString newName = QInputDialog::getText(this, QStringLiteral("重命名"),
                                                  QStringLiteral("新名称："), QLineEdit::Normal,
                                                  m_model->nameForIndex(current), &ok);
    if (!ok || newName.trimmed().isEmpty())
        return;
    const QString target = QFileInfo(path).path() + QLatin1Char('/') + newName.trimmed();
    m_worker->enqueue(SftpWorker::Op::Rename, path, target);
    setStatus(QStringLiteral("已请求重命名，稍后刷新查看"));
}

void FileTreePanel::removeSelection()
{
    if (!m_model || !m_worker)
        return;
    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid())
        return;
    const QString path = m_model->pathForIndex(current);
    const auto answer = QMessageBox::question(
        this, QStringLiteral("删除"),
        QStringLiteral("确认删除 %1 ？目录必须为空才能删除。").arg(path));
    if (answer != QMessageBox::Yes)
        return;
    m_worker->enqueue(SftpWorker::Op::Remove, path);
    setStatus(QStringLiteral("已请求删除，稍后刷新查看"));
}

void FileTreePanel::openSelection()
{
    if (!m_model || !m_worker)
        return;
    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid())
        return;
    if (m_model->isDirIndex(current)) {
        m_view->setExpanded(current, !m_view->isExpanded(current));
        return;
    }
    const QString remote = m_model->pathForIndex(current);
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/termdock");
    QDir().mkpath(cacheDir);
    const QString local = cacheDir + QLatin1Char('/') + m_model->nameForIndex(current);
    const quint64 id = m_worker->enqueue(SftpWorker::Op::Download, remote, local);
    m_pendingDownloads.insert(id, local);
    setStatus(QStringLiteral("下载完成后自动打开…"));
}

} // namespace termdock
