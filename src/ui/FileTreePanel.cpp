#include "ui/FileTreePanel.h"

#include "ssh/SftpModel.h"
#include "ssh/SftpWorker.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
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
    m_toolbar->setIconSize(QSize(15, 15));
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_toolbar->addAction(QStringLiteral("刷新"), this, &FileTreePanel::refreshCurrent);
    m_toolbar->addAction(QStringLiteral("上传"), this, &FileTreePanel::uploadFiles);

    m_pathLabel = new QLabel(this);
    m_pathLabel->setObjectName(QStringLiteral("hint"));
    m_pathLabel->setContentsMargins(10, 0, 10, 4);
    m_pathLabel->setText(QStringLiteral("未连接"));
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_toolbar);
    layout->addWidget(m_pathLabel);

    m_view = new QTreeView(this);
    m_view->setUniformRowHeights(true);
    m_view->setAnimated(false);
    m_view->setAlternatingRowColors(false);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->header()->setStretchLastSection(false);
    layout->addWidget(m_view, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("hint"));
    m_status->setContentsMargins(10, 4, 10, 6);
    m_status->setText(QStringLiteral("未连接"));
    layout->addWidget(m_status);

    connect(m_view, &QTreeView::doubleClicked, this, &FileTreePanel::openSelection);
    connect(m_view, &QWidget::customContextMenuRequested, this, &FileTreePanel::showContextMenu);
    // 选择模型要等 setModel 之后才存在，相关信号在 attach() 里连接

    updatePathLabel();
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
        if (QItemSelectionModel *selection = m_view->selectionModel()) {
            connect(selection, &QItemSelectionModel::currentChanged, this,
                    [this](const QModelIndex &, const QModelIndex &) { updatePathLabel(); });
        }
    }
    if (model && worker) {
        connect(worker, &SftpWorker::progressed, this,
                [this](quint64 id, qint64 done, qint64 total) {
                    Q_UNUSED(id);
                    if (total > 0) {
                        setStatus(QStringLiteral("传输中 %1 / %2 KB（%3%）")
                                      .arg(done / 1024)
                                      .arg(total / 1024)
                                      .arg(int(done * 100 / total)));
                    }
                });
        connect(worker, &SftpWorker::succeeded, this, [this](quint64 id, const QString &message) {
            setStatus(message);
            const QString localPath = m_pendingDownloads.take(id);
            if (!localPath.isEmpty())
                QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
            // 结构变化后自动刷新当前目录，省得用户手动点刷新
            if (message.contains(QStringLiteral("已创建")) || message.contains(QStringLiteral("已删除"))
                || message.contains(QStringLiteral("已重命名"))) {
                const QModelIndex dir = currentDirectoryIndex();
                if (dir.isValid())
                    m_model->refresh(dir);
                else
                    m_model->refresh(QModelIndex());
            }
        });
        connect(worker, &SftpWorker::failedOp, this, [this](quint64, const QString &message) {
            setStatus(message);
        });
        setStatus(QStringLiteral("正在加载目录…"));
    }
    updatePathLabel();
}

void FileTreePanel::detach()
{
    m_view->setModel(nullptr);
    m_model = nullptr;
    m_worker = nullptr;
    m_pendingDownloads.clear();
    setStatus(QStringLiteral("未连接"));
    updatePathLabel();
}

void FileTreePanel::setStatus(const QString &message)
{
    m_status->setText(message);
}

QModelIndex FileTreePanel::selectedIndex() const
{
    return m_view->currentIndex();
}

QModelIndex FileTreePanel::currentDirectoryIndex() const
{
    if (!m_model)
        return QModelIndex();
    const QModelIndex current = selectedIndex();
    if (!current.isValid())
        return QModelIndex();
    if (m_model->isDirIndex(current))
        return current;
    return current.parent();
}

QString FileTreePanel::currentDirectory() const
{
    if (!m_model)
        return QString();
    const QModelIndex current = selectedIndex();
    if (!current.isValid())
        return m_model->homePath();
    const QString path = m_model->pathForIndex(current);
    if (m_model->isDirIndex(current))
        return path;
    return QFileInfo(path).path();
}

void FileTreePanel::updatePathLabel()
{
    if (!m_model) {
        m_pathLabel->setText(QStringLiteral("未连接"));
        m_pathLabel->setToolTip(QString());
        return;
    }
    const QString dir = currentDirectory();
    m_pathLabel->setText(dir.isEmpty() ? m_model->homePath() : dir);
    m_pathLabel->setToolTip(dir);
}

