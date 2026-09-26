#pragma once

// Spectrum BASIC tokenisation, shared by bas2tap and tap2bas.
//
// This is a BASIC-level concern rather than a CPU one, so it lives in its own
// namespace -- but it is shared deliberately: the two tools are inverses of
// each other, and if their token tables ever drifted apart a listing would
// stop surviving a round trip in ways that are tedious to track down.

#include <cstdint>
#include <string>

namespace zxbasic {

/// Keyword tokens 0xA5..0xFF, in the order the ROM stores them. Verified
/// against the 48K ROM's own token table at 0x0095, where each word's final
/// character carries bit 7.
inline const char* const kKeywords[] = {
    "RND", "INKEY$", "PI", "FN", "POINT", "SCREEN$", "ATTR", "AT", "TAB", "VAL$",
    "CODE", "VAL", "LEN", "SIN", "COS", "TAN", "ASN", "ACS", "ATN", "LN",
    "EXP", "INT", "SQR", "SGN", "ABS", "PEEK", "IN", "USR", "STR$", "CHR$",
    "NOT", "BIN", "OR", "AND", "<=", ">=", "<>", "LINE", "THEN", "TO",
    "STEP", "DEF FN", "CAT", "FORMAT", "MOVE", "ERASE", "OPEN #", "CLOSE #", "MERGE", "VERIFY",
    "BEEP", "CIRCLE", "INK", "PAPER", "FLASH", "BRIGHT", "INVERSE", "OVER", "OUT", "LPRINT",
    "LLIST", "STOP", "READ", "DATA", "RESTORE", "NEW", "BORDER", "CONTINUE", "DIM", "REM",
    "FOR", "GO TO", "GO SUB", "INPUT", "LOAD", "LIST", "LET", "PAUSE", "NEXT", "POKE",
    "PRINT", "PLOT", "RUN", "SAVE", "RANDOMIZE", "IF", "CLS", "DRAW", "CLEAR", "RETURN",
    "COPY"};

inline constexpr uint8_t kFirstToken = 0xA5;
inline constexpr uint8_t kLastToken  = 0xFF;
inline constexpr int     kTokenCount = kLastToken - kFirstToken + 1;

static_assert(sizeof(kKeywords) / sizeof(kKeywords[0]) == kTokenCount,
              "the keyword table must cover exactly tokens 0xA5..0xFF");

inline constexpr uint8_t TOK_BIN = 0xC4;
inline constexpr uint8_t TOK_REM = 0xEA;

/// A numeric literal is stored twice: the digits as typed, then this marker
/// and a 5-byte binary form the editor keeps hidden. Listing shows only the
/// digits, so detokenising skips the marker and the six bytes after it.
inline constexpr uint8_t kNumberMarker   = 0x0E;
inline constexpr int     kNumberFormSize = 5;

/// Terminates every BASIC line.
inline constexpr uint8_t kLineEnd = 0x0D;

/// A stored program line: line number big-endian, body length little-endian,
/// then the body, which ends with kLineEnd.
inline constexpr int kLineHeaderSize = 4;

// ---- how non-ASCII bytes are written in a .bas file ----------------------
//
// The Spectrum's character set runs past ASCII: block graphics at 0x80-0x8F,
// user-defined graphics at 0x90-0xA4, and colour controls embedded in strings.
// Written raw, those bytes are not valid text -- an editor shows them as "?".
// So a .bas file uses:
//
//   0x81-0x8F   the Unicode block element that looks the same, e.g. "█"
//   \xNN        any other byte outside printable ASCII (UDGs, controls, 0x80)
//   \\          a literal backslash
//
// The block graphics were measured from the emulator rather than taken from a
// reference: code 0x80+n lights quadrants by the bits of n -- 1 top-right,
// 2 top-left, 4 bottom-right, 8 bottom-left.

inline constexpr uint8_t kFirstBlockGraphic = 0x80;
inline constexpr uint8_t kLastBlockGraphic  = 0x8F;

/// Indexed by code - 0x80. Entry 0, the blank graphic, is deliberately empty:
/// it would render as a space and could no longer be told apart from one, so
/// it is written as \x80 instead.
inline const char* const kBlockGraphics[] = {
    nullptr,     // 0x80 blank
    "▝",    // 0x81 upper right
    "▘",    // 0x82 upper left
    "▀",    // 0x83 upper half
    "▗",    // 0x84 lower right
    "▐",    // 0x85 right half
    "▚",    // 0x86 upper left + lower right
    "▜",    // 0x87 all but lower left
    "▖",    // 0x88 lower left
    "▞",    // 0x89 upper right + lower left
    "▌",    // 0x8A left half
    "▛",    // 0x8B all but lower right
    "▄",    // 0x8C lower half
    "▟",    // 0x8D all but upper left
    "▙",    // 0x8E all but upper right
    "█",    // 0x8F full block
};

static_assert(sizeof(kBlockGraphics) / sizeof(kBlockGraphics[0]) ==
              kLastBlockGraphic - kFirstBlockGraphic + 1,
              "one entry per block graphic");

/// Appends one Spectrum byte to .bas source text, escaping as described above.
inline void append_escaped(std::string& out, uint8_t b) {
    if (b == '\\') { out += "\\\\"; return; }
    if (b >= 0x20 && b <= 0x7E) { out += static_cast<char>(b); return; }
    if (b > kFirstBlockGraphic && b <= kLastBlockGraphic) {
        out += kBlockGraphics[b - kFirstBlockGraphic];
        return;
    }
    static const char hex[] = "0123456789ABCDEF";
    out += "\\x";
    out += hex[b >> 4];
    out += hex[b & 0x0F];
}

namespace detail {
inline int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/// If s[i] starts one of the block-element characters, returns its Spectrum
/// code and sets `len` to the UTF-8 length; otherwise returns 0.
inline uint8_t match_block_graphic(const std::string& s, size_t i, size_t& len) {
    for (int n = 1; n <= kLastBlockGraphic - kFirstBlockGraphic; n++) {
        const std::string glyph = kBlockGraphics[n];
        if (s.compare(i, glyph.size(), glyph) == 0) {
            len = glyph.size();
            return static_cast<uint8_t>(kFirstBlockGraphic + n);
        }
    }
    return 0;
}
} // namespace detail

/// Turns .bas source text back into Spectrum bytes. A backslash that does not
/// begin a recognised escape is kept as typed, so a stray one is harmless.
inline std::string decode_escapes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i + 1] == '\\') { out += '\\'; i += 2; continue; }
            if ((s[i + 1] == 'x' || s[i + 1] == 'X') && i + 3 < s.size()) {
                const int hi = detail::hex_value(s[i + 2]);
                const int lo = detail::hex_value(s[i + 3]);
                if (hi >= 0 && lo >= 0) {
                    out += static_cast<char>(hi * 16 + lo);
                    i += 4;
                    continue;
                }
            }
        }
        size_t len = 0;
        if (const uint8_t code = detail::match_block_graphic(s, i, len)) {
            out += static_cast<char>(code);
            i += len;
            continue;
        }
        out += s[i];
        i++;
    }
    return out;
}

} // namespace zxbasic
