#include "ui/FileTreePanel.h"

#include "ssh/SftpModel.h"
#include "ssh/SftpWorker.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QStandardPaths>
#include <QToolBar>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace termdock {

namespace {

QString formatBytes(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    const double kb = bytes / 1024.0;
    if (kb < 1024.0)
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    const double mb = kb / 1024.0;
    if (mb < 1024.0)
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 2);
}

QString formatDuration(qint64 seconds)
{
    if (seconds < 1)
        return QStringLiteral("不到 1 秒");
    if (seconds < 60)
        return QStringLiteral("%1 秒").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1 分 %2 秒").arg(seconds / 60).arg(seconds % 60);
    return QStringLiteral("%1 小时 %2 分").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

} // namespace

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
    // 双击展开由 openSelection() 自己处理：视图默认也会在双击时展开，
    // 两个动作叠加的结果是展开后立刻被收回去（双击永远打不开文件夹）
    m_view->setExpandsOnDoubleClick(false);
    m_view->setAlternatingRowColors(false);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->header()->setStretchLastSection(false);
    // 拖拽上传：把本地文件/文件夹拖到树里（文件夹行 → 该文件夹，其他位置 → 当前目录）
    m_view->viewport()->setAcceptDrops(true);
    m_view->viewport()->installEventFilter(this);
    layout->addWidget(m_view, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("hint"));
    m_status->setContentsMargins(10, 4, 10, 6);
    m_status->setText(QStringLiteral("未连接"));
    layout->addWidget(m_status);

    connect(m_view, &QTreeView::doubleClicked, this, &FileTreePanel::openSelection);
    // 每展开一层就探一下这层的子目录内容：这样空目录不会白带一个展开箭头
    connect(m_view, &QTreeView::expanded, this, [this](const QModelIndex &index) {
        if (m_model)
            m_model->probeChildren(index);
    });
    connect(m_view, &QWidget::customContextMenuRequested, this, &FileTreePanel::showContextMenu);
    // 选择模型要等 setModel 之后才存在，相关信号在 attach() 里连接

    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(200);
    connect(&m_refreshTimer, &QTimer::timeout, this, &FileTreePanel::flushRefresh);

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
            m_attached.append(connect(selection, &QItemSelectionModel::currentChanged, this,
                                      [this](const QModelIndex &, const QModelIndex &) {
                                          updatePathLabel();
                                      }));
        }
    }
    if (model && worker) {
        m_attached.append(connect(worker, &SftpWorker::progressed, this,
                                  [this](quint64 id, qint64 done, qint64 total, double speed) {
                                      Q_UNUSED(id);
                                      QString text = QStringLiteral("传输中 %1").arg(formatBytes(done));
                                      if (total > 0)
                                          text += QStringLiteral(" / %1（%2%）")
                                                      .arg(formatBytes(total))
                                                      .arg(int(done * 100 / total));
                                      if (speed >= 1.0)
                                          text += QStringLiteral(" · %1/s").arg(formatBytes(qint64(speed)));
                                      if (total > done && speed >= 1.0)
                                          text += QStringLiteral(" · 剩余 %1")
                                                      .arg(formatDuration(qint64((total - done) / speed)));
                                      setStatus(text);
                                  }));
        m_attached.append(connect(worker, &SftpWorker::succeeded, this,
                                  [this](quint64 id, const QString &message) {
                                      setStatus(message);
                                      const QString localPath = m_openAfterDownload.take(id);
                                      if (!localPath.isEmpty())
                                          QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
                                      // 结构变化后自动刷新受影响目录，省得用户手动点刷新。
                                      // 必须刷父目录：被改名/删除的条目自己已经不在服务器上了，去列它会报 SFTP 错误。
                                      const QString dirPath = m_refreshDirs.take(id);
                                      if (!dirPath.isEmpty())
                                          refreshDirPath(dirPath);
                                  }));
        m_attached.append(connect(worker, &SftpWorker::failedOp, this,
                                  [this](quint64, const QString &message) { setStatus(message); }));
        setStatus(QStringLiteral("正在加载目录…"));
    }
    updatePathLabel();
}

