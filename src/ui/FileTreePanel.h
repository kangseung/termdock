#pragma once

#include <QHash>
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

    QModelIndex selectedIndex() const;
    QModelIndex currentDirectoryIndex() const;
    QString currentDirectory() const;

    QTreeView *m_view = nullptr;
    QToolBar *m_toolbar = nullptr;
    QLabel *m_pathLabel = nullptr;
    QLabel *m_status = nullptr;
    SftpModel *m_model = nullptr;
    SftpWorker *m_worker = nullptr;
    QHash<quint64, QString> m_pendingDownloads;
    QHash<quint64, QString> m_refreshDirs;
    // attach() 建立的连接，detach() 时逐条断开，避免切换标签时信号重复发送
    QVector<QMetaObject::Connection> m_attached;
};

} // namespace termdock
