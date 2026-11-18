#include <QApplication>
#include <QIcon>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QMouseEvent>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include "app/AppConfig.h"
#include "app/Theme.h"
#include "session/Crypto.h"
#include "session/Recovery.h"
#include "session/Session.h"
#include "ssh/SftpWorker.h"
#include "ssh/ShellSession.h"
#include "term/TerminalWidget.h"
#include "term/VtParser.h"
#include "term/VtScreen.h"

#include "ui/DataDirDialog.h"
#include "ui/VaultDialog.h"
#include "ui/MainWindow.h"

using namespace termdock;

namespace {

// 首次换到新目录时，如果旧位置 ~/.config/termdock 里有数据，问一句要不要导入
void maybeImportLegacyData(const QString &targetDir)
{
    const QString legacyDir = QDir::homePath() + QStringLiteral("/.config/termdock");
    if (QDir::cleanPath(targetDir) == QDir::cleanPath(legacyDir))
        return;
    const QStringList files{QStringLiteral("sessions.json"), QStringLiteral("key"),
                            QStringLiteral("known_hosts.json")};
    bool legacyHas = false;
    for (const QString &name : files)
        legacyHas = legacyHas || QFile::exists(legacyDir + QLatin1Char('/') + name);
    bool targetHas = false;
    for (const QString &name : files)
        targetHas = targetHas || QFile::exists(targetDir + QLatin1Char('/') + name);
    if (!legacyHas || targetHas)
        return;
    const auto answer = QMessageBox::question(
        nullptr, QStringLiteral("导入旧配置"),
        QStringLiteral("在 %1 发现之前的会话数据，要导入到新目录吗？\n\n"
                       "导入后会话列表和已保存的密码可直接使用。")
            .arg(legacyDir));
    if (answer != QMessageBox::Yes)
        return;
    for (const QString &name : files) {
        const QString from = legacyDir + QLatin1Char('/') + name;
        if (QFile::exists(from))
            QFile::copy(from, targetDir + QLatin1Char('/') + name);
    }
}

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

