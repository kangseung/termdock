#pragma once

#include <QHash>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include "ssh/SftpWorker.h"

class QLabel;
class QTreeView;
class QToolBar;

namespace termdock {

class SftpModel;

// 右侧文件树：SFTP 浏览。顶部只放「路径 + 刷新 + 上传」，
// 其余（新建文件/文件夹、重命名、删除、下载）都走右键菜单。
class FileTreePanel : public QWidget {
    Q_OBJECT

public:
    explicit FileTreePanel(QWidget *parent = nullptr);

    void attach(SftpModel *model, SftpWorker *worker);
    void detach();
    void setStatus(const QString &message);

protected:
    // 拖拽上传：监听文件树视口的 DragEnter/DragMove/Drop
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupUi();
    void showContextMenu(const QPoint &pos);
    void refreshCurrent();
    void makeDirectory();
    void makeFile();
    void uploadFiles();
    void downloadSelection();
    void renameSelection();
    void removeSelection();
    void openSelection();
    void copyRemotePath();
    void updatePathLabel();
    // 会改变目录结构（新建/改名/删除/上传）的请求：记下操作完成后该刷新的目录
    quint64 enqueueStructural(SftpWorker::Op op, const QString &path,
                             const QString &path2 = QString());
    void refreshDirPath(const QString &path);
    void flushRefresh();
    // 拖拽落地目录：文件夹行就是它自己，文件行/空白处是当前目录
    QString dropTargetDir(const QPoint &pos) const;
    // 本地文件直接上传；本地文件夹先递归 mkdir 再传里面的文件（队列 FIFO 保证顺序）
    void uploadLocalPath(const QString &local, const QString &remoteDir);

    QModelIndex selectedIndex() const;
    QModelIndex currentDirectoryIndex() const;
    QString currentDirectory() const;

    QTreeView *m_view = nullptr;
    QToolBar *m_toolbar = nullptr;
    QLabel *m_pathLabel = nullptr;
    QLabel *m_status = nullptr;
    SftpModel *m_model = nullptr;
    SftpWorker *m_worker = nullptr;
    QHash<quint64, QString> m_openAfterDownload; // 双击预览下载的文件，下载完成后自动打开
    QHash<quint64, QString> m_refreshDirs;
    // 拖拽/批量操作会短时间内产生很多次刷新，合并成一次
    QSet<QString> m_pendingRefreshPaths;
    QTimer m_refreshTimer;
    QString m_statusBeforeDrag;
    // attach() 建立的连接，detach() 时逐条断开，避免切换标签时信号重复发送
    QVector<QMetaObject::Connection> m_attached;
};

} // namespace termdock