void FileTreePanel::detach()
{
    // 每次切标签都会重新 attach，旧连接必须断开：否则信号会一直累积，
    // 上一个会话的操作结果会写到当前面板的状态栏里
    for (const QMetaObject::Connection &connection : m_attached)
        disconnect(connection);
    m_attached.clear();
    m_view->setModel(nullptr);
    m_model = nullptr;
    m_worker = nullptr;
    m_openAfterDownload.clear();
    m_refreshDirs.clear();
    m_pendingRefreshPaths.clear();
    m_refreshTimer.stop();
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

quint64 FileTreePanel::enqueueStructural(SftpWorker::Op op, const QString &path, const QString &path2)
{
    const quint64 id = m_worker->enqueue(op, path, path2);
    // 结构变化发生在远端目标所在的目录里，操作完成后刷那个目录。
    // 上传时 path 是本地文件、path2 才是远端目标。
    const QString &remotePath = (op == SftpWorker::Op::Upload) ? path2 : path;
    m_refreshDirs.insert(id, QFileInfo(remotePath).path());
    return id;
}

void FileTreePanel::refreshDirPath(const QString &path)
{
    if (!m_model || path.isEmpty())
        return;
    // 拖拽/批量操作会连着触发很多次，合并成一次刷新
    m_pendingRefreshPaths.insert(path);
    if (!m_refreshTimer.isActive())
        m_refreshTimer.start();
}

void FileTreePanel::flushRefresh()
{
    const QSet<QString> paths = m_pendingRefreshPaths;
    m_pendingRefreshPaths.clear();
    if (!m_model)
        return;
    for (const QString &path : paths) {
        const QModelIndex index = m_model->indexForPath(path);
        if (index.isValid())
            m_model->refresh(index);
        else if (path == m_model->homePath())
            m_model->refresh(QModelIndex()); // 根目录本身没有 QModelIndex
    }
}

QString FileTreePanel::dropTargetDir(const QPoint &pos) const
{
    if (!m_model)
        return QString();
    const QModelIndex index = m_view->indexAt(pos);
    if (!index.isValid())
        return currentDirectory();
    const QString path = m_model->pathForIndex(index);
    if (m_model->isDirIndex(index))
        return path; // 落在文件夹上就传进这个文件夹
    return QFileInfo(path).path();
}

void FileTreePanel::uploadLocalPath(const QString &local, const QString &remoteDir)
{
    const QFileInfo info(local);
    const QString remote = remoteDir + QLatin1Char('/') + info.fileName();
    if (!info.isDir()) {
        enqueueStructural(SftpWorker::Op::Upload, info.absoluteFilePath(), remote);
        return;
    }
    // 文件夹：先建远端目录，再把里面的东西排到队尾（worker 单线程 FIFO，顺序有保证）
    enqueueStructural(SftpWorker::Op::Mkdir, remote);
    const QFileInfoList children =
        QDir(local).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo &child : children)
        uploadLocalPath(child.absoluteFilePath(), remote);
}

bool FileTreePanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_view->viewport())
        return QWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto *dragEvent = static_cast<QDragMoveEvent *>(event);
        const QString target = dropTargetDir(dragEvent->position().toPoint());
        bool hasLocalFile = false;
        for (const QUrl &url : dragEvent->mimeData()->urls()) {
            if (url.isLocalFile() && QFileInfo::exists(url.toLocalFile())) {
                hasLocalFile = true;
                break;
            }
        }
        if (!m_model || !m_worker || target.isEmpty() || !hasLocalFile) {
            dragEvent->ignore();
            return true;
        }
        if (event->type() == QEvent::DragEnter)
            m_statusBeforeDrag = m_status->text();
        setStatus(QStringLiteral("松开即可上传到 %1").arg(target));
        dragEvent->setDropAction(Qt::CopyAction);
        dragEvent->accept();
        return true;
    }
    case QEvent::DragLeave:
        if (!m_statusBeforeDrag.isEmpty())
            setStatus(m_statusBeforeDrag);
        m_statusBeforeDrag.clear();
        return true;
    case QEvent::Drop: {
        auto *dropEvent = static_cast<QDropEvent *>(event);
        const QString target = dropTargetDir(dropEvent->position().toPoint());
        if (!m_model || !m_worker || target.isEmpty()) {
            dropEvent->ignore();
            return true;
        }
        int count = 0;
        for (const QUrl &url : dropEvent->mimeData()->urls()) {
            if (!url.isLocalFile())
                continue;
            const QString local = url.toLocalFile();
            if (!QFileInfo::exists(local))
                continue;
            uploadLocalPath(local, target);
            ++count;
        }
        if (count == 0) {
            setStatus(QStringLiteral("拖进来的内容里没有可上传的本地文件"));
            dropEvent->ignore();
            return true;
        }
        setStatus(QStringLiteral("正在上传 %1 项到 %2 …").arg(count).arg(target));
        dropEvent->setDropAction(Qt::CopyAction);
        dropEvent->accept();
        return true;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
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
    enqueueStructural(SftpWorker::Op::Mkdir, base + QLatin1Char('/') + name.trimmed());
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
    enqueueStructural(SftpWorker::Op::CreateFile, base + QLatin1Char('/') + name.trimmed());
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
        enqueueStructural(SftpWorker::Op::Upload, local, remote);
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
    if (QFileInfo::exists(local)) {
        // 覆盖本地已有内容前必须问一声，免得默默盖掉用户的东西
        const auto answer = QMessageBox::question(
            this, QStringLiteral("已存在同名%1").arg(isDir ? QStringLiteral("文件夹") : QStringLiteral("文件")),
            isDir ? QStringLiteral("本地已有文件夹 %1，继续会覆盖里面同名的文件。\n确定继续吗？").arg(local)
                  : QStringLiteral("本地已有文件 %1，确定覆盖吗？").arg(local),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes)
            return;
    }
    // 明确“下载到…”的请求只落地，不自动打开：以前这里也登记到 m_openAfterDownload，
    // 结果下载完就被 XDG 默认程序“打开”，系统弹“出于安全考虑不可执行此文件”
    m_worker->enqueue(SftpWorker::Op::Download, remote, local);
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
    enqueueStructural(SftpWorker::Op::Rename, path, target);
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
    enqueueStructural(SftpWorker::Op::Remove, path);
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
    m_openAfterDownload.insert(id, local);
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
