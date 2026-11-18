#include "term/VtParser.h"

#include "term/VtScreen.h"

#include <QChar>
#include <algorithm>

namespace termdock {

VtParser::VtParser(VtScreen *screen)
    : m_screen(screen)
{
}

void VtParser::reset()
{
    m_state = Ground;
    m_intermediates.clear();
    m_prefix.clear();
    m_tokens.clear();
    m_currentToken.clear();
    m_currentNumber = 0;
    m_hasNumber = false;
    m_oscBuffer.clear();
    m_decoder = QStringDecoder(QStringDecoder::Utf8);
}

void VtParser::feed(const QByteArray &data)
{
    const QString text = m_decoder.decode(data);
    for (int i = 0; i < text.size(); ++i) {
        const char16_t u = text.at(i).unicode();
        if (QChar::isHighSurrogate(u)) {
            m_pendingHighSurrogate = true;
            m_highSurrogate = u;
            continue;
        }
        if (QChar::isLowSurrogate(u) && m_pendingHighSurrogate) {
            const char32_t cp = QChar::surrogateToUcs4(char16_t(m_highSurrogate), u);
            m_pendingHighSurrogate = false;
            if (m_state == Ground)
                putChar(cp);
            continue;
        }
        m_pendingHighSurrogate = false;
        const char32_t ch = char32_t(u);
        if (m_state == Ground) {
            handleAscii(ch);
            continue;
        }
        switch (m_state) {
        case Escape:
            handleEsc(ch);
            break;
        case EscapeIntermediate:
            if (ch == '8' && m_intermediates == "#") {
                // DECALN：VT 测试图样
                for (int r = 0; r < m_screen->rows(); ++r)
                    for (int c = 0; c < m_screen->cols(); ++c) {
                        m_screen->setCursor(r, c);
                        m_screen->putChar(U'E');
                    }
                m_screen->setCursor(0, 0);
            }
            m_intermediates.clear();
            m_state = Ground;
            break;
        case Csi:
        case CsiIgnore:
            if (ch == 0x1B) {
                csiBegin();
                m_state = Escape;
            } else if (ch == 0x18 || ch == 0x1A) {
                m_state = Ground;
            } else {
                handleCsi(ch);
            }
            break;
        case OscString:
            if (ch == 0x07) {
                const QString text2 = m_oscBuffer;
                m_oscBuffer.clear();
                m_state = Ground;
                const int sep = text2.indexOf(QLatin1Char(';'));
                const QString codeStr = sep < 0 ? text2 : text2.left(sep);
                const QString rest = sep < 0 ? QString() : text2.mid(sep + 1);
                bool ok = false;
                const int code = codeStr.toInt(&ok);
                if (ok && (code == 0 || code == 1 || code == 2)) {
                    m_screen->setTitle(rest);
                    if (m_titleChanged)
                        m_titleChanged(rest);
                }
            } else if (ch == 0x1B) {
                m_state = OscEscape;
            } else if (ch >= 0x20) {
                m_oscBuffer.append(QString::fromUcs4(&ch, 1));
                if (m_oscBuffer.size() > 4096)
                    m_oscBuffer = m_oscBuffer.right(512);
            }
            break;
        case OscEscape:
            m_state = (ch == '\\') ? Ground : OscString;
            break;
        case DcsString:
        case SosString:
            if (ch == 0x1B)
                m_state = (m_state == DcsString) ? DcsEscape : SosEscape;
            break;
        case DcsEscape:
            m_state = (ch == '\\') ? Ground : DcsString;
            break;
        case SosEscape:
            m_state = (ch == '\\') ? Ground : SosString;
            break;
        case Ground:
            break;
        }
    }
}

void VtParser::putChar(char32_t ch)
{
    m_lastPrinted = ch;
    m_screen->putChar(ch);
}

void VtParser::handleAscii(char32_t ch)
{
    switch (ch) {
    case 0x00:
    case 0x7F:
        return;
    case 0x07:
        if (m_bell)
            m_bell();
        return;
    case 0x08:
        m_screen->backspace();
        return;
    case 0x09:
        m_screen->tab(1);
        return;
    case 0x0A:
    case 0x0B:
    case 0x0C:
        m_screen->lineFeed();
        return;
    case 0x0D:
        m_screen->carriageReturn();
        return;
    case 0x0E:
    case 0x0F:
        return;
    case 0x18:
    case 0x1A:
        return;
    case 0x1B:
        m_state = Escape;
        return;
    default:
        break;
    }
    if (ch < 0x20 || (ch >= 0x80 && ch < 0xA0))
        return;
    putChar(ch);
}

void VtParser::handleEsc(char32_t ch)
{
    switch (ch) {
    case '[':
        csiBegin();
        m_state = Csi;
        break;
    case ']':
        m_oscBuffer.clear();
        m_state = OscString;
        break;
    case 'P':
        m_state = DcsString;
        break;
    case 'X':
    case '^':
    case '_':
        m_state = SosString;
        break;
    case '7':
        m_screen->saveCursor();
        m_state = Ground;
        break;
    case '8':
        m_screen->restoreCursor();
        m_state = Ground;
        break;
    case 'D':
        m_screen->lineFeed();
        m_state = Ground;
        break;
    case 'E':
        m_screen->nextLine(1);
        m_state = Ground;
        break;
    case 'M':
        m_screen->reverseIndex();
        m_state = Ground;
        break;
    case 'H':
        m_state = Ground; // HTS：使用固定 8 列制表位
        break;
    case 'c':
        m_screen->fullReset();
        reset();
        break;
    case '=':
        m_screen->setApplicationKeypad(true);
        m_state = Ground;
        break;
    case '>':
        m_screen->setApplicationKeypad(false);
        m_state = Ground;
        break;
    case '\\':
        m_state = Ground;
        break;
    case 'N':
    case 'O':
        m_state = Ground;
        break;
    case 'Z':
        respond("\x1b[?1;2c");
        m_state = Ground;
        break;
    case '6':
    case '9':
        m_state = Ground;
        break;
    case '(':
    case ')':
    case '*':
    case '+':
    case '-':
    case '.':
    case '/':
    case '#':
    case ' ':
        m_intermediates.clear();
        m_intermediates.append(char(ch));
        m_state = EscapeIntermediate;
        break;
    default:
        m_state = Ground;
        break;
    }
}

void VtParser::csiBegin()
{
    m_tokens.clear();
    m_currentToken.clear();
    m_currentNumber = 0;
    m_hasNumber = false;
    m_intermediates.clear();
    m_prefix.clear();
}

void VtParser::csiPushDigit(char32_t ch)
{
    if (m_currentNumber < 100000)
        m_currentNumber = m_currentNumber * 10 + int(ch - '0');
    m_hasNumber = true;
}

void VtParser::csiEndNumber()
{
    m_currentToken.push_back(m_hasNumber ? m_currentNumber : -1);
    m_currentNumber = 0;
    m_hasNumber = false;
}

void VtParser::handleCsi(char32_t ch)
{
    if (ch >= '0' && ch <= '9') {
        if (!m_intermediates.isEmpty()) {
            m_state = CsiIgnore;
            return;
        }
        csiPushDigit(ch);
        return;
    }
    if (ch == ';') {
        csiEndNumber();
        m_tokens.push_back(m_currentToken);
        m_currentToken.clear();
        return;
    }
    if (ch == ':') {
        csiEndNumber();
        return;
    }
    if (ch >= 0x3C && ch <= 0x3F) {
        m_prefix.append(char(ch));
        return;
    }
    if (ch >= 0x20 && ch <= 0x2F) {
        m_intermediates.append(char(ch));
        return;
    }
    if (ch >= 0x40 && ch <= 0x7E) {
        csiEndNumber();
        m_tokens.push_back(m_currentToken);
        m_currentToken.clear();
        if (m_state != CsiIgnore)
            dispatchCsi(char(ch));
        m_state = Ground;
        return;
    }
    if (ch == 0x1B) {
        m_state = Escape;
        return;
    }
    // 其它控制字符直接忽略
}

int VtParser::param(int index, int defValue) const
{
    if (index < 0 || index >= int(m_tokens.size()))
        return defValue;
    if (m_tokens[size_t(index)].empty())
        return defValue;
    const int v = m_tokens[size_t(index)][0];
    return v < 0 ? defValue : v;
}

int VtParser::subParam(int index, int sub, int defValue) const
{
    if (index < 0 || index >= int(m_tokens.size()))
        return defValue;
    const auto &t = m_tokens[size_t(index)];
    if (sub < 0 || sub >= int(t.size()))
        return defValue;
    return t[size_t(sub)] < 0 ? defValue : t[size_t(sub)];
}

void VtParser::respond(const char *text)
{
    respond(QByteArray(text));
}

void VtParser::respond(const QByteArray &text)
{
    if (m_respond)
        m_respond(text);
}

void VtParser::dispatchCsi(char final)
{
    const int p1 = param(0, 1);
    const bool priv = !m_prefix.isEmpty();

    switch (final) {
    case '@':
        m_screen->insertChars(std::max(1, p1));
        break;
    case 'A':
        m_screen->moveUp(p1);
        break;
    case 'B':
    case 'e':
        m_screen->moveDown(p1);
        break;
    case 'C':
    case 'a':
        m_screen->moveRight(p1);
        break;
    case 'D':
        m_screen->moveLeft(p1);
        break;
    case 'E':
        m_screen->nextLine(p1);
        break;
    case 'F':
        m_screen->prevLine(p1);
        break;
    case 'G':
    case '`':
        m_screen->moveToCol(param(0, 1) - 1);
        break;
    case 'H':
    case 'f':
        m_screen->moveToRow(param(0, 1) - 1);
        m_screen->moveToCol(param(1, 1) - 1);
        break;
    case 'I':
        m_screen->tab(std::max(1, p1));
        break;
    case 'J':
        m_screen->eraseInDisplay(param(0, 0));
        break;
    case 'K':
        m_screen->eraseInLine(param(0, 0));
        break;
    case 'L':
        m_screen->insertLines(std::max(1, p1));
        break;
    case 'M':
        m_screen->deleteLines(std::max(1, p1));
        break;
    case 'P':
        m_screen->deleteChars(std::max(1, p1));
        break;
    case 'S':
        m_screen->scrollUpRegion(std::max(1, p1));
        break;
    case 'T':
        m_screen->scrollDownRegion(std::max(1, p1));
        break;
    case 'X':
        m_screen->eraseChars(std::max(1, p1));
        break;
    case 'Z':
        m_screen->backTab(std::max(1, p1));
        break;
    case 'b': {
        const int n = std::min(std::max(1, p1), 512);
        for (int i = 0; i < n; ++i)
            putChar(m_lastPrinted);
        break;
    }
    case 'c':
        if (m_prefix == ">")
            respond("\x1b[>0;276;0c");
        else if (m_prefix == "=")
            respond("\x1bP!|00000000\x1b\\");
        else
            respond("\x1b[?1;2c");
        break;
    case 'd':
        m_screen->moveToRow(param(0, 1) - 1);
        break;
    case 'g':
        break; // 固定制表位，忽略 TBC
    case 'h':
    case 'l':
        setMode(final == 'h', priv);
        break;
    case 'm':
        applySgr();
        break;
    case 'n': {
        const int what = param(0, 0);
        if (priv) {
            if (what == 6) {
                respond("\x1b[?" + QByteArray::number(m_screen->cursorRow() + 1) + ";"
                        + QByteArray::number(m_screen->cursorCol() + 1) + "R");
            }
            break;
        }
        if (what == 5)
            respond("\x1b[0n");
        else if (what == 6) {
            int row = m_screen->cursorRow() + 1;
            if (m_screen->originMode())
                row -= m_screen->scrollTop();
            respond("\x1b[" + QByteArray::number(row) + ";"
                    + QByteArray::number(m_screen->cursorCol() + 1) + "R");
        }
        break;
    }
    case 'p':
        if (m_prefix == "!") {
            VtPen pen;
            m_screen->pen() = pen;
            m_screen->setInsertMode(false);
            m_screen->setOriginMode(false);
            m_screen->setAutoWrap(true);
            m_screen->setCursorVisible(true);
            m_screen->setApplicationCursorKeys(false);
            m_screen->setScrollRegion(0, m_screen->rows() - 1);
        }
        break;
    case 'q':
        break;
    case 'r':
        m_screen->setScrollRegion(param(0, 1) - 1, param(1, m_screen->rows()) - 1);
        break;
    case 's':
        m_screen->saveCursor();
        break;
    case 'u':
        m_screen->restoreCursor();
        break;
    case 't': {
        const int op = param(0, 0);
        if (op == 18)
            respond("\x1b[8;" + QByteArray::number(m_screen->rows()) + ";"
                    + QByteArray::number(m_screen->cols()) + "t");
        break;
    }
    case 'x':
        break;
    default:
        break;
    }
}

void VtParser::setMode(bool on, bool priv)
{
    if (!priv) {
        for (int i = 0; i < paramCount(); ++i) {
            const int mode = param(i, 0);
            if (mode == 4)
                m_screen->setInsertMode(on);
        }
        return;
    }
    for (int i = 0; i < paramCount(); ++i) {
        const int mode = param(i, 0);
        switch (mode) {
        case 1:
            m_screen->setApplicationCursorKeys(on);
            break;
        case 6:
            m_screen->setOriginMode(on);
            break;
        case 7:
            m_screen->setAutoWrap(on);
            break;
        case 12:
        case 1004:
        case 1005:
        case 1015:
        case 1007:
        case 1034:
        case 2026:
        case 3:
        case 5:
        case 9:
            break;
        case 25:
            m_screen->setCursorVisible(on);
            break;
        case 1000:
            m_screen->setMouseMode(on ? VtScreen::MouseMode::Click : VtScreen::MouseMode::Off);
            break;
        case 1002:
            m_screen->setMouseMode(on ? VtScreen::MouseMode::Drag : VtScreen::MouseMode::Off);
            break;
        case 1003:
            m_screen->setMouseMode(on ? VtScreen::MouseMode::Move : VtScreen::MouseMode::Off);
            break;
        case 1006:
            m_screen->setSgrMouse(on);
            break;
        case 47:
        case 1047:
            if (on)
                m_screen->enterAltScreen();
            else
                m_screen->exitAltScreen();
            break;
        case 1049:
            if (on) {
                m_screen->saveCursor();
                m_screen->enterAltScreen();
            } else {
                m_screen->exitAltScreen();
                m_screen->restoreCursor();
            }
            break;
        case 2004:
            m_screen->setBracketedPaste(on);
            break;
        default:
            break;
        }
    }
}

void VtParser::applySgr()
{
    VtPen &pen = m_screen->pen();
    if (paramCount() == 0) {
        pen = VtPen();
        return;
    }

    for (int i = 0; i < paramCount(); ++i) {
        const auto &token = m_tokens[size_t(i)];
        const int code = token.empty() ? 0 : (token[0] < 0 ? 0 : token[0]);
        switch (code) {
        case 0:
            pen = VtPen();
            break;
        case 1:
            pen.attr |= AttrBold;
            break;
        case 2:
            pen.attr |= AttrDim;
            break;
        case 3:
            pen.attr |= AttrItalic;
            break;
        case 4:
            pen.attr |= AttrUnderline;
            break;
        case 5:
        case 6:
            pen.attr |= AttrBlink;
            break;
        case 7:
            pen.attr |= AttrReverse;
            break;
        case 8:
            pen.attr |= AttrHidden;
            break;
        case 9:
            pen.attr |= AttrStrike;
            break;
        case 21:
        case 22:
            pen.attr &= quint16(~(AttrBold | AttrDim));
            break;
        case 23:
            pen.attr &= quint16(~AttrItalic);
            break;
        case 24:
            pen.attr &= quint16(~AttrUnderline);
            break;
        case 25:
            pen.attr &= quint16(~AttrBlink);
            break;
        case 27:
            pen.attr &= quint16(~AttrReverse);
            break;
        case 28:
            pen.attr &= quint16(~AttrHidden);
            break;
        case 29:
            pen.attr &= quint16(~AttrStrike);
            break;
        case 38:
        case 48: {
            const bool isFg = (code == 38);
            VtColor color;
            bool have = false;
            if (token.size() >= 2) {
                const int mode = token[1];
                if (mode == 5 && token.size() >= 3) {
                    color = VtColor::indexed(token[2]);
                    have = true;
                } else if (mode == 2) {
                    if (token.size() >= 6) {
                        color = VtColor::rgb(quint32((token[3] & 0xFF) << 16 | (token[4] & 0xFF) << 8
                                                     | (token[5] & 0xFF)));
                        have = true;
                    } else if (token.size() == 5) {
                        color = VtColor::rgb(quint32((token[2] & 0xFF) << 16 | (token[3] & 0xFF) << 8
                                                     | (token[4] & 0xFF)));
                        have = true;
                    }
                }
            } else if (i + 1 < paramCount()) {
                const int mode = param(i + 1, 0);
                if (mode == 5 && i + 2 < paramCount()) {
                    color = VtColor::indexed(param(i + 2, 0));
                    i += 2;
                    have = true;
                } else if (mode == 2 && i + 4 < paramCount()) {
                    color = VtColor::rgb(quint32((param(i + 2, 0) & 0xFF) << 16
                                                 | (param(i + 3, 0) & 0xFF) << 8
                                                 | (param(i + 4, 0) & 0xFF)));
                    i += 4;
                    have = true;
                }
            }
            if (have) {
                if (isFg)
                    pen.fg = color;
                else
                    pen.bg = color;
            }
            break;
        }
        case 39:
            pen.fg = VtColor();
            break;
        case 49:
            pen.bg = VtColor();
            break;
        default:
            if (code >= 30 && code <= 37)
                pen.fg = VtColor::indexed(code - 30);
            else if (code >= 40 && code <= 47)
                pen.bg = VtColor::indexed(code - 40);
            else if (code >= 90 && code <= 97)
                pen.fg = VtColor::indexed(code - 90 + 8);
            else if (code >= 100 && code <= 107)
                pen.bg = VtColor::indexed(code - 100 + 8);
            break;
        }
    }
}

} // namespace termdock
