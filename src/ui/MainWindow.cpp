#include "ui/MainWindow.h"

#include "app/Theme.h"
#include "session/SessionStore.h"
#include "ssh/SftpModel.h"
#include "ssh/SftpWorker.h"
#include "ssh/ShellSession.h"
#include "term/TerminalWidget.h"
#include "ui/FileTreePanel.h"
#include "ui/VaultDialog.h"
#include "ui/SessionDialog.h"
#include "ui/SessionPanel.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace termdock {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    setupActions();
    reloadSessions();

    QSettings settings;
    const int fontSize = settings.value(QStringLiteral("terminal/fontSize"), 0).toInt();
    if (fontSize > 0)
        Theme::mutableInstance().setTerminalFontSize(fontSize);
    const QString family = settings.value(QStringLiteral("terminal/fontFamily")).toString();
    if (!family.isEmpty())
        Theme::mutableInstance().setTerminalFontFamily(family);
    restoreGeometry(settings.value(QStringLiteral("ui/geometry")).toByteArray());
    const QByteArray splitterState = settings.value(QStringLiteral("ui/splitter")).toByteArray();
    if (!splitterState.isEmpty())
        m_splitter->restoreState(splitterState);

    statusBar()->showMessage(
        QStringLiteral("双击右侧会话即可连接；左侧文件树右键可新建/上传/删除；Ctrl+Shift+C / V 复制粘贴"));
}

MainWindow::~MainWindow()
{
    for (Page *page : m_pages) {
        if (page->shell) {
            page->shell->stop();
            page->shell->wait(3000);
        }
        if (page->sftp) {
            page->sftp->stop();
            page->sftp->wait(3000);
        }
    }
}

void MainWindow::setupUi()
{
    setWindowTitle(QStringLiteral("termdock"));
    resize(1180, 720);

    m_sessionPanel = new SessionPanel(this);
    m_sessionPanel->setMinimumWidth(250);
    m_sessionPanel->setMaximumWidth(460);

    m_tabs = new QTabWidget(this);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->setMinimumWidth(420);

    // 没有标签页时显示提示，避免中间留一大块空白
    auto *placeholder = new QWidget(this);
    auto *placeholderLayout = new QVBoxLayout(placeholder);
    auto *placeholderText = new QLabel(
        QStringLiteral("双击右侧的会话即可连接\n\nCtrl+N 新建会话 · Ctrl+Shift+K 用安全密钥解锁已保存密码"),
        placeholder);
    placeholderText->setObjectName(QStringLiteral("placeholder"));
    placeholderText->setAlignment(Qt::AlignCenter);
    placeholderLayout->addWidget(placeholderText);

    m_tabStack = new QStackedWidget(this);
    m_tabStack->addWidget(placeholder);
    m_tabStack->addWidget(m_tabs);
    m_tabStack->setMinimumWidth(420);

    m_filePanel = new FileTreePanel(this);
    m_filePanel->setMinimumWidth(240);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    // 默认布局：左 = 文件树，中 = 终端标签，右 = 会话簿
    m_splitter->addWidget(m_filePanel);
    m_splitter->addWidget(m_tabStack);
    m_splitter->addWidget(m_sessionPanel);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setStretchFactor(2, 0);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setSizes({300, 640, 280});
    setCentralWidget(m_splitter);

    m_statusInfo = new QLabel(this);
    m_statusInfo->setObjectName(QStringLiteral("hint"));
    statusBar()->addPermanentWidget(m_statusInfo);

    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closePage);

    connect(m_sessionPanel, &SessionPanel::connectRequested, this, &MainWindow::activateSessionFromPanel);
    connect(m_sessionPanel, &SessionPanel::newRequested, this, &MainWindow::newSession);
    connect(m_sessionPanel, &SessionPanel::editRequested, this, &MainWindow::editSession);
    connect(m_sessionPanel, &SessionPanel::duplicateRequested, this, &MainWindow::duplicateSession);
    connect(m_sessionPanel, &SessionPanel::deleteRequested, this, &MainWindow::deleteSession);
}

