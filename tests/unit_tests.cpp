// termdock 单元测试：VT 内核 / 终端解析 / 加密 / 会话存储 / 恢复数据 / 数据目录规则
// 跑法：ctest --test-dir build --output-on-failure  （或直接 ./build/termdock-tests）
// 全程在临时沙箱里跑，不碰用户真实数据目录。

#include "../src/app/AppConfig.h"
#include "../src/session/Crypto.h"
#include "../src/session/Recovery.h"
#include "../src/session/Session.h"
#include "../src/session/SessionStore.h"
#include "../src/term/VtParser.h"
#include "../src/term/VtScreen.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>

using namespace termdock;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char *what, const QString &detail = QString())
{
    if (ok) {
        ++g_pass;
        std::printf("  PASS  %s\n", what);
    } else {
        ++g_fail;
        std::printf("  FAIL  %s%s\n", what, detail.isEmpty() ? "" : qPrintable(" (" + detail + ")"));
    }
}

QString rowText(const VtScreen &screen, int row, int cols)
{
    QString out;
    for (int c = 0; c < cols; ++c)
        out.append(QChar(screen.cellAt(row, c).ch));
    return out;
}

// ---------------- VT 内核 ----------------

void testVtBasics()
{
    VtScreen screen(10, 3, 100);
    VtParser parser(&screen);
    parser.feed("abc");
    check(screen.cellAt(0, 0).ch == U'a' && screen.cellAt(0, 2).ch == U'c', "基本字符写入缓冲区");
    check(screen.cursorRow() == 0 && screen.cursorCol() == 3, "写入后光标前进");
}

void testVtAutoWrap()
{
    VtScreen screen(5, 3, 100);
    VtParser parser(&screen);
    parser.feed("abcde");
    // 写满最后一行最后一列后光标停在末列（pending wrap），下一次输出才换行
    check(screen.cursorRow() == 0 && screen.cursorCol() == 4, "写满一行后仍停在末列");
    parser.feed("f");
    check(screen.cursorRow() == 1 && screen.cursorCol() == 1, "再写一个字符才换行");
    check(screen.cellAt(1, 0).ch == U'f', "换行后字符落在新行");
}

void testVtWideChar()
{
    VtScreen screen(10, 2, 100);
    VtParser parser(&screen);
    parser.feed(QStringLiteral("中文").toUtf8());
    check(screen.cellAt(0, 0).ch == U'中', "宽字符占第一格");
    check((screen.cellAt(0, 0).attr & AttrWide) != 0, "宽字符标记 AttrWide");
    check((screen.cellAt(0, 1).attr & AttrWideTail) != 0, "第二格标记 AttrWideTail");
    check(screen.cursorCol() == 4, "两个宽字符前进 4 列");
    check(VtScreen::charWidth(U'a') == 1 && VtScreen::charWidth(U'中') == 2,
          "charWidth 区分窄宽字符");
}

void testVtScrollback()
{
    VtScreen screen(10, 2, 100);
    VtParser parser(&screen);
    parser.feed("one\r\ntwo\r\nthree");
    check(screen.scrollbackCount() == 1, "溢出底行进入滚回", QString::number(screen.scrollbackCount()));
    check(screen.absoluteLineOf(0) == 1, "滚回后可视行 0 对应绝对行 1",
          QString::number(screen.absoluteLineOf(0)));
}

void testVtResizeGrowKeepsContent()
{
    VtScreen screen(10, 2, 100);
    VtParser parser(&screen);
    parser.feed("keep");
    screen.resize(20, 5);
    check(screen.rows() == 5 && screen.cols() == 20, "变大后行列更新");
    check(rowText(screen, 0, 4) == QStringLiteral("keep"), "变大后首行内容不动");
    check(screen.cursorRow() == 0 && screen.cursorCol() == 4, "变大后光标位置不动");
}

void testVtResizeShrinkKeepsTop()
{
    VtScreen screen(10, 4, 100);
    VtParser parser(&screen);
    parser.feed("r0\r\nr1\r\nr2\r\nr3");
    screen.resize(10, 2);
    check(rowText(screen, 0, 2) == QStringLiteral("r0"), "变小后顶部内容仍在（从上往下裁）");
    check(screen.scrollbackCount() == 2, "被裁掉的两行进滚回",
          QString::number(screen.scrollbackCount()));
}

void testVtResizeNarrowKeepsTail()
{
    VtScreen screen(20, 2, 100);
    VtParser parser(&screen);
    parser.feed("0123456789");
    screen.resize(5, 2);
    screen.resize(20, 2);
    check(rowText(screen, 0, 10) == QStringLiteral("0123456789"),
          "变窄再变宽，行尾字符没有被丢掉");
}

