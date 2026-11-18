#pragma once

#include <QHash>
#include <QSet>
#include <QMainWindow>
#include <QVector>

#include "session/Session.h"

class QAction;
class QLabel;
class QSplitter;
class QStackedWidget;
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
    // 关闭标签/退出程序时是否弹确认框（自动截图、脚本化测试时关掉）
    void setConfirmClose(bool on);

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
        QSet<quint64> quietListRequests; // 模型探路用的列表请求，不打扰状态栏
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
    void updateTabPlaceholder();
    void onShellConnected(Page *page, const QString &fingerprint, const QString &hostKeyType);
    void onShellFailed(Page *page, const QString &message);
    void onShellClosed(Page *page, const QString &reason);
    void updateTabTitle(Page *page);
    void updateStatusBar();
    void persistUiState();

    Page *currentPage() const;
    // 关闭页面前先断信号、停线程，避免回调摸到已释放的 Page
    void teardownPage(Page *page);
    void confirmQuit();
    Page *findPageBySessionId(const QString &id) const;
    int pageIndexOf(const Page *page) const;
    Session *findSession(const QString &id);

    SessionPanel *m_sessionPanel = nullptr;
    FileTreePanel *m_filePanel = nullptr;
    QTabWidget *m_tabs = nullptr;
    QStackedWidget *m_tabStack = nullptr;
    QSplitter *m_splitter = nullptr;
    QLabel *m_statusInfo = nullptr;
    bool m_confirmClose = true;
    bool m_quitConfirmed = false;
    QAction *m_showSessionPanelAction = nullptr;
    QAction *m_showFilePanelAction = nullptr;
    QVector<Page *> m_pages;
    SessionList m_sessions;
};

} // namespace termdock
