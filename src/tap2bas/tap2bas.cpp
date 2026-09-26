// tap2bas.cpp - convert a ZX Spectrum .tap file back into plain-text BASIC
//
// The inverse of bas2tap: reads a tape image and writes one .bas file per
// BASIC program on it, so a tape holding three programs yields three files.
//
// Build:  part of the CMake project; links z80-emulator for .tap parsing.
// Usage:  tap2bas input.tap [output_dir]
//
// Files are named after the name each program was SAVEd under, so a tape
// saved as SAVE "loader" produces loader.bas.
//
// Characters beyond ASCII are escaped so the output is valid text: block
// graphics become the Unicode block element they look like ("█"), and
// anything else becomes \xNN. bas2tap reads both back. See basic_tokens.h.

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "z80/basic_tokens.h"
#include "z80/tape.h"

using z80::TapBlock;
using z80::TapFile;
using z80::TapFileType;
using z80::TapHeader;

namespace {

struct Line {
    unsigned number = 0;
    std::string text;
};

struct Program {
    std::vector<Line> lines;
    /// Bytes written as \xNN escapes -- UDGs and embedded colour controls,
    /// which have no printable form. Block graphics are not counted: they are
    /// written as the matching Unicode block element and read naturally.
    int escaped = 0;
};

/// True when a keyword that ended in a letter has to be kept clear of what
/// follows. Required before a letter or digit -- bas2tap's matcher rejects a
/// keyword that runs straight into one, so "PRINTx" would not tokenise -- and
/// kept before '"' and '(' because that is how a real listing reads.
bool needs_separator_before(uint8_t c) {
    return std::isalnum(c) || c == '$' || c == '"' || c == '(' || c == '.';
}

/// True when a keyword starting with a letter would glue onto what precedes
/// it. "aTO" would be read back as one variable name, so the space is load
/// bearing; after ':', '"' and ')' it is only for readability.
bool needs_separator_after(char last) {
    return std::isalnum(static_cast<unsigned char>(last)) ||
           last == '$' || last == ':' || last == '"' || last == ')';
}

/// Turns one stored line body into its listing text.
///
/// Separators after a keyword are emitted lazily -- only once the next byte
/// shows one is wanted. Appending them eagerly and trimming the line's tail
/// afterwards looks equivalent but is not: a REM comment ending in a space
/// holds that space as program data, and trimming would silently shorten it.
std::string detokenise_line(const uint8_t* body, size_t length, int& escaped) {
    std::string out;
    bool in_string = false;
    bool pending_separator = false;   // a keyword ended in a letter

    auto flush_separator = [&](uint8_t next) {
        if (!pending_separator) return;
        if (needs_separator_before(next)) out += ' ';
        pending_separator = false;
    };

    for (size_t i = 0; i < length;) {
        const uint8_t b = body[i];

        if (b == zxbasic::kLineEnd) break;

        if (b == zxbasic::kNumberMarker) {
            // The visible digits are already out; skip the hidden binary form.
            i += 1 + zxbasic::kNumberFormSize;
            continue;
        }

        if (b == '"') {
            flush_separator('"');
            in_string = !in_string;
            out += '"';
            i++;
            continue;
        }

        // Inside a string every byte is data, including values that would be
        // keywords outside one, so it is copied through untouched.
        if (!in_string && b >= zxbasic::kFirstToken) {
            const char* kw = zxbasic::kKeywords[b - zxbasic::kFirstToken];
            if (pending_separator) {
                out += ' ';
                pending_separator = false;
            } else if (!out.empty() &&
                       std::isalpha(static_cast<unsigned char>(kw[0])) &&
                       needs_separator_after(out.back())) {
                out += ' ';
            }
            out += kw;
            const size_t n = std::strlen(kw);
            pending_separator = std::isalpha(static_cast<unsigned char>(kw[n - 1]));
            i++;
            continue;
        }

        flush_separator(b);
        if ((b < 0x20 || b > 0x7E) &&
            !(b > zxbasic::kFirstBlockGraphic && b <= zxbasic::kLastBlockGraphic)) {
            escaped++;
        }
        zxbasic::append_escaped(out, b);
        i++;
    }

    return out;
}

/// Splits a stored program into lines. Returns false on a malformed one,
/// leaving whatever was recovered in `prog` so a damaged tape still yields
/// the part that reads cleanly.
bool detokenise_program(const std::vector<uint8_t>& data, size_t program_bytes,
                        Program& prog, std::string& error) {
    size_t i = 0;
    while (i < program_bytes) {
        if (program_bytes - i < size_t(zxbasic::kLineHeaderSize)) {
            error = "truncated line header after line " +
                    std::to_string(prog.lines.empty() ? 0 : prog.lines.back().number);
            return false;
        }

        // Line numbers are big-endian -- the one field on a Spectrum tape that
        // is -- while the length beside them is little-endian.
        const unsigned number = (unsigned(data[i]) << 8) | data[i + 1];
        const size_t body_len = size_t(data[i + 2]) | (size_t(data[i + 3]) << 8);
        i += zxbasic::kLineHeaderSize;

        if (i + body_len > program_bytes) {
            error = "line " + std::to_string(number) + " runs past the end of the program";
            return false;
        }

        Line line;
        line.number = number;
        line.text = detokenise_line(&data[i], body_len, prog.escaped);
        prog.lines.push_back(std::move(line));
        i += body_len;
    }
    return true;
}

/// Turns a tape name into something safe to open on a filesystem.
std::string safe_filename(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') out += char(c);
        else if (c == ' ') out += '_';
        // Anything else -- graphics characters, punctuation -- is dropped.
    }
    while (!out.empty() && (out.front() == '.' || out.front() == '_')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '.' || out.back() == '_')) out.pop_back();
    return out.empty() ? std::string("program") : out;
}