void testVtEraseAndEdit()
{
    VtScreen screen(10, 2, 100);
    VtParser parser(&screen);
    parser.feed("abcdef\x1b[3D\x1b[K");
    check(rowText(screen, 0, 3) == QStringLiteral("abc"), "EL 清除光标之后的内容");

    parser.feed("\x1b[1;1H\x1b[2P");
    check(rowText(screen, 0, 3) == QStringLiteral("c  "), "DCH 删除字符并左移");

    parser.feed("\x1b[1;1H\x1b[2@");
    check(rowText(screen, 0, 3) == QStringLiteral("  c"), "ICH 插入空格并右移");
}

void testVtScrollRegion()
{
    VtScreen screen(10, 4, 100);
    VtParser parser(&screen);
    parser.feed("a\r\nb\r\nc\r\nd");
    parser.feed("\x1b[1;3r"); // 滚动区域限定在 0~2 行
    parser.feed("\x1b[3;1H\n");
    check(screen.scrollTop() == 0 && screen.scrollBottom() == 2, "滚动区域按 DECSTBM 设置",
          QString("%1-%2").arg(screen.scrollTop()).arg(screen.scrollBottom()));
    check(screen.cellAt(0, 0).ch == U'b', "区域内滚动的首行上移");
    check(screen.cellAt(3, 0).ch == U'd', "区域外的行不受影响");
}

void testVtAltScreen()
{
    VtScreen screen(10, 3, 100);
    VtParser parser(&screen);
    parser.feed("primary");
    parser.feed("\x1b[?1049h");
    check(screen.inAltScreen(), "进入备用屏幕");
    parser.feed("\x1b[2J\x1b[1;1Halt");
    check(rowText(screen, 0, 3) == QStringLiteral("alt"), "备用屏幕内容独立");
    parser.feed("\x1b[?1049l");
    check(!screen.inAltScreen(), "退出备用屏幕");
    check(rowText(screen, 0, 7) == QStringLiteral("primary"), "主屏幕内容恢复");
}

void testVtCursorHelpers()
{
    VtScreen screen(20, 3, 100);
    VtParser parser(&screen);
    parser.feed("\x1b[5Gx"); // 跳到第 5 列再写一个字符
    check(screen.cellAt(0, 4).ch == U'x', "CHA 定位到第 5 列");
    parser.feed("\x1b[s\x1b[2;3H\x1b[u");
    check(screen.cursorRow() == 0 && screen.cursorCol() == 5, "SCP/RCP 保存恢复光标");

    parser.feed("\x1b[1;1H\t");
    check(screen.cursorCol() == 8, "TAB 跳到 8 的倍数");

    parser.feed("\x1b[1;1H\b");
    check(screen.cursorCol() == 0, "首列退格不会越界");
}

void testVtTextExtraction()
{
    VtScreen screen(20, 2, 100);
    VtParser parser(&screen);
    parser.feed("hello world");
    check(screen.textInRange(0, 0, 0, 4) == QStringLiteral("hello"), "textInRange 取区间文本",
          screen.textInRange(0, 0, 0, 4));
}

// ---------------- 解析器 ----------------

void testParserSgrAndOsc()
{
    VtScreen screen(20, 2, 100);
    VtParser parser(&screen);
    parser.feed("\x1b[1;31mx");
    const VtCell &cell = screen.cellAt(0, 0);
    check((cell.attr & AttrBold) != 0, "SGR 1 打开加粗");
    check(cell.fg.isIndexed() && cell.fg.index() == 1, "SGR 31 设置前景色 1",
          QString::number(cell.fg.index()));

    parser.feed("\x1b[0m");
    check(screen.pen().attr == 0 && screen.pen().fg.isDefault(), "SGR 0 复位");

    parser.feed("\x1b]0;termdock-title\x07");
    check(screen.title() == QStringLiteral("termdock-title"), "OSC 0 设置标题", screen.title());
}

void testParserSplitSequence()
{
    VtScreen screen(10, 3, 100);
    VtParser parser(&screen);
    parser.feed("\x1b[2;");
    parser.feed("3Hz");
    check(screen.cellAt(1, 2).ch == U'z', "跨 feed 的不完整 CSI 仍能解析");
    check(screen.cursorRow() == 1 && screen.cursorCol() == 3, "跨 feed 后光标位置正确");
}