void FileTreePanel::refreshCurrent()
{
    if (!m_model)
        return;
    const QModelIndex dir = currentDirectoryIndex();
    if (dir.isValid())
        m_model->refresh(dir);
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
    const QString name = QInputDialog::getText(this, QStringLiteral("新建文件夹"),
                                               QStringLiteral("名称："), QLineEdit::Normal,
                                               QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    m_worker->enqueue(SftpWorker::Op::Mkdir, base + QLatin1Char('/') + name.trimmed());
    setStatus(QStringLiteral("正在创建文件夹 %1 …").arg(name.trimmed()));
}

void FileTreePanel::makeFile()
{
    if (!m_model || !m_worker)
        return;
    const QString base = currentDirectory();
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("新建文件"),
                                               QStringLiteral("文件名（例如 notes.txt）："),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    m_worker->enqueue(SftpWorker::Op::CreateFile, base + QLatin1Char('/') + name.trimmed());
    setStatus(QStringLiteral("正在创建文件 %1 …").arg(name.trimmed()));
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
    const QModelIndex current = selectedIndex();
    if (!current.isValid())
        return;
    const QString remote = m_model->pathForIndex(current);
    const bool isDir = m_model->isDirIndex(current);
    const QString dir = QFileDialog::getExistingDirectory(
        this, isDir ? QStringLiteral("下载整个文件夹到…") : QStringLiteral("下载到本地目录"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (dir.isEmpty())
        return;
    const QString local = dir + QLatin1Char('/') + m_model->nameForIndex(current);
    const quint64 id = m_worker->enqueue(SftpWorker::Op::Download, remote, local);
    m_pendingDownloads.insert(id, local);
    setStatus(isDir ? QStringLiteral("正在递归下载 %1 …").arg(m_model->nameForIndex(current))
                    : QStringLiteral("下载中…"));
}

void FileTreePanel::renameSelection()
{
    if (!m_model || !m_worker)
        return;
    const QModelIndex current = selectedIndex();
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
    setStatus(QStringLiteral("正在重命名…"));
}

void FileTreePanel::removeSelection()
{
    if (!m_model || !m_worker)
        return;
    const QModelIndex current = selectedIndex();
    if (!current.isValid())
        return;
    const QString path = m_model->pathForIndex(current);
    const bool isDir = m_model->isDirIndex(current);
    const auto answer = QMessageBox::question(
        this, QStringLiteral("删除"),
        isDir ? QStringLiteral("确认递归删除文件夹 %1 及其全部内容？此操作不可撤销。").arg(path)
              : QStringLiteral("确认删除 %1 ？").arg(path));
    if (answer != QMessageBox::Yes)
        return;
    m_worker->enqueue(SftpWorker::Op::Remove, path);
    setStatus(QStringLiteral("正在删除…"));
}

void FileTreePanel::openSelection()
{
    if (!m_model || !m_worker)
        return;
    const QModelIndex current = selectedIndex();
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

void FileTreePanel::copyRemotePath()
{
    const QModelIndex current = selectedIndex();
    if (!m_model || !current.isValid())
        return;
    QApplication::clipboard()->setText(m_model->pathForIndex(current));
    setStatus(QStringLiteral("已复制路径"));
}

void FileTreePanel::showContextMenu(const QPoint &pos)
{
    if (!m_model || !m_worker)
        return;
    const QModelIndex index = m_view->indexAt(pos);
    if (index.isValid())
        m_view->setCurrentIndex(index);

    const bool onEntry = index.isValid();
    const bool isDir = onEntry && m_model->isDirIndex(index);

    QMenu menu(this);
    if (onEntry) {
        menu.addAction(isDir ? QStringLiteral("展开 / 收起") : QStringLiteral("打开（下载后预览）"),
                       this, &FileTreePanel::openSelection);
        menu.addAction(isDir ? QStringLiteral("下载整个文件夹…") : QStringLiteral("下载…"), this,
                       &FileTreePanel::downloadSelection);
        menu.addSeparator();
        menu.addAction(QStringLiteral("重命名…"), this, &FileTreePanel::renameSelection);
        menu.addAction(QStringLiteral("删除…"), this, &FileTreePanel::removeSelection);
        menu.addSeparator();
        menu.addAction(QStringLiteral("复制远程路径"), this, &FileTreePanel::copyRemotePath);
        menu.addSeparator();
    }
    menu.addAction(QStringLiteral("新建文件夹…"), this, &FileTreePanel::makeDirectory);
    menu.addAction(QStringLiteral("新建文件…"), this, &FileTreePanel::makeFile);
    menu.addAction(QStringLiteral("上传文件…"), this, &FileTreePanel::uploadFiles);
    menu.addSeparator();
    menu.addAction(QStringLiteral("刷新"), this, &FileTreePanel::refreshCurrent);
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

} // namespace termdock