std::string join_path(const std::string& dir, const std::string& file) {
    if (dir.empty() || dir == ".") return file;
    if (dir.back() == '/') return dir + file;
    return dir + "/" + file;
}

/// Quotes a name for the suggested bas2tap command line.
std::string shell_quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: tap2bas input.tap [output_dir]\n"
                     "       writes one .bas file per BASIC program on the tape\n";
        return 1;
    }
    const std::string tap_path = argv[1];
    const std::string out_dir = argc > 2 ? argv[2] : ".";

    TapFile tape;
    if (!tape.load(tap_path)) {
        // A partially readable tape still yields its intact leading blocks, so
        // report the damage and carry on with whatever parsed.
        std::cerr << "warning: " << tape.last_error() << "\n";
        if (tape.empty()) return 1;
    }

    int written = 0;
    int skipped = 0;
    std::set<std::string> used;

    for (size_t i = 0; i < tape.size(); i++) {
        const TapBlock& block = tape.blocks()[i];
        if (!block.is_header()) continue;   // a stray data block; its header covers it

        TapHeader header;
        if (!TapHeader::decode(block, header)) {
            std::cerr << "warning: block " << i << " is not a readable header\n";
            continue;
        }

        if (header.type != TapFileType::Program) {
            const char* kind = header.type == TapFileType::Code ? "CODE"
                             : header.type == TapFileType::NumberArray ? "number array"
                             : "character array";
            std::cout << "skipping '" << header.name << "' (" << kind
                      << ", " << header.data_length << " bytes) -- not BASIC\n";
            skipped++;
            continue;
        }

        if (i + 1 >= tape.size() || !tape.blocks()[i + 1].is_data()) {
            std::cerr << "warning: '" << header.name << "' has no data block\n";
            continue;
        }
        const TapBlock& data = tape.blocks()[i + 1];
        if (!data.checksum_valid()) {
            std::cerr << "warning: '" << header.name
                      << "' has a bad checksum; converting anyway\n";
        }

        // param2 is where the variables area starts, so it marks the end of the
        // program proper. A SAVE that captured variables has data beyond it.
        size_t program_bytes = data.payload.size();
        if (header.param2 > 0 && header.param2 < program_bytes) {
            program_bytes = header.param2;
        }

        Program prog;
        std::string error;
        if (!detokenise_program(data.payload, program_bytes, prog, error)) {
            std::cerr << "warning: '" << header.name << "': " << error
                      << " (keeping " << prog.lines.size() << " line(s))\n";
        }
        if (prog.lines.empty()) {
            std::cerr << "warning: '" << header.name << "' holds no BASIC lines\n";
            continue;
        }

        std::string base = safe_filename(header.name);
        std::string candidate = base;
        for (int n = 2; used.count(candidate); n++) {
            candidate = base + "_" + std::to_string(n);
        }
        used.insert(candidate);

        const std::string path = join_path(out_dir, candidate + ".bas");
        std::ofstream out(path);
        if (!out) {
            std::cerr << "error: cannot write " << path << "\n";
            return 1;
        }
        for (const Line& line : prog.lines) {
            out << line.number << ' ' << line.text << '\n';
        }
        out.flush();
        if (!out) {
            std::cerr << "error: failed writing " << path << "\n";
            return 1;
        }

        std::cout << "wrote " << path << ": " << prog.lines.size() << " line(s), "
                  << program_bytes << " bytes of BASIC\n";
        if (prog.escaped > 0) {
            std::cout << "  note: " << prog.escaped
                      << " byte(s) written as \\xNN escapes (UDGs or embedded"
                         " colour controls)\n";
        }
        if (header.param1 < 32768) {
            std::cout << "  autostarts at line " << header.param1 << "\n";
        }
        std::cout << "  rebuild with: bas2tap " << path << " out.tap "
                  << shell_quote(header.name) << ' ' << header.param1 << "\n";
        written++;
    }

    if (written == 0) {
        std::cerr << "no BASIC programs found in " << tap_path;
        if (skipped > 0) std::cerr << " (" << skipped << " non-BASIC file(s) skipped)";
        std::cerr << "\n";
        return 1;
    }
    return 0;
}
