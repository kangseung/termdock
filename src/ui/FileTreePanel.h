#pragma once

#include <QHash>
#include <QWidget>

class QLabel;
class QTreeView;
class QToolBar;

namespace termdock {

class SftpModel;
class SftpWorker;

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
};

} // namespace termdock