void MainWindow::setupActions()
{
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件"));
    auto *newAction = fileMenu->addAction(QStringLiteral("新建会话…"));
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &MainWindow::newSession);

    auto *vaultAction = fileMenu->addAction(QStringLiteral("安全密钥与已保存的密码…"));
    vaultAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+K")));
    connect(vaultAction, &QAction::triggered, this, [this]() {
        VaultDialog dialog(this);
        dialog.exec();
    });

    fileMenu->addSeparator();
    auto *closeTabAction = fileMenu->addAction(QStringLiteral("关闭当前标签"));
    closeTabAction->setShortcut(QKeySequence::Close);
    connect(closeTabAction, &QAction::triggered, this, [this]() {
        if (m_tabs->currentIndex() >= 0)
            closePage(m_tabs->currentIndex());
    });

    fileMenu->addSeparator();
    auto *quitAction = fileMenu->addAction(QStringLiteral("退出"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto *sessionMenu = menuBar()->addMenu(QStringLiteral("会话"));
    auto *connectAction = sessionMenu->addAction(QStringLiteral("连接选中会话"));
    connectAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Return")));
    connect(connectAction, &QAction::triggered, this,
            [this]() { activateSessionFromPanel(m_sessionPanel->currentSessionId()); });

    auto *editAction = sessionMenu->addAction(QStringLiteral("编辑会话…"));
    editAction->setShortcut(QKeySequence(QStringLiteral("F2")));
    connect(editAction, &QAction::triggered, this,
            [this]() { editSession(m_sessionPanel->currentSessionId()); });

    auto *deleteAction = sessionMenu->addAction(QStringLiteral("删除会话"));
    connect(deleteAction, &QAction::triggered, this,
            [this]() { deleteSession(m_sessionPanel->currentSessionId()); });

    sessionMenu->addSeparator();
    auto *sftpRefreshAction = sessionMenu->addAction(QStringLiteral("刷新文件树"));
    sftpRefreshAction->setShortcut(QKeySequence::Refresh);
    connect(sftpRefreshAction, &QAction::triggered, this, [this]() {
        Page *page = currentPage();
        if (page && page->model)
            page->model->refresh(QModelIndex());
    });

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图"));
    m_showSessionPanelAction = viewMenu->addAction(QStringLiteral("显示会话面板（右侧）"));
    m_showSessionPanelAction->setCheckable(true);
    m_showSessionPanelAction->setChecked(true);
    connect(m_showSessionPanelAction, &QAction::toggled, this,
            [this](bool on) { m_sessionPanel->setVisible(on); });

    m_showFilePanelAction = viewMenu->addAction(QStringLiteral("显示文件树（左侧）"));
    m_showFilePanelAction->setCheckable(true);
    m_showFilePanelAction->setChecked(true);
    connect(m_showFilePanelAction, &QAction::toggled, this,
            [this](bool on) { m_filePanel->setVisible(on); });

    viewMenu->addSeparator();
    auto *zoomIn = viewMenu->addAction(QStringLiteral("放大字体"));
    zoomIn->setShortcut(QKeySequence::ZoomIn);
    connect(zoomIn, &QAction::triggered, this, [this]() { adjustFontSize(1); });
    auto *zoomOut = viewMenu->addAction(QStringLiteral("缩小字体"));
    zoomOut->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOut, &QAction::triggered, this, [this]() { adjustFontSize(-1); });
    auto *zoomReset = viewMenu->addAction(QStringLiteral("重置字体"));
    zoomReset->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(zoomReset, &QAction::triggered, this, [this]() {
        Theme::mutableInstance().setTerminalFontSize(11);
        for (Page *page : m_pages) {
            if (page->terminal)
                page->terminal->applyFont();
        }
        persistUiState();
    });

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
    auto *aboutAction = helpMenu->addAction(QStringLiteral("关于 termdock"));
    connect(aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, QStringLiteral("关于 termdock"),
                           QStringLiteral("<b>termdock</b> %1<br/>"
                                          "Qt Widgets + libssh2 的精简终端：会话簿双击直连 + SFTP 文件树。"
                                          ).arg(QApplication::applicationVersion()));
    });
}

