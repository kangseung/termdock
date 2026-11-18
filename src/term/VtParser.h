#pragma once

#include <QByteArray>
#include <QString>
#include <QStringDecoder>
#include <functional>
#include <vector>

namespace termdock {

class VtScreen;

// VT100 / xterm 子集解析器：把 pty 输出翻译成 VtScreen 的状态变化。
class VtParser {
public:
    explicit VtParser(VtScreen *screen);

    void feed(const QByteArray &data);
    void reset();

    void setRespondCallback(std::function<void(const QByteArray &)> cb) { m_respond = std::move(cb); }
    void setBellCallback(std::function<void()> cb) { m_bell = std::move(cb); }
    void setTitleCallback(std::function<void(const QString &)> cb) { m_titleChanged = std::move(cb); }

private:
    enum State {
        Ground,
        Escape,
        EscapeIntermediate,
        Csi,
        CsiIgnore,
        OscString,
        OscEscape,
        DcsString,
        DcsEscape,
        SosString,
        SosEscape,
    };

    void handleAscii(char32_t ch);
    void handleEsc(char32_t ch);
    void handleCsi(char32_t ch);
    void dispatchCsi(char final);
    void dispatchEsc(char final, const QByteArray &intermediates);

    void csiBegin();
    void csiPushDigit(char32_t ch);
    void csiEndNumber();
    int param(int index, int defValue = -1) const;
    int subParam(int index, int sub, int defValue = -1) const;
    int paramCount() const { return int(m_tokens.size()); }

    void applySgr();
    void setMode(bool on, bool priv);
    void respond(const char *text);
    void respond(const QByteArray &text);
    void putChar(char32_t ch);

    VtScreen *m_screen;
    QStringDecoder m_decoder{QStringDecoder::Utf8};

    State m_state = Ground;
    QByteArray m_intermediates;
    QByteArray m_prefix;
    std::vector<std::vector<int>> m_tokens;
    std::vector<int> m_currentToken;
    int m_currentNumber = 0;
    bool m_hasNumber = false;
    QString m_oscBuffer;
    char32_t m_lastPrinted = U' ';
    bool m_pendingHighSurrogate = false;
    char32_t m_highSurrogate = 0;

    std::function<void(const QByteArray &)> m_respond;
    std::function<void()> m_bell;
    std::function<void(const QString &)> m_titleChanged;
};

} // namespace termdock