void testParserUnfinishedOsc()
{
    VtScreen screen(10, 2, 100);
    VtParser parser(&screen);
    parser.feed("\x1b]0;unfinished");   // 没有 BEL / ST 就断掉
    check(screen.textInRange(0, 0, 0, 9).trimmed().isEmpty(), "未结束的 OSC 不写进缓冲区");
}

// ---------------- 加密 ----------------

void testCryptoRoundTrip()
{
    const QByteArray key = Crypto::randomBytes(32);
    check(key.size() == 32, "随机密钥长度正确");

    const QString encoded = Crypto::encryptWithKey(key, QStringLiteral("p@ss 中文").toUtf8());
    check(!encoded.isEmpty(), "加密返回密文");
    check(Crypto::decryptWithKey(key, encoded) == QStringLiteral("p@ss 中文").toUtf8(),
          "解密还原明文");

    const QString again = Crypto::encryptWithKey(key, QStringLiteral("p@ss 中文").toUtf8());
    check(again != encoded, "同一明文两次加密结果不同（IV 不复用）");
}

void testCryptoTamperDetected()
{
    const QByteArray key = Crypto::randomBytes(32);
    QString encoded = Crypto::encryptWithKey(key, QByteArray("secret"));
    // 密文最后一段是 GCM tag，翻掉一个字符必须解密失败而不是返回垃圾
    encoded[encoded.size() - 1] = encoded.at(encoded.size() - 1) == QLatin1Char('A')
        ? QLatin1Char('B') : QLatin1Char('A');
    QString error;
    const QByteArray out = Crypto::decryptWithKey(key, encoded, &error);
    check(out.isEmpty() && !error.isEmpty(), "篡改密文被 GCM 拒绝");
}

void testCryptoMissingKeyNotRecreated()
{
    const QString keyPath = AppConfig::keyFile();
    QFile::remove(keyPath);
    QString error;
    const QByteArray key = Crypto::existingMasterKey(&error);
    check(key.isEmpty() && !error.isEmpty(), "existingMasterKey 在密钥缺失时报错");
    check(!QFile::exists(keyPath), "读路径不会偷偷新建密钥文件");
}

void testCryptoUnwrapBounds()
{
    QJsonObject payload;
    payload.insert(QStringLiteral("version"), 1);
    payload.insert(QStringLiteral("kdf"), QStringLiteral("pbkdf2-sha256"));
    payload.insert(QStringLiteral("iterations"), 2000000000);
    payload.insert(QStringLiteral("salt"), QString::fromLatin1("AAAA"));
    payload.insert(QStringLiteral("wrapped"), QStringLiteral("gcm1:AAAA"));
    QString error;
    check(Crypto::unwrapMasterKey(payload, QStringLiteral("x"), &error).isEmpty() && !error.isEmpty(),
          "恢复数据迭代次数越界被拒绝");
}

// ---------------- 会话与恢复 ----------------

void testSessionRoundTrip()
{
    Session session;
    session.id = QStringLiteral("id-1");
    session.name = QStringLiteral("测试机");
    session.host = QStringLiteral("192.0.2.10");
    session.port = 2222;
    session.user = QStringLiteral("root");
    session.note = QStringLiteral("备注");

    const Session parsed = Session::fromJson(session.toJson());
    check(parsed.id == session.id && parsed.host == session.host && parsed.port == 2222
              && parsed.user == session.user && parsed.note == session.note && parsed.name == session.name,
          "Session JSON 往返保持一致");
}

void testSessionPortClamp()
{
    QJsonObject raw;
    raw.insert(QStringLiteral("host"), QStringLiteral("h"));
    raw.insert(QStringLiteral("port"), 70000);
    check(Session::fromJson(raw).port == 22, "非法端口回落到 22");
}

void testSessionKeepsUnreadableCipher()
{
    QJsonObject raw;
    raw.insert(QStringLiteral("name"), QStringLiteral("t"));
    raw.insert(QStringLiteral("host"), QStringLiteral("h"));
    raw.insert(QStringLiteral("password"), QStringLiteral("gcm1:broken"));
    const Session parsed = Session::fromJson(raw);
    check(parsed.password.isEmpty(), "解不开的密码不当作明文");
    check(parsed.toJson().value(QStringLiteral("password")).toString() == QStringLiteral("gcm1:broken"),
          "保存时原样保留密文");
}

