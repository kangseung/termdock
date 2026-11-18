#pragma once

#include <QHash>
#include <QWidget>

class QLabel;
class QTreeView;
class QToolBar;

namespace termdock {

class SftpModel;
class SftpWorker;

// 右侧文件树：SFTP 浏览 + 上传/下载/新建/删除/重命名。
class FileTreePanel : public QWidget {
    Q_OBJECT

public:
    explicit FileTreePanel(QWidget *parent = nullptr);

    void attach(SftpModel *model, SftpWorker *worker);
    void detach();
    void setStatus(const QString &message);

private:
    void setupUi();
    void refresh();
    void makeDirectory();
    void uploadFiles();
    void downloadSelection();
    void renameSelection();
    void removeSelection();
    void openSelection();
    void updateActions();
    QString currentDirectory() const;

    QTreeView *m_view = nullptr;
    QToolBar *m_toolbar = nullptr;
    QLabel *m_status = nullptr;
    SftpModel *m_model = nullptr;
    SftpWorker *m_worker = nullptr;
    QHash<quint64, QString> m_pendingDownloads;
};

} // namespace termdock
