#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include "app/AppConfig.h"
#include "app/Theme.h"
#include "session/Crypto.h"
#include "session/Session.h"
#include "ssh/SftpWorker.h"
#include "ssh/ShellSession.h"
#include "term/VtParser.h"
#include "term/VtScreen.h"
#include "ui/MainWindow.h"

using namespace termdock;

namespace {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

struct TestRunner {
    int checks = 0;
    int failures = 0;

    void check(bool condition, const QString &name, const QString &detail = QString())
    {
        ++checks;
        if (condition) {
            out() << "  PASS  " << name << "\n";
        } else {
            ++failures;
            out() << "  FAIL  " << name;
            if (!detail.isEmpty())
                out() << "  (" << detail << ")";
            out() << "\n";
        }
        out().flush();
    }
    void section(const QString &name)
    {
        out() << "\n[" << name << "]\n";
        out().flush();
    }
};

QString cellText(const VtScreen &screen)
{
    QString text;
    for (int col = 0; col < screen.cols(); ++col) {
        const VtCell &cell = screen.cellAt(0, col);
        if (cell.ch == 0)
            continue;
        text.append(QString::fromUcs4(&cell.ch, 1));
    }
    return text.trimmed();
}

int runSelfTest()
{
    TestRunner test;
    out() << "termdock 自检（VT 解析 + 屏幕模型）\n";

    test.section(QStringLiteral("基础写入"));
    {
        VtScreen screen(80, 24);
        VtParser parser(&screen);
        parser.feed(QByteArrayLiteral("hello"));
        test.check(screen.cellAt(0, 0).ch == U'h', QStringLiteral("首个字符落位"));
        test.check(screen.cellAt(0, 4).ch == U'o', QStringLiteral("第五个字符落位"));
        test.check(screen.cursorCol() == 5, QStringLiteral("光标前进到第 6 列"),
                   QString::number(screen.cursorCol()));

        parser.feed(QByteArrayLiteral("\r\nab"));
        test.check(screen.cellAt(1, 0).ch == U'a', QStringLiteral("换行后写入新行"));
        test.check(screen.cursorRow() == 1, QStringLiteral("光标在新行"),
                   QString::number(screen.cursorRow()));
    }

    test.section(QStringLiteral("光标移动与擦除"));
    {
        VtScreen screen(80, 24);
        VtParser parser(&screen);
        parser.feed(QByteArrayLiteral("abcdef\x1b[2D"));
        test.check(screen.cursorCol() == 4, QStringLiteral("CSI 2D 左移 2 列"),
                   QString::number(screen.cursorCol()));
        parser.feed(QByteArrayLiteral("\x1b[K"));
        test.check(screen.cellAt(0, 4).ch == U' ', QStringLiteral("EL 擦到行尾"));
        parser.feed(QByteArrayLiteral("\x1b[1;1H\x1b[0J"));
        test.check(screen.cellAt(0, 0).ch == U' ', QStringLiteral("ED 0 从光标清到屏幕尾"));
        parser.feed(QByteArrayLiteral("12345\x1b[1;2H\x1b[2P"));
        test.check(cellText(screen) == QStringLiteral("145"), QStringLiteral("DCH 删除 2 个字符"),
                   cellText(screen));
        parser.feed(QByteArrayLiteral("\x1b[1;1H\x1b[2@"));
        test.check(screen.cellAt(0, 0).ch == U' ' && screen.cellAt(0, 1).ch == U' '
                       && cellText(screen) == QStringLiteral("145"),
                   QStringLiteral("ICH 在行首插入 2 个空白"), cellText(screen));
    }

    test.section(QStringLiteral("SGR 颜色"));
    {
        VtScreen screen(80, 24);
        VtParser parser(&screen);
        parser.feed(QByteArrayLiteral("\x1b[31mR\x1b[0m"));
        test.check(screen.cellAt(0, 0).fg.isIndexed() && screen.cellAt(0, 0).fg.index() == 1,
                   QStringLiteral("基础前景色 31 → 调色板 1"));
        parser.feed(QByteArrayLiteral("\x1b[38;5;196mX"));
        test.check(screen.cellAt(0, 1).fg.isIndexed() && screen.cellAt(0, 1).fg.index() == 196,
                   QStringLiteral("256 色 38;5;196"));
        parser.feed(QByteArrayLiteral("\x1b[38;2;10;20;30mY"));
        test.check(screen.cellAt(0, 2).fg.isRgb()
                       && screen.cellAt(0, 2).fg.rgbValue() == ((10u << 16) | (20u << 8) | 30u),
                   QStringLiteral("真彩 38;2;r;g;b"));
        parser.feed(QByteArrayLiteral("\x1b[1;4mZ"));
        test.check((screen.cellAt(0, 3).attr & AttrBold) && (screen.cellAt(0, 3).attr & AttrUnderline),
                   QStringLiteral("粗体 + 下划线"));
    }

    test.section(QStringLiteral("UTF-8 与宽字符"));
    {
        VtScreen screen(20, 5);
        VtParser parser(&screen);
        parser.feed(QString::fromUtf8("你好世界").toUtf8());
        test.check(screen.cellAt(0, 0).ch == U'你', QStringLiteral("首个汉字（宽字符头部）"));
        test.check((screen.cellAt(0, 0).attr & AttrWide) != 0, QStringLiteral("宽字符标记"));
        test.check((screen.cellAt(0, 1).attr & AttrWideTail) != 0, QStringLiteral("宽字符尾部占位"));
        test.check(screen.cellAt(0, 2).ch == U'好', QStringLiteral("第二个汉字起点在第 3 列"));
        test.check(screen.cursorCol() == 8, QStringLiteral("4 个汉字占 8 列"),
                   QString::number(screen.cursorCol()));
    }

    test.section(QStringLiteral("自动换行与滚回"));
    {
        VtScreen screen(10, 4, 100);
        VtParser parser(&screen);
        parser.feed(QByteArray(25, 'x'));
        test.check(screen.cellAt(0, 9).ch == U'x', QStringLiteral("写满第一行"));
        test.check(screen.cellAt(1, 0).ch == U'x', QStringLiteral("自动换行到第二行"));
        parser.feed(QByteArrayLiteral("\r\n"));
        test.check(screen.cursorRow() >= 1, QStringLiteral("换行后光标下移"),
                   QString::number(screen.cursorRow()));

        VtScreen scrolled(10, 4, 100);
        VtParser scrolledParser(&scrolled);
        for (int i = 0; i < 12; ++i)
            scrolledParser.feed(QByteArrayLiteral("line\r\n"));
        test.check(scrolled.scrollbackCount() > 0, QStringLiteral("超出行数进入滚回缓冲"),
                   QString::number(scrolled.scrollbackCount()));
        scrolled.setScrollOffset(scrolled.scrollbackCount());
        test.check(scrolled.absoluteLineOf(0) == 0, QStringLiteral("滚回顶部可见最老行"),
                   QString::number(scrolled.absoluteLineOf(0)));
        scrolled.scrollToBottom();
        test.check(scrolled.scrollOffset() == 0, QStringLiteral("滚回底部归零"));
    }

    test.section(QStringLiteral("备用屏与标题"));
    {
        VtScreen screen(40, 10);
        VtParser parser(&screen);
        parser.feed(QByteArrayLiteral("primary"));
        parser.feed(QByteArrayLiteral("\x1b[?1049h"));
        test.check(screen.inAltScreen(), QStringLiteral("进入备用屏"));
        parser.feed(QByteArrayLiteral("\x1b[2J\x1b[Halt"));
        test.check(screen.cellAt(0, 0).ch == U'a', QStringLiteral("备用屏独立内容"));
        parser.feed(QByteArrayLiteral("\x1b[?1049l"));
        test.check(!screen.inAltScreen(), QStringLiteral("退出备用屏"));
        test.check(screen.cellAt(0, 0).ch == U'p', QStringLiteral("主屏内容保留"));

        QByteArray title;
        VtParser titleParser(&screen);
        titleParser.setTitleCallback([&title](const QString &text) { title = text.toUtf8(); });
        titleParser.feed(QByteArrayLiteral("\x1b]0;my-title\x07"));
        test.check(screen.title() == QStringLiteral("my-title"), QStringLiteral("OSC 0 设置标题"));
        test.check(title == QByteArrayLiteral("my-title"), QStringLiteral("标题回调触发"));
    }

    test.section(QStringLiteral("终端应答（CPR / DA）"));
    {
        VtScreen screen(40, 10);
        VtParser parser(&screen);
        QByteArray response;
        parser.setRespondCallback([&response](const QByteArray &data) { response += data; });
        parser.feed(QByteArrayLiteral("\x1b[3;5H\x1b[6n"));
        test.check(response == QByteArrayLiteral("\x1b[3;5R"), QStringLiteral("CPR 应答光标位置"),
                   QString::fromLatin1(response));
        response.clear();
        parser.feed(QByteArrayLiteral("\x1b[c"));
        test.check(response == QByteArrayLiteral("\x1b[?1;2c"), QStringLiteral("DA 应答(主设备属性)"),
                   QString::fromLatin1(response));
        response.clear();
        parser.feed(QByteArrayLiteral("\x1b[2J"));
        test.check(response.isEmpty(), QStringLiteral("ED 不产生应答"));
    }

    test.section(QStringLiteral("选区复制文本"));
    {
        VtScreen screen(40, 10);
        VtParser parser(&screen);
        parser.feed(QByteArrayLiteral("first line\r\nsecond line"));
        const QString text = screen.textInRange(0, 0, 1, 10);
        test.check(text == QStringLiteral("first line\nsecond line"), QStringLiteral("跨行取文本"),
                   text);
    }

    test.section(QStringLiteral("路径与配置"));
    {
        test.check(!AppConfig::configDir().isEmpty(), QStringLiteral("配置目录可解析"),
                   AppConfig::configDir());
        test.check(AppConfig::sessionsFile().endsWith(QStringLiteral("sessions.json")),
                   QStringLiteral("会话文件路径"));

        QString cryptoError;
        const QString secret = QStringLiteral("hs-secret-测试-42");
        const QString encrypted = Crypto::encrypt(secret, &cryptoError);
        test.check(!encrypted.isEmpty(), QStringLiteral("密码可加密"), cryptoError);
        test.check(encrypted != secret && !encrypted.contains(secret),
                   QStringLiteral("密文不含明文"));
        test.check(Crypto::decrypt(encrypted, &cryptoError) == secret,
                   QStringLiteral("密码可解密回明文"), cryptoError);
        QString badError;
        Crypto::decrypt(QStringLiteral("gcm1:AAAA"), &badError);
        test.check(!badError.isEmpty(), QStringLiteral("损坏的密文会被拒绝"));
    }

    out() << "\n结果：" << (test.checks - test.failures) << "/" << test.checks << " 通过，"
          << test.failures << " 失败\n";
    out().flush();
    return test.failures == 0 ? 0 : 1;
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate())
            return true;
        QThread::msleep(15);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

int runConnectTest(const QString &host, int port, const QString &user, const QString &password)
{
    out() << "连接测试：" << user << "@" << host << ":" << port << "\n";
    if (password.isEmpty()) {
        out() << "  FAIL  未提供密码（用 TERMDOCK_PASSWORD 环境变量传入）\n";
        out().flush();
        return 2;
    }

    SshParams params;
    params.host = host;
    params.port = port;
    params.user = user;
    params.password = password;

    // 1) shell 通道
    ShellSession shell(params, 100, 30);
    QString output;
    QString failure;
    QString fingerprint;
    bool connected = false;
    QObject::connect(&shell, &ShellSession::connected, [&](const QString &fp, const QString &) {
        connected = true;
        fingerprint = fp;
    });
    QObject::connect(&shell, &ShellSession::readyRead,
                     [&](const QByteArray &data) { output += QString::fromUtf8(data); });
    QObject::connect(&shell, &ShellSession::failed,
                     [&](const QString &message) { failure = message; });
    shell.start();

    const bool gotConnect = waitUntil([&]() { return connected || !failure.isEmpty(); }, 20000);
    if (!gotConnect || !connected) {
        out() << "  FAIL  SSH 连接失败：" << (failure.isEmpty() ? QStringLiteral("超时") : failure)
              << "\n";
        shell.stop();
        shell.wait(3000);
        out().flush();
        return 1;
    }
    out() << "  PASS  SSH 密码认证成功，主机指纹 " << fingerprint << "\n";

    shell.sendInput(QByteArrayLiteral("echo TERMDOCK_$((6*7))\r"));
    const bool gotMarker = waitUntil(
        [&]() { return output.contains(QStringLiteral("TERMDOCK_42")); }, 15000);
    out() << (gotMarker ? "  PASS" : "  FAIL") << "  远端命令回显（echo TERMDOCK_$((6*7)) → 42）\n";
    if (!gotMarker)
        out() << "        收到的前 200 字：" << output.left(200) << "\n";

    shell.resizePty(90, 25);
    QThread::msleep(300);
    shell.sendInput(QByteArrayLiteral("printf 'COLS=%s\\n' \"$(tput cols 2>/dev/null || echo -)\"\r"));
    const bool gotCols = waitUntil(
        [&]() { return output.contains(QStringLiteral("COLS=")); }, 8000);
    out() << (gotCols ? "  PASS" : "  FAIL") << "  PTY 窗口尺寸生效\n";

    shell.stop();
    shell.wait(4000);

    // 2) SFTP 通道
    SftpWorker sftp(params);
    QString home;
    QString sftpFailure;
    SftpEntryList listed;
    QString listedPath;
    quint64 listedId = 0;
    QString lastError;
    QObject::connect(&sftp, &SftpWorker::connected, [&](const QString &path) { home = path; });
    QObject::connect(&sftp, &SftpWorker::failed,
                     [&](const QString &message) { sftpFailure = message; });
    QObject::connect(&sftp, &SftpWorker::listed,
                     [&](quint64 id, const QString &path, const SftpEntryList &entries) {
                         listedId = id;
                         listedPath = path;
                         listed = entries;
                     });
    QObject::connect(&sftp, &SftpWorker::failedOp,
                     [&](quint64, const QString &message) { lastError = message; });
    sftp.start();

    const bool gotSftp = waitUntil([&]() { return !home.isEmpty() || !sftpFailure.isEmpty(); }, 20000);
    if (!gotSftp || home.isEmpty()) {
        out() << "  FAIL  SFTP 初始化失败："
              << (sftpFailure.isEmpty() ? QStringLiteral("超时") : sftpFailure) << "\n";
        sftp.stop();
        sftp.wait(3000);
        out().flush();
        return 1;
    }
    out() << "  PASS  SFTP 子系统就绪，home = " << home << "\n";

    sftp.enqueue(SftpWorker::Op::List, QStringLiteral("."));
    const bool gotList = waitUntil([&]() { return !listed.isEmpty() || !lastError.isEmpty(); }, 15000);
    out() << (gotList && !listed.isEmpty() ? "  PASS" : "  FAIL") << "  列目录 "
          << (gotList ? listedPath : home) << "（" << listed.size() << " 项）\n";
    if (!lastError.isEmpty())
        out() << "        错误：" << lastError << "\n";

    // 上传 → 下载 → 删除 的闭环校验
    const QString remote = QStringLiteral("/tmp/termdock_selftest_%1.txt").arg(QCoreApplication::applicationPid());
    const QString localSource = QDir::tempPath() + QStringLiteral("/termdock_upload_source.txt");
    QFile source(localSource);
    const QString payload = QStringLiteral("termdock sftp roundtrip %1").arg(QDateTime::currentSecsSinceEpoch());
    if (source.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        source.write(payload.toUtf8());
        source.close();
    }

    bool uploadOk = false;
    bool downloadOk = false;
    bool deleteOk = false;
    QString uploadedLocal;
    QString downloadedLocal;
    QString opError;
    QObject::connect(&sftp, &SftpWorker::succeeded, [&](quint64, const QString &message) {
        if (message.contains(QStringLiteral("已上传")))
            uploadOk = true;
        else if (message.contains(QStringLiteral("已下载")))
            downloadOk = true;
        else if (message.contains(QStringLiteral("已删除")))
            deleteOk = true;
    });
    QObject::connect(&sftp, &SftpWorker::failedOp, [&](quint64, const QString &message) {
        opError += message + QStringLiteral("\n");
    });

    uploadedLocal = localSource;
    sftp.enqueue(SftpWorker::Op::Upload, uploadedLocal, remote);
    const bool uploadFinished = waitUntil([&]() { return uploadOk || opError.contains(QStringLiteral("上传")); }, 15000);
    out() << (uploadFinished && uploadOk ? "  PASS" : "  FAIL") << "  上传文件 → " << remote << "\n";

    downloadedLocal = QDir::tempPath() + QStringLiteral("/termdock_roundtrip.txt");
    QFile::remove(downloadedLocal);
    sftp.enqueue(SftpWorker::Op::Download, remote, downloadedLocal);
    const bool downloadFinished = waitUntil([&]() { return downloadOk || opError.contains(QStringLiteral("下载")); }, 15000);
    bool contentMatches = false;
    if (downloadFinished && downloadOk) {
        QFile roundtrip(downloadedLocal);
        if (roundtrip.open(QIODevice::ReadOnly)) {
            const QString gotText = QString::fromUtf8(roundtrip.readAll()).trimmed();
            contentMatches = gotText == payload;
        }
    }
    out() << (contentMatches ? "  PASS" : "  FAIL") << "  下载回本地并校验内容一致\n";

    sftp.enqueue(SftpWorker::Op::Remove, remote);
    const bool deleteFinished = waitUntil([&]() { return deleteOk || opError.contains(QStringLiteral("删除")); }, 15000);
    out() << (deleteFinished && deleteOk ? "  PASS" : "  FAIL") << "  删除远端测试文件\n";
    if (!opError.isEmpty())
        out() << "        错误详情：" << opError.trimmed().replace(QLatin1Char('\n'), QLatin1String(" | ")) << "\n";

    sftp.stop();
    sftp.wait(4000);
    QFile::remove(localSource);
    QFile::remove(downloadedLocal);

    const bool allOk = gotMarker && gotCols && !listed.isEmpty() && contentMatches && deleteOk;
    out() << "\n连接测试结果：" << (allOk ? "全部通过" : "存在失败项") << "\n";
    out().flush();
    return allOk ? 0 : 1;
}

} // namespace

