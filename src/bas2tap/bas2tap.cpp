// bas2tap.cpp - convert a plain-text BASIC listing into a ZX Spectrum .tap file
//
// Build:  cl /EHsc /std:c++17 /O2 bas2tap.cpp
//    or:  g++ -std=c++17 -O2 bas2tap.cpp -o bas2tap
// Usage:  bas2tap input.bas output.tap [name] [autostart_line]
//
// Input example:
//   10 FOR n=1 TO 10
//   20 PRINT "Hello ";n
//   30 NEXT n
//   40 GOTO 10

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "z80/basic_tokens.h"

using Bytes = std::vector<uint8_t>;

// The keyword table, the token numbering and the hidden-number encoding are
// shared with tap2bas: the two tools are inverses and must agree exactly.
// A space inside a keyword matches zero or more spaces in the source, so
// "GOTO" and "GO TO" both work.
using zxbasic::kKeywords;
using zxbasic::kFirstToken;
using zxbasic::TOK_BIN;
using zxbasic::TOK_REM;

// Try to match a keyword at text[pos]. Returns the token (0 = none) and sets end.
// Longest match wins; a keyword ending in a letter must not be followed by a letter.
static uint8_t matchKeyword(const std::string& s, size_t pos, size_t& end) {
    uint8_t best = 0;
    size_t bestEnd = pos;
    for (size_t k = 0; k < sizeof(kKeywords) / sizeof(kKeywords[0]); ++k) {
        const std::string kw = kKeywords[k];
        size_t i = pos;
        bool ok = true;
        for (char kc : kw) {
            if (kc == ' ') {
                while (i < s.size() && s[i] == ' ') ++i;
            } else if (i < s.size() && std::toupper((unsigned char)s[i]) == kc) {
                ++i;
            } else {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        if (std::isalpha((unsigned char)kw.back()) && i < s.size() &&
            std::isalpha((unsigned char)s[i]))
            continue;  // e.g. "TO" inside "total"
        if (i > bestEnd) {
            best = uint8_t(kFirstToken + k);
            bestEnd = i;
        }
    }
    end = bestEnd;
    return best;
}

// Append the hidden 5-byte number form.
static void encodeNumber(double v, Bytes& out) {
    if (v >= 0 && v <= 65535 && v == std::floor(v)) {  // small-integer form
        uint16_t n = uint16_t(v);
        out.insert(out.end(), {0x00, 0x00, uint8_t(n & 0xFF), uint8_t(n >> 8), 0x00});
        return;
    }
    int e;
    double m = std::frexp(v, &e);  // v = m * 2^e, 0.5 <= m < 1
    unsigned long long mant = (unsigned long long)std::llround(std::ldexp(m, 32));
    if (mant >> 32) { mant >>= 1; ++e; }  // rounding carried past bit 31
    if (e + 128 < 1) { out.insert(out.end(), 5, 0x00); return; }  // underflow -> 0
    if (e + 128 > 255) throw std::runtime_error("number too large");
    out.push_back(uint8_t(e + 128));
    out.push_back(uint8_t((mant >> 24) & 0x7F));  // top bit is the sign: 0 = positive
    out.push_back(uint8_t(mant >> 16));
    out.push_back(uint8_t(mant >> 8));
    out.push_back(uint8_t(mant));
}

// Scan a numeric literal at s[pos]. Sets end and value.
static void scanNumber(const std::string& s, size_t pos, bool binary, size_t& end, double& value) {
    size_t i = pos;
    if (binary) {
        value = 0;
        while (i < s.size() && (s[i] == '0' || s[i] == '1')) value = value * 2 + (s[i++] - '0');
    } else {
        while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {  // exponent only if digits follow
            size_t j = i + 1;
            if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
            if (j < s.size() && std::isdigit((unsigned char)s[j])) {
                i = j;
                while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
            }
        }
        value = std::strtod(s.substr(pos, i - pos).c_str(), nullptr);
    }
    end = i;
}

// Tokenize one line body (everything after the line number).
static Bytes tokenizeLine(const std::string& s) {
    Bytes out;
    size_t i = 0;
    bool afterBin = false;
    while (i < s.size()) {
        unsigned char c = s[i];

        if (c == '"') {  // string literal: copy verbatim, "" is an escaped quote
            out.push_back(c);
            ++i;
            while (i < s.size() && s[i] != '"') out.push_back(s[i++]);
            if (i < s.size()) out.push_back(s[i++]);
            afterBin = false;
            continue;
        }
        if (std::isspace(c)) { ++i; continue; }  // the ROM re-inserts display spaces

        if (std::isalpha(c) || c == '<' || c == '>') {
            size_t end;
            if (uint8_t tok = matchKeyword(s, i, end)) {
                out.push_back(tok);
                i = end;
                afterBin = (tok == TOK_BIN);
                if (tok == TOK_REM) {  // rest of line is a comment, copied as-is
                    if (i < s.size() && s[i] == ' ') ++i;
                    out.insert(out.end(), s.begin() + i, s.end());
                    i = s.size();
                }
                continue;
            }
        }
        if (std::isalpha(c)) {  // variable name: letters/digits, optional '$'
            while (i < s.size() && std::isalnum((unsigned char)s[i])) out.push_back(s[i++]);
            if (i < s.size() && s[i] == '$') out.push_back(s[i++]);
            afterBin = false;
            continue;
        }
        if (std::isdigit(c) || (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1]))) {
            size_t end;
            double value;
            scanNumber(s, i, afterBin, end, value);
            out.insert(out.end(), s.begin() + i, s.begin() + end);  // visible digits
            out.push_back(0x0E);                                   // hidden number marker
            encodeNumber(value, out);
            i = end;
            afterBin = false;
            continue;
        }
        out.push_back(c);  // operators and punctuation: = + - * / ( ) , ; : etc.
        ++i;
        afterBin = false;
    }
    out.push_back(0x0D);
    return out;
}