void MainWindow::reloadSessions()
{
    QString error;
    m_sessions = SessionStore::load(&error);
    m_sessionPanel->setSessions(m_sessions);
    if (!error.isEmpty())
        statusBar()->showMessage(error, 8000);
}

void MainWindow::saveSessions()
{
    QString error;
    if (!SessionStore::save(m_sessions, &error))
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
}

Session *MainWindow::findSession(const QString &id)
{
    for (Session &session : m_sessions) {
        if (session.id == id)
            return &session;
    }
    return nullptr;
}

void MainWindow::newSession()
{
    SessionDialog dialog(Session::createEmpty(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    Session session = dialog.session();
    m_sessions.append(session);
    saveSessions();
    m_sessionPanel->setSessions(m_sessions);
    m_sessionPanel->selectSession(session.id);
    openSession(session);
}

void MainWindow::editSession(const QString &id)
{
    Session *session = findSession(id);
    if (!session)
        return;
    SessionDialog dialog(*session, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const Session updated = dialog.session();
    const QString sessionId = session->id;
    *session = updated;
    session->id = sessionId;
    saveSessions();
    m_sessionPanel->setSessions(m_sessions);
    m_sessionPanel->selectSession(sessionId);

    // 已经打开的标签页同步新配置（下次连接生效）
    for (Page *page : m_pages) {
        if (page->session.id == sessionId) {
            page->session = *session;
            updateTabTitle(page);
        }
    }
}

void MainWindow::duplicateSession(const QString &id)
{
    const Session *session = findSession(id);
    if (!session)
        return;
    Session copy = *session;
    copy.id = Session::newId();
    copy.name = session->name + QStringLiteral(" 副本");
    m_sessions.append(copy);
    saveSessions();
    m_sessionPanel->setSessions(m_sessions);
    m_sessionPanel->selectSession(copy.id);
}

void MainWindow::deleteSession(const QString &id)
{
    for (int i = 0; i < m_sessions.size(); ++i) {
        if (m_sessions.at(i).id != id)
            continue;
        const auto answer = QMessageBox::question(
            this, QStringLiteral("删除会话"),
            QStringLiteral("确认删除会话「%1」？已连接的标签页不会断开。").arg(m_sessions.at(i).name));
        if (answer != QMessageBox::Yes)
            return;
        m_sessions.removeAt(i);
        saveSessions();
        m_sessionPanel->setSessions(m_sessions);
        return;
    }
}

void MainWindow::activateSessionFromPanel(const QString &id)
{
    if (id.isEmpty())
        return;
    if (Page *page = findPageBySessionId(id)) {
        m_tabs->setCurrentIndex(pageIndexOf(page));
        return;
    }
    openSessionById(id);
}

bool MainWindow::openSessionById(const QString &id)
{
    const Session *session = findSession(id);
    if (!session)
        return false;
    openSession(*session);
    return true;
}

bool MainWindow::connectDirect(const Session &session)
{
    if (!session.isValid())
        return false;
    openSession(session);
    return true;
}

void MainWindow::openSession(const Session &session)
{
    if (Page *existing = findPageBySessionId(session.id)) {
        m_tabs->setCurrentIndex(pageIndexOf(existing));
        return;
    }

    auto *page = new Page();
    page->session = session;
    page->terminal = new TerminalWidget(m_tabs);
    page->terminal->setFallbackTitle(session.name);
    page->terminal->setConnected(false);
    page->terminal->setStatusText(QStringLiteral("正在连接 %1 …").arg(session.displayTarget()));

    const int tabIndex = m_tabs->addTab(page->terminal, session.name);
    updateTabPlaceholder();
    m_pages.append(page);
    m_tabs->setCurrentIndex(tabIndex);
    // 连上就把键盘焦点交给终端，用户不用再点一下才能输入
    page->terminal->setFocus(Qt::OtherFocusReason);

    SshParams params;
    params.host = session.host;
    params.port = session.port;
    params.user = session.user;
    params.password = session.password;

    page->shell = new ShellSession(params, page->terminal->screen()->cols(),
                                   page->terminal->screen()->rows(), this);
    connect(page->terminal, &TerminalWidget::dataToSend, this,
            [shell = page->shell](const QByteArray &data) { shell->sendInput(data); });
    connect(page->shell, &ShellSession::readyRead, page->terminal,
            [terminal = page->terminal](const QByteArray &data) { terminal->feed(data); });
    connect(page->shell, &ShellSession::connected, this,
            [this, page](const QString &fingerprint, const QString &hostKeyType) {
                onShellConnected(page, fingerprint, hostKeyType);
            });
    connect(page->shell, &ShellSession::failed, this,
            [this, page](const QString &message) { onShellFailed(page, message); });
    connect(page->shell, &ShellSession::closedWith, this,
            [this, page](const QString &reason) { onShellClosed(page, reason); });
    connect(page->terminal, &TerminalWidget::gridResized, this,
            [shell = page->shell](int cols, int rows) { shell->resizePty(cols, rows); });
    connect(page->terminal, &TerminalWidget::titleChanged, this, [this, page](const QString &title) {
        page->terminal->setFallbackTitle(title);
        updateTabTitle(page);
    });
    connect(page->terminal, &TerminalWidget::zoomRequested, this,
            [this](int delta) { adjustFontSize(delta); });
    connect(page->terminal, &TerminalWidget::bell, this, []() { QApplication::beep(); });

    page->sftp = new SftpWorker(params, this);
    page->model = new SftpModel(this);
    connect(page->model, &SftpModel::listRequested, this, [page](const QString &path) {
        if (page->sftp) {
            const quint64 id = page->sftp->enqueue(SftpWorker::Op::List, path);
            page->listRequests.insert(id, path);
        }
    });
    connect(page->sftp, &SftpWorker::listed, this,
            [this, page](quint64 id, const QString &path, const SftpEntryList &entries) {
                page->listRequests.remove(id);
                page->model->applyListing(path, entries);
                if (page == currentPage())
                    m_filePanel->setStatus(QStringLiteral("%1：共 %2 项").arg(path).arg(entries.size()));
            });
    connect(page->sftp, &SftpWorker::failedOp, this, [this, page](quint64 id, const QString &message) {
        const QString path = page->listRequests.take(id);
        if (!path.isEmpty())
            page->model->markLoadingFailed(path);
        if (page == currentPage())
            m_filePanel->setStatus(message);
        statusBar()->showMessage(message, 6000);
    });
    connect(page->sftp, &SftpWorker::connected, this, [this, page](const QString &home) {
        page->sftpReady = true;
        page->model->setHomePath(home);
        if (page == currentPage())
            m_filePanel->attach(page->model, page->sftp);
    });
    connect(page->sftp, &SftpWorker::failed, this, [this, page](const QString &message) {
        if (page == currentPage())
            m_filePanel->setStatus(QStringLiteral("SFTP 不可用：%1").arg(message));
    });

    page->shell->start();
    page->sftp->start();
    updateStatusBar();
}

void MainWindow::onShellConnected(Page *page, const QString &fingerprint, const QString &hostKeyType)
{
    if (!m_pages.contains(page))
        return;
    page->shellConnected = true;
    page->fingerprint = fingerprint;
    page->terminal->setConnected(true);
    page->terminal->setStatusText(QString());

    const QString known = SessionStore::knownFingerprint(page->session.host, page->session.port);
    if (known.isEmpty()) {
        SessionStore::rememberFingerprint(page->session.host, page->session.port, fingerprint);
        statusBar()->showMessage(QStringLiteral("%1 首次连接，已记录主机指纹 %2（%3）")
                                     .arg(page->session.host, fingerprint, hostKeyType),
                                 8000);
    } else if (known != fingerprint) {
        page->terminal->setConnected(false);
        page->terminal->setStatusText(QStringLiteral("主机密钥已变化，连接已中止"));
        QMessageBox::critical(
            this, QStringLiteral("主机密钥变化"),
            QStringLiteral("主机 %1:%2 的密钥指纹与上次记录不一致，可能被中间人攻击，连接已中止。\n\n"
                           "上次：%3\n本次：%4\n\n确认是自己重装系统后可删除记录再连。")
                .arg(page->session.host)
                .arg(page->session.port)
                .arg(known, fingerprint));
        page->shell->stop();
        return;
    }
    updateTabTitle(page);
    updateStatusBar();
}

void MainWindow::onShellFailed(Page *page, const QString &message)
{
    if (!m_pages.contains(page))
        return;
    page->shellConnected = false;
    page->terminal->setConnected(false);
    page->terminal->setStatusText(QStringLiteral("连接失败：%1").arg(message));
    statusBar()->showMessage(message, 10000);
}

void MainWindow::onShellClosed(Page *page, const QString &reason)
{
    if (!m_pages.contains(page))
        return;
    page->shellConnected = false;
    page->terminal->setConnected(false);
    page->terminal->setStatusText(QStringLiteral("连接已断开：%1").arg(reason));
    updateTabTitle(page);
    updateStatusBar();
}

void MainWindow::updateTabTitle(Page *page)
{
    const int index = pageIndexOf(page);
    if (index < 0)
        return;
    QString title = page->terminal->screen()->title();
    if (title.isEmpty())
        title = page->session.name;
    if (!page->shellConnected)
        title += QStringLiteral(" · 已断开");
    m_tabs->setTabText(index, title);
    m_tabs->setTabToolTip(index, page->session.displaySubtitle());
}

void MainWindow::setConfirmClose(bool on)
{
    m_confirmClose = on;
}

void MainWindow::teardownPage(Page *page)
{
    if (!page)
        return;
    // 先断信号：停线程/删对象期间 shell 仍可能发 closed/failed，
    // 回调里拿着已经 delete 掉的 Page 用就是段错误（表现为"叉掉标签整个程序退出"）。
    if (page->shell) {
        QObject::disconnect(page->shell, nullptr, this, nullptr);
        page->shell->stop();
    }
    if (page->sftp) {
        QObject::disconnect(page->sftp, nullptr, this, nullptr);
        page->sftp->stop();
    }
    if (page->terminal) {
        QObject::disconnect(page->terminal, nullptr, this, nullptr);
        QObject::disconnect(page->terminal, nullptr, page->shell, nullptr);
    }
    if (page->model)
        QObject::disconnect(page->model, nullptr, this, nullptr);
}

void MainWindow::closePage(int tabIndex)
{
    if (tabIndex < 0 || tabIndex >= m_pages.size())
        return;
    Page *page = m_pages.at(tabIndex);
    if (m_confirmClose) {
        const QString name = m_tabs->tabText(tabIndex);
        const auto answer = QMessageBox::question(
            this, QStringLiteral("关闭标签页"),
            QStringLiteral("确定关闭「%1」？\n该会话的连接会被断开。").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    m_pages.removeAt(tabIndex);
    m_tabs->removeTab(tabIndex);
    updateTabPlaceholder();

    teardownPage(page);
    if (page->shell) {
        page->shell->wait(3000);
        page->shell->deleteLater();
    }
    if (page->sftp) {
        page->sftp->wait(3000);
        page->sftp->deleteLater();
    }
    if (page->model) {
        if (currentPage() == nullptr || currentPage()->model != page->model)
            m_filePanel->detach();
        page->model->deleteLater();
    }
    if (page->terminal)
        page->terminal->deleteLater();
    delete page;

    updateStatusBar();
}

MainWindow::Page *MainWindow::currentPage() const
{
    const int index = m_tabs->currentIndex();
    if (index < 0 || index >= m_pages.size())
        return nullptr;
    return m_pages.at(index);
}

MainWindow::Page *MainWindow::findPageBySessionId(const QString &id) const
{
    for (Page *page : m_pages) {
        if (page->session.id == id)
            return page;
    }
    return nullptr;
}

int MainWindow::pageIndexOf(const Page *page) const
{
    for (int i = 0; i < m_pages.size(); ++i) {
        if (m_pages.at(i) == page)
            return i;
    }
    return -1;
}

void MainWindow::updateTabPlaceholder()
{
    if (m_tabStack)
        m_tabStack->setCurrentIndex(m_tabs->count() > 0 ? 1 : 0);
}

void MainWindow::onTabChanged(int index)
{
    Q_UNUSED(index);
    Page *page = currentPage();
    if (page && page->sftp && page->sftpReady) {
        m_filePanel->attach(page->model, page->sftp);
    } else if (page) {
        m_filePanel->detach();
        m_filePanel->setStatus(QStringLiteral("SFTP 连接中…"));
    } else {
        m_filePanel->detach();
    }
    if (page) {
        m_sessionPanel->setActiveSessionId(page->session.id);
        page->terminal->setFocus(Qt::OtherFocusReason);
    }
    updateStatusBar();
}

void MainWindow::updateStatusBar()
{
    Page *page = currentPage();
    if (!page) {
        m_statusInfo->setText(QStringLiteral("无活动会话"));
        return;
    }
    const VtScreen *screen = page->terminal->screen();
    m_statusInfo->setText(QStringLiteral("%1 × %2  |  %3  |  %4")
                              .arg(screen->cols())
                              .arg(screen->rows())
                              .arg(page->session.displayTarget())
                              .arg(page->shellConnected ? QStringLiteral("SSH 已连接")
                                                        : QStringLiteral("未连接")));
}

TerminalWidget *MainWindow::currentTerminal() const
{
    Page *page = currentPage();
    return page ? page->terminal : nullptr;
}

void MainWindow::adjustFontSize(int delta)
{
    Theme &theme = Theme::mutableInstance();
    theme.setTerminalFontSize(theme.terminalFontSize() + delta);
    for (Page *page : m_pages) {
        if (page->terminal)
            page->terminal->applyFont();
    }
    persistUiState();
}

void MainWindow::persistUiState()
{
    QSettings settings;
    settings.setValue(QStringLiteral("terminal/fontSize"), Theme::instance().terminalFontSize());
    settings.setValue(QStringLiteral("terminal/fontFamily"), Theme::instance().terminalFontFamily());
}

void MainWindow::confirmQuit()
{
    const QString detail = m_pages.isEmpty()
        ? QStringLiteral("确定退出 termdock？")
        : QStringLiteral("确定退出 termdock？\n当前有 %1 个会话，退出后会断开连接。")
              .arg(m_pages.size());
    if (QMessageBox::question(this, QStringLiteral("退出 termdock"), detail,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;
    m_quitConfirmed = true;
    close();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_confirmClose && !m_quitConfirmed) {
        // 关键：不要在 closeEvent 里直接 exec 模态框——有些窗口管理器会把
        // 关闭事件和模态循环搅在一起（对话框不显示 / 卡死）。先拒绝关闭，
        // 下一轮事件循环再问；确认后带着标记重新 close()。
        event->ignore();
        QTimer::singleShot(0, this, &MainWindow::confirmQuit);
        return;
    }
    for (Page *page : m_pages)
        teardownPage(page);
    QSettings settings;
    settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/splitter"), m_splitter->saveState());
    persistUiState();
    event->accept();
}

} // namespace termdock
