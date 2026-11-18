#pragma once

#include <QHash>
#include <QMainWindow>
#include <QVector>

#include "session/Session.h"

class QAction;
class QLabel;
class QSplitter;
class QTabWidget;

namespace termdock {

class FileTreePanel;
class SessionPanel;
class SftpModel;
class SftpWorker;
class ShellSession;
class TerminalWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void reloadSessions();
    void openSession(const Session &session);
    bool openSessionById(const QString &id);
    bool connectDirect(const Session &session); // 命令行直连，不写入会话簿

    TerminalWidget *currentTerminal() const;
    void adjustFontSize(int delta);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct Page {
        TerminalWidget *terminal = nullptr;
        ShellSession *shell = nullptr;
        SftpWorker *sftp = nullptr;
        SftpModel *model = nullptr;
        Session session;
        QString fingerprint;
        bool shellConnected = false;
        bool sftpReady = false;
        QHash<quint64, QString> listRequests;
    };

    void setupUi();
    void setupActions();
    void saveSessions();
    void newSession();
    void editSession(const QString &id);
    void duplicateSession(const QString &id);
    void deleteSession(const QString &id);
    void activateSessionFromPanel(const QString &id);
    void closePage(int tabIndex);
    void onTabChanged(int index);
    void onShellConnected(Page *page, const QString &fingerprint, const QString &hostKeyType);
    void onShellFailed(Page *page, const QString &message);
    void onShellClosed(Page *page, const QString &reason);
    void updateTabTitle(Page *page);
    void updateStatusBar();
    void persistUiState();

    Page *currentPage() const;
    Page *findPageBySessionId(const QString &id) const;
    int pageIndexOf(const Page *page) const;
    Session *findSession(const QString &id);

    SessionPanel *m_sessionPanel = nullptr;
    FileTreePanel *m_filePanel = nullptr;
    QTabWidget *m_tabs = nullptr;
    QSplitter *m_splitter = nullptr;
    QLabel *m_statusInfo = nullptr;
    QAction *m_showSessionPanelAction = nullptr;
    QAction *m_showFilePanelAction = nullptr;
    QVector<Page *> m_pages;
    SessionList m_sessions;
};

} // namespace termdock