static void writeBlock(std::ofstream& f, uint8_t flag, const Bytes& data) {
    size_t len = data.size() + 2;
    f.put(char(len & 0xFF));
    f.put(char(len >> 8));
    uint8_t sum = flag;
    f.put(char(flag));
    for (uint8_t b : data) { f.put(char(b)); sum ^= b; }
    f.put(char(sum));
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: bas2tap input.bas output.tap [name] [autostart_line]\n";
        return 1;
    }
    std::string name = argc > 3 ? argv[3] : "program";
    unsigned autostart = argc > 4 ? unsigned(std::atoi(argv[4])) : 32768;  // >=32768: none

    std::ifstream in(argv[1]);
    if (!in) { std::cerr << "cannot open " << argv[1] << "\n"; return 1; }

    struct Line { unsigned number; Bytes body; };
    std::vector<Line> lines;
    std::string text;
    int srcLine = 0;
    try {
        while (std::getline(in, text)) {
            ++srcLine;
            if (!text.empty() && text.back() == '\r') text.pop_back();  // CRLF files
            size_t i = 0;
            while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;
            if (i == text.size()) continue;  // blank line
            if (!std::isdigit((unsigned char)text[i]))
                throw std::runtime_error("missing line number");
            unsigned num = 0;
            while (i < text.size() && std::isdigit((unsigned char)text[i])) num = num * 10 + (text[i++] - '0');
            if (num < 1 || num > 9999) throw std::runtime_error("line number out of range 1-9999");
            // Decode escapes first, so a UTF-8 block element or a \xNN arrives
            // at the tokeniser as the single Spectrum byte it stands for.
            lines.push_back({num, tokenizeLine(zxbasic::decode_escapes(text.substr(i)))});
        }
    } catch (const std::exception& ex) {
        std::cerr << argv[1] << ":" << srcLine << ": " << ex.what() << "\n";
        return 1;
    }

    // The ROM expects ascending order; later duplicates replace earlier ones.
    std::stable_sort(lines.begin(), lines.end(),
                     [](const Line& a, const Line& b) { return a.number < b.number; });
    Bytes program;
    for (size_t k = 0; k < lines.size(); ++k) {
        if (k + 1 < lines.size() && lines[k + 1].number == lines[k].number) continue;
        const Line& l = lines[k];
        program.push_back(uint8_t(l.number >> 8));  // big-endian line number
        program.push_back(uint8_t(l.number & 0xFF));
        program.push_back(uint8_t(l.body.size() & 0xFF));  // little-endian length
        program.push_back(uint8_t(l.body.size() >> 8));
        program.insert(program.end(), l.body.begin(), l.body.end());
    }

    Bytes header(17, 0);
    header[0] = 0;  // type 0 = BASIC program
    for (int k = 0; k < 10; ++k) header[1 + k] = k < int(name.size()) ? uint8_t(name[k]) : ' ';
    auto put16 = [&](int off, unsigned v) { header[off] = uint8_t(v & 0xFF); header[off + 1] = uint8_t(v >> 8); };
    put16(11, unsigned(program.size()));  // data length
    put16(13, autostart);                 // LINE parameter
    put16(15, unsigned(program.size()));  // offset of variables area (none)

    std::ofstream out(argv[2], std::ios::binary);  // binary mode matters on Windows
    if (!out) { std::cerr << "cannot write " << argv[2] << "\n"; return 1; }
    writeBlock(out, 0x00, header);
    writeBlock(out, 0xFF, program);
    std::cout << "wrote " << program.size() << " bytes of BASIC to " << argv[2] << "\n";
    return 0;
}