int main(int argc, char *argv[])
{
    // --selftest 会做加解密往返，必须在沙箱配置目录里跑，
    // 否则会创建/覆盖用户真实的 key 与 sessions.json。
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--selftest") == 0) {
            const QString sandbox = QDir::tempPath() + QStringLiteral("/termdock-selftest-config");
            QDir(sandbox).removeRecursively();
            qputenv("XDG_CONFIG_HOME", sandbox.toUtf8());
            break;
        }
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("termdock"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("termdock"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("termdock：精简的 Qt 终端 + 会话簿 + SFTP 文件树"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption selfTestOption(QStringLiteral("selftest"),
                                            QStringLiteral("运行终端内核自检后退出"));
    const QCommandLineOption connectTestOption(
        QStringLiteral("connect-test"),
        QStringLiteral("连接测试目标主机（密码从 TERMDOCK_PASSWORD 读取）"), QStringLiteral("host"));
    const QCommandLineOption connectOption(QStringLiteral("connect"),
                                           QStringLiteral("启动后直接连接该主机"),
                                           QStringLiteral("host"));
    const QCommandLineOption userOption(QStringLiteral("user"), QStringLiteral("登录账号"),
                                        QStringLiteral("user"));
    const QCommandLineOption portOption(QStringLiteral("port"), QStringLiteral("SSH 端口"),
                                        QStringLiteral("port"), QStringLiteral("22"));
    const QCommandLineOption screenshotOption(QStringLiteral("screenshot"),
                                              QStringLiteral("截图保存路径（测试用）"),
                                              QStringLiteral("path"));
    const QCommandLineOption screenshotDelayOption(QStringLiteral("screenshot-delay"),
                                                   QStringLiteral("截图前等待毫秒数"),
                                                   QStringLiteral("ms"), QStringLiteral("4000"));
    parser.addOption(selfTestOption);
    parser.addOption(connectTestOption);
    parser.addOption(connectOption);
    parser.addOption(userOption);
    parser.addOption(portOption);
    parser.addOption(screenshotOption);
    parser.addOption(screenshotDelayOption);
    parser.process(app);

    if (parser.isSet(selfTestOption))
        return runSelfTest();

    const QString password = qEnvironmentVariable("TERMDOCK_PASSWORD");

    if (parser.isSet(connectTestOption)) {
        const QString host = parser.value(connectTestOption);
        const QString user = parser.value(userOption);
        const int port = parser.value(portOption).toInt();
        return runConnectTest(host, port, user, password);
    }

    QApplication::setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(Theme::instance().styleSheet());

    MainWindow window;
    window.show();

    if (parser.isSet(connectOption)) {
        Session session = Session::createEmpty();
        session.name = parser.value(connectOption);
        session.host = parser.value(connectOption);
        session.port = parser.value(portOption).toInt();
        session.user = parser.value(userOption);
        session.password = password;
        if (!window.connectDirect(session))
            out() << "无法连接：参数不完整\n";
    }

    if (parser.isSet(screenshotOption)) {
        const QString path = parser.value(screenshotOption);
        const int delay = parser.value(screenshotDelayOption).toInt();
        QTimer::singleShot(delay, &app, [&window, path]() {
            const QPixmap pixmap = window.grab();
            if (pixmap.save(path)) {
                out() << "SCREENSHOT_OK " << path << " " << pixmap.width() << "x"
                      << pixmap.height() << "\n";
            } else {
                out() << "SCREENSHOT_FAIL " << path << "\n";
            }
            out().flush();
            QCoreApplication::quit();
        });
    }

    return QApplication::exec();
}