void testRecoveryLifecycle()
{
    QString error;
    check(Recovery::enable(QStringLiteral("unit-test-pass"), &error), "开启安全密钥成功", error);
    check(Recovery::enabled(), "开启后状态为已启用");

    const QByteArray original = Crypto::masterKey(&error);
    QByteArray unlocked;
    check(Recovery::unlock(QStringLiteral("unit-test-pass"), &unlocked, &error)
              && unlocked == original,
          "正确口令可以解出主密钥");
    check(!Recovery::unlock(QStringLiteral("wrong-pass"), &unlocked, &error),
          "错误口令被拒绝");
    check(Recovery::disable(), "关闭安全密钥成功");
    check(!QFile::exists(Recovery::filePath()), "关闭后 recovery.json 被删除");
}

void testKnownHostsStates()
{
    // 沙箱里没有 known_hosts.json → 首次连接
    check(SessionStore::knownHostsState() == SessionStore::KnownHosts::Missing,
          "没有指纹库时状态为首次连接");

    QString error;
    check(SessionStore::rememberFingerprint(QStringLiteral("192.0.2.1"), 22,
                                            QStringLiteral("SHA256:abc"), &error),
          "写入指纹成功", error);
    check(SessionStore::knownHostsState() == SessionStore::KnownHosts::Ok, "写入后状态正常");
    check(SessionStore::knownFingerprint(QStringLiteral("192.0.2.1"), 22)
              == QStringLiteral("SHA256:abc"),
          "读回刚写入的指纹");

    // 把文件写坏：必须报"损坏"，不能被当成首次连接直接信任
    QFile broken(AppConfig::knownHostsFile());
    check(broken.open(QIODevice::WriteOnly | QIODevice::Truncate), "可以写坏指纹库做测试");
    broken.write("{ not json");
    broken.close();
    QString corruptError;
    check(SessionStore::knownHostsState(&corruptError) == SessionStore::KnownHosts::Corrupt,
          "指纹库损坏时状态为 Corrupt");
    check(!corruptError.isEmpty(), "损坏时给出可读的错误信息", corruptError);
}

void testPassphraseMinimum()
{
    QString error;
    check(Crypto::wrapMasterKey(QStringLiteral("1234567"), &error).isEmpty() && !error.isEmpty(),
          "7 位口令被拒绝（最少 8 位）");
    error.clear();
    check(!Crypto::wrapMasterKey(QStringLiteral("12345678"), &error).isEmpty(),
          "8 位口令可以设置", error);
}

// ---------------- 数据目录规则 ----------------

void testDataDirRules()
{
    // 命令行 --data-dir 是本次运行的显式要求：允许但必须告警（只拒绝环境变量与配置里的临时目录）
    AppConfig::setDataDirOverride(QStringLiteral("/tmp/termdock-must-not-use"));
    check(AppConfig::dataDir() == QStringLiteral("/tmp/termdock-must-not-use"),
          "命令行覆盖到临时目录仍生效（仅告警）", AppConfig::dataDir());
    AppConfig::setDataDirOverride(QString());
}

} // namespace

int main(int argc, char **argv)
{
    // 沙箱：所有读写都落在临时目录，绝不碰真实配置
    QTemporaryDir sandbox;
    if (!sandbox.isValid()) {
        std::printf("无法创建临时沙箱目录\n");
        return 2;
    }
    qputenv("XDG_CONFIG_HOME", sandbox.path().toUtf8());
    qunsetenv("TERMDOCK_DATA_DIR");

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("termdock-tests"));

    std::printf("== VT 内核\n");
    testVtBasics();
    testVtAutoWrap();
    testVtWideChar();
    testVtScrollback();
    testVtResizeGrowKeepsContent();
    testVtResizeShrinkKeepsTop();
    testVtResizeNarrowKeepsTail();
    testVtEraseAndEdit();
    testVtScrollRegion();
    testVtAltScreen();
    testVtCursorHelpers();
    testVtTextExtraction();

    std::printf("== 解析器\n");
    testParserSgrAndOsc();
    testParserSplitSequence();
    testParserUnfinishedOsc();

    std::printf("== 加密\n");
    testCryptoRoundTrip();
    testCryptoTamperDetected();
    testCryptoUnwrapBounds();

    std::printf("== 会话与恢复\n");
    testSessionRoundTrip();
    testSessionPortClamp();
    testSessionKeepsUnreadableCipher();
    testRecoveryLifecycle();

    std::printf("== 主机指纹库\n");
    testKnownHostsStates();

    std::printf("== 口令策略\n");
    testPassphraseMinimum();

    std::printf("== 数据目录规则\n");
    testDataDirRules();
    testCryptoMissingKeyNotRecreated(); // 放最后：它会删掉沙箱里的 key 文件

    std::printf("\n结果：%d 通过，%d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