    test.section(QStringLiteral("鼠标上报"));
    {
        VtScreen screen(40, 10);
        VtParser parser(&screen);
        parser.feed(QByteArrayLiteral("\x1b[?1000h"));
        test.check(screen.mouseMode() == VtScreen::MouseMode::Click, QStringLiteral("1000h 开启点击上报"));
        parser.feed(QByteArrayLiteral("\x1b[?1002h"));
        test.check(screen.mouseMode() == VtScreen::MouseMode::Drag, QStringLiteral("1002h 开启拖动上报"));
        parser.feed(QByteArrayLiteral("\x1b[?1006h"));
        test.check(screen.sgrMouse(), QStringLiteral("1006h 开启 SGR 编码"));
        parser.feed(QByteArrayLiteral("\x1b[?1002l"));
        test.check(screen.mouseMode() == VtScreen::MouseMode::Off, QStringLiteral("1002l 关闭上报"));

        TerminalWidget widget;
        widget.resize(400, 200);
        widget.applyFont();
        widget.setConnected(true);
        QByteArray mouseBytes;
        QObject::connect(&widget, &TerminalWidget::dataToSend,
                         [&mouseBytes](const QByteArray &data) { mouseBytes += data; });
        widget.screen()->setMouseMode(VtScreen::MouseMode::Click);
        widget.screen()->setSgrMouse(true);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(14, 12), QPointF(14, 12),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &press);
        test.check(mouseBytes == QByteArrayLiteral("\x1b[<0;1;1M"), QStringLiteral("SGR 按下编码"),
                   QString::fromLatin1(mouseBytes));
        mouseBytes.clear();
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(14, 12), QPointF(14, 12),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &release);
        test.check(mouseBytes == QByteArrayLiteral("\x1b[<0;1;1m"), QStringLiteral("SGR 抬起编码"),
                   QString::fromLatin1(mouseBytes));
        mouseBytes.clear();
        widget.screen()->setSgrMouse(false);
        QMouseEvent pressX10(QEvent::MouseButtonPress, QPointF(14, 12), QPointF(14, 12),
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &pressX10);
        test.check(mouseBytes.size() == 6 && mouseBytes.startsWith(QByteArrayLiteral("\x1b[M")),
                   QStringLiteral("X10 编码"), QString::fromLatin1(mouseBytes.toHex()));
        mouseBytes.clear();
        QMouseEvent shiftPress(QEvent::MouseButtonPress, QPointF(14, 12), QPointF(14, 12),
                               Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(&widget, &shiftPress);
        test.check(mouseBytes.isEmpty(), QStringLiteral("Shift+点击保留本地选区不上报"));
    }

    test.section(QStringLiteral("路径与配置"));
    {
        test.check(!AppConfig::configDir().isEmpty(), QStringLiteral("配置目录可解析"),
                   AppConfig::configDir());
        test.check(!QIcon(QStringLiteral(":/icons/termdock-256.png")).isNull(),
                   QStringLiteral("内置应用图标可加载"));

        // 安全审查修复的回归断言
        {
            // 1) 解不开的密文必须原样保留，不能被一次保存抹掉
            QJsonObject raw;
            raw.insert(QStringLiteral("name"), QStringLiteral("t"));
            raw.insert(QStringLiteral("host"), QStringLiteral("h"));
            raw.insert(QStringLiteral("password"), QStringLiteral("gcm1:broken"));
            const Session parsed = Session::fromJson(raw);
            test.check(parsed.toJson().value(QStringLiteral("password")).toString()
                           == QStringLiteral("gcm1:broken"),
                       QStringLiteral("解密失败时保留原始密文"));
        }
        {
            // 2) 恢复数据里的 iterations 越界必须拒绝，不能把 GUI 卡死
            QJsonObject payload;
            payload.insert(QStringLiteral("version"), 1);
            payload.insert(QStringLiteral("kdf"), QStringLiteral("pbkdf2-sha256"));
            payload.insert(QStringLiteral("iterations"), 2000000000);
            payload.insert(QStringLiteral("salt"), QString::fromLatin1("AAAA"));
            payload.insert(QStringLiteral("wrapped"), QStringLiteral("gcm1:AAAA"));
            QString boundsError;
            test.check(Crypto::unwrapMasterKey(payload, QStringLiteral("x"), &boundsError).isEmpty()
                           && !boundsError.isEmpty(),
                       QStringLiteral("恢复数据迭代次数越界被拒绝"));
        }
        {
            // 3) TERMDOCK_DATA_DIR 指向临时目录时要被忽略
            qputenv("TERMDOCK_DATA_DIR", "/tmp/termdock-should-ignore");
            const QString ignored = AppConfig::dataDir();
            qunsetenv("TERMDOCK_DATA_DIR");
            // 自检本身跑在 /tmp 沙箱里，所以只断言"没有采用那个环境变量"
            test.check(ignored != QStringLiteral("/tmp/termdock-should-ignore"),
                       QStringLiteral("环境变量指向临时目录时被忽略"), ignored);
        }

        // 反复 resize 压力测试：曾因为裁剪用固定次数 pop_back 而踩空段错误
        {
            VtScreen stress(80, 24, 2000);
            VtParser parser(&stress);
            bool sizesOk = true;
            for (int i = 0; i < 400; ++i) {
                parser.feed(QString("line %1  中文 mixed 0123456789\r\n").arg(i).toUtf8());
                const int c = 10 + (i * 37) % 220;
                const int r = 1 + (i * 13) % 70;
                stress.resize(c, r);
                if (stress.rows() != r || stress.cols() != c) {
                    sizesOk = false;
                    break;
                }
            }
            test.check(sizesOk, QStringLiteral("极端尺寸反复 resize 不崩溃、行列数正确"));
        }

        // 输入法（fcitx5 中文）提交路径：QInputMethodEvent 的 commitString 要原样送到 PTY
        if (QGuiApplication::platformName() != QLatin1String("offscreen")) {
            test.check(true, QStringLiteral("输入法提交（需要窗口平台，已跳过）"));
        } else {
            TerminalWidget widget;
            widget.setConnected(true);
            QByteArray sent;
            QObject::connect(&widget, &TerminalWidget::dataToSend,
                             [&sent](const QByteArray &d) { sent += d; });
            QInputMethodEvent commit;
            commit.setCommitString(QStringLiteral("你好"));
            QCoreApplication::sendEvent(&widget, &commit);
            test.check(sent == QStringLiteral("你好").toUtf8(),
                       QStringLiteral("输入法提交串按 UTF-8 送到 PTY"),
                       QString::fromUtf8(sent.toHex()));
            QInputMethodEvent preedit(QStringLiteral("nihao"), {});
            QCoreApplication::sendEvent(&widget, &preedit);
            test.check(sent == QStringLiteral("你好").toUtf8(),
                       QStringLiteral("预编辑阶段不发送任何字节"));
        }
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

        // 显式密钥加解密（恢复保险依赖它）
        const QByteArray key = Crypto::masterKey();
        const QString payload = Crypto::encryptWithKey(key, QStringLiteral("secret-值").toUtf8());
        test.check(QString::fromUtf8(Crypto::decryptWithKey(key, payload))
                       == QStringLiteral("secret-值"),
                   QStringLiteral("显式密钥加解密往返"));

        // 恢复保险：开启 → 用口令解出主密钥 → 错误口令被拒 → 关闭
        const QString passphrase = QStringLiteral("termdock-recovery-test");
        QString recoveryError;
        test.check(Recovery::enable(passphrase, &recoveryError), QStringLiteral("恢复保险可开启"),
                   recoveryError);
        test.check(Recovery::enabled(), QStringLiteral("recovery.json 已生成"));
        QByteArray recovered;
        test.check(Recovery::unlock(passphrase, &recovered, &recoveryError)
                       && recovered == Crypto::masterKey(),
                   QStringLiteral("恢复口令能解出主密钥"), recoveryError);
        QString wrongError;
        test.check(!Recovery::unlock(QStringLiteral("wrong-passphrase"), &recovered, &wrongError),
                   QStringLiteral("错误口令被拒绝"));
        test.check(!wrongError.isEmpty(), QStringLiteral("错误口令有明确报错"));
        test.check(Recovery::disable(), QStringLiteral("恢复保险可关闭"));
        test.check(!Recovery::enabled(), QStringLiteral("关闭后 recovery.json 已删除"));
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
    int uploadCount = 0;
    bool mkdirOk = false;
    bool dirDownloadOk = false;
    bool dirDeleteOk = false;
    QString uploadedLocal;
    QString downloadedLocal;
    QString opError;
    QObject::connect(&sftp, &SftpWorker::succeeded, [&](quint64, const QString &message) {
        if (message.contains(QStringLiteral("已创建目录")))
            mkdirOk = true;
        else if (message.contains(QStringLiteral("已上传"))) {
            uploadOk = true;
            ++uploadCount;
        } else if (message.contains(QStringLiteral("已下载目录")))
            dirDownloadOk = true;
        else if (message.contains(QStringLiteral("已下载")))
            downloadOk = true;
        else if (message.contains(QStringLiteral("已递归删除")))
            dirDeleteOk = true;
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

    // 目录级闭环：建目录 → 传两个文件 → 递归下载 → 递归删除
    const QString remoteDir = QStringLiteral("/tmp/termdock_dir_%1").arg(QCoreApplication::applicationPid());
    sftp.enqueue(SftpWorker::Op::Mkdir, remoteDir);
    const bool mkdirFinished = waitUntil([&]() { return mkdirOk || opError.contains(QStringLiteral("创建目录")); }, 10000);
    out() << (mkdirFinished && mkdirOk ? "  PASS" : "  FAIL") << "  创建远端目录 " << remoteDir << "\n";

    const QString localA = QDir::tempPath() + QStringLiteral("/termdock_dir_a.txt");
    const QString localB = QDir::tempPath() + QStringLiteral("/termdock_dir_b.txt");
    for (const auto &pair : {qMakePair(localA, QStringLiteral("alpha-content")),
                             qMakePair(localB, QStringLiteral("beta-content"))}) {
        QFile f(pair.first);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(pair.second.toUtf8());
            f.close();
        }
    }
    uploadCount = 0;
    sftp.enqueue(SftpWorker::Op::Upload, localA, remoteDir + QStringLiteral("/a.txt"));
    sftp.enqueue(SftpWorker::Op::Upload, localB, remoteDir + QStringLiteral("/b.txt"));
    const bool dirUploadFinished = waitUntil([&]() { return uploadCount >= 2; }, 20000);
    out() << (dirUploadFinished ? "  PASS" : "  FAIL") << "  上传 2 个文件进该目录\n";

    const QString localDir = QDir::tempPath()
        + QStringLiteral("/termdock_download_%1").arg(QCoreApplication::applicationPid());
    QDir(localDir).removeRecursively();
    sftp.enqueue(SftpWorker::Op::Download, remoteDir, localDir);
    const bool dirDownloadFinished = waitUntil(
        [&]() { return dirDownloadOk || opError.contains(QStringLiteral("下载目录")); }, 20000);
    bool dirContentOk = false;
    if (dirDownloadFinished && dirDownloadOk) {
        QFile fa(localDir + QStringLiteral("/a.txt"));
        QFile fb(localDir + QStringLiteral("/b.txt"));
        if (fa.open(QIODevice::ReadOnly) && fb.open(QIODevice::ReadOnly)) {
            dirContentOk = QString::fromUtf8(fa.readAll()).trimmed() == QStringLiteral("alpha-content")
                && QString::fromUtf8(fb.readAll()).trimmed() == QStringLiteral("beta-content");
        }
    }
    out() << (dirContentOk ? "  PASS" : "  FAIL") << "  递归下载目录并校验两个文件内容\n";

    sftp.enqueue(SftpWorker::Op::Remove, remoteDir);
    const bool dirDeleteFinished = waitUntil(
        [&]() { return dirDeleteOk || opError.contains(QStringLiteral("删除目录")); }, 20000);
    out() << (dirDeleteFinished && dirDeleteOk ? "  PASS" : "  FAIL") << "  递归删除该目录\n";
    QDir(localDir).removeRecursively();

    sftp.stop();
    sftp.wait(4000);
    QFile::remove(localA);
    QFile::remove(localB);
    QFile::remove(localSource);
    QFile::remove(downloadedLocal);

    const bool allOk = gotMarker && gotCols && !listed.isEmpty() && contentMatches && deleteOk
        && mkdirOk && dirUploadFinished && dirContentOk && dirDeleteOk;
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

#ifdef Q_OS_LINUX
    // 官方安装版 Qt（如 ~/Qt/6.8.2/gcc_64）的插件搜索路径只有它自己那份，
    // 系统里的 fcitx5 输入法插件在 /usr/lib/.../qt6/plugins，不补进来就永远打不了中文。
    {
        const QStringList candidates{
            QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/plugins"),
            QStringLiteral("/usr/lib/aarch64-linux-gnu/qt6/plugins"),
            QStringLiteral("/usr/lib64/qt6/plugins"),
            QStringLiteral("/usr/lib/qt6/plugins"),
            QStringLiteral("/usr/local/lib/qt6/plugins"),
        };
        QStringList paths = qEnvironmentVariable("QT_PLUGIN_PATH")
                                .split(QLatin1Char(':'), Qt::SkipEmptyParts);
        for (const QString &dir : candidates) {
            if (QDir(dir).exists() && !paths.contains(dir))
                paths.append(dir);
        }
        if (!paths.isEmpty())
            qputenv("QT_PLUGIN_PATH", paths.join(QLatin1Char(':')).toUtf8());
    }
#endif

    QApplication app(argc, argv);
    // 关标签页（哪怕关掉最后一个）只是关掉一个连接，绝不退出程序；
    // 真正退出只走关窗 / 菜单“退出”，见 MainWindow::closeEvent。
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setApplicationName(QStringLiteral("termdock"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.3"));
    QApplication::setOrganizationName(QStringLiteral("termdock"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/termdock-256.png")));

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
    const QCommandLineOption dataDirOption(
        QStringLiteral("data-dir"),
        QStringLiteral("直接指定数据保存目录（跳过首次启动选择）"), QStringLiteral("path"));
    const QCommandLineOption screenshotOption(QStringLiteral("screenshot"),
                                              QStringLiteral("截图保存路径（测试用）"),
                                              QStringLiteral("path"));
    const QCommandLineOption screenshotDelayOption(QStringLiteral("screenshot-delay"),
                                                   QStringLiteral("截图前等待毫秒数"),
                                                   QStringLiteral("ms"), QStringLiteral("4000"));
    parser.addOption(selfTestOption);
    parser.addOption(dataDirOption);
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

    // ---- 数据保存目录：优先命令行，其次首次启动向导 ----
    const bool headless = parser.isSet(selfTestOption) || parser.isSet(connectTestOption);
    if (parser.isSet(dataDirOption)) {
        // 命令行只是「本次运行」的临时覆盖，绝不写进持久配置
        AppConfig::setDataDirOverride(parser.value(dataDirOption));
        out() << "本次运行使用数据目录（不修改默认）：" << AppConfig::dataDir() << "\n";
    } else if (!AppConfig::hasStoredDataDir()) {
        if (headless) {
            AppConfig::setDataDirOverride(AppConfig::defaultDataDir());
        } else {
            DataDirDialog dialog(AppConfig::defaultDataDir());
            const QString chosen = (dialog.exec() == QDialog::Accepted) ? dialog.dataDir()
                                                                        : QString();
            const QString target = chosen.isEmpty() ? AppConfig::defaultDataDir() : chosen;
            if (!AppConfig::setDataDir(target)) {
                out() << "数据目录写入失败：" << target << "\n";
            } else {
                out() << "数据目录：" << AppConfig::dataDir() << "\n";
                if (!chosen.isEmpty())
                    maybeImportLegacyData(AppConfig::dataDir());
                if (!Recovery::enabled()) {
                    const auto wantsRecovery = QMessageBox::question(
                        nullptr, QStringLiteral("安全密钥（可选）"),
                        QStringLiteral(
                            "要不要顺手留一把「安全密钥」？\n\n"
                            "留了以后，凭这把密钥就能随时解锁查看已保存的 SSH 明文密码；"
                            "即使密钥文件丢失也能救回来。不开启不影响使用，主密钥丢失则历史密码无法解密。\n\n"
                            "以后随时可以在「文件 → 安全密钥与已保存的密码…」里设置或删除。"));
                    if (wantsRecovery == QMessageBox::Yes)
                        VaultDialog::promptEnable(nullptr);
                }
            }
        }
        out().flush();
    }

    MainWindow window;
    // 自动截图/脚本化场景不需要人工确认，避免卡在对话框上
    window.setConfirmClose(!parser.isSet(screenshotOption)
                           && qEnvironmentVariableIsEmpty("TERMDOCK_NO_CONFIRM"));
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
