// Unit tests for the .tap block layer (milestone 1 of the tape module).
// Pure logic and file I/O -- no CPU involved.

#include "z80/tape.h"
#include "test_harness.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using testing::add_test;
using z80::TapBlock;
using z80::TapFile;
using z80::TapFileType;
using z80::TapHeader;
using z80::TAP_FLAG_DATA;
using z80::TAP_FLAG_HEADER;

namespace {

using Bytes = std::vector<::byte>;

// ---------------------------------------------------------------- helpers --

/// Frames a block the way a .tap file does: 2-byte little-endian length, then
/// flag + payload + checksum. Written out longhand rather than via
/// TapFile::serialise so the tests do not validate the code with itself.
Bytes frame(::byte flag, const Bytes& payload, ::byte checksum) {
    const size_t length = payload.size() + 2;
    Bytes out;
    out.push_back(static_cast<::byte>(length & 0xFF));
    out.push_back(static_cast<::byte>((length >> 8) & 0xFF));
    out.push_back(flag);
    out.insert(out.end(), payload.begin(), payload.end());
    out.push_back(checksum);
    return out;
}

/// Same, but computing the checksum by hand (XOR of flag and payload).
Bytes frame(::byte flag, const Bytes& payload) {
    ::byte sum = flag;
    for (::byte b : payload) sum ^= b;
    return frame(flag, payload, sum);
}

void concat(Bytes& into, const Bytes& more) {
    into.insert(into.end(), more.begin(), more.end());
}

/// A unique scratch path per test, so a failure cannot poison another test.
std::string temp_path(const char* tag) {
    const char* dir = std::getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    static int counter = 0;
    return std::string(dir) + "/tapetest_" + tag + "_"
         + std::to_string(counter++) + ".tap";
}

Bytes read_file(const std::string& path) {
    Bytes out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    ::byte buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    std::fclose(f);
    return out;
}

bool write_file(const std::string& path, const Bytes& data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return true;
}

/// Removes a scratch file; returns `result` so tests can tail-call it.
bool cleanup(const std::string& path, bool result) {
    std::remove(path.c_str());
    return result;
}

/// A realistic header, as `SAVE "TEST" CODE 32768,256` would produce.
TapHeader code_header() {
    TapHeader h;
    h.type        = TapFileType::Code;
    h.name        = "TEST";
    h.data_length = 256;
    h.param1      = 32768;   // start address
    h.param2      = 32768;   // unused for CODE
    return h;
}

} // namespace

void register_tape_tests() {

    // ============================================================ checksum ==

    add_test("tap: checksum is XOR of flag and payload", [] {
        const Bytes payload{0x01, 0x02, 0x04};
        // 0xFF ^ 0x01 ^ 0x02 ^ 0x04 = 0xF8
        return TapBlock::compute_checksum(0xFF, payload.data(), payload.size()) == 0xF8;
    });

    add_test("tap: checksum of empty payload is the flag itself", [] {
        return TapBlock::compute_checksum(0xFF, nullptr, 0) == 0xFF
            && TapBlock::compute_checksum(0x00, nullptr, 0) == 0x00;
    });

    add_test("tap: checksum of a header flag", [] {
        const Bytes payload{0x03, 0x41};
        // 0x00 ^ 0x03 ^ 0x41 = 0x42
        return TapBlock::compute_checksum(0x00, payload.data(), payload.size()) == 0x42;
    });

    add_test("tap: identical bytes cancel out in XOR", [] {
        const Bytes payload{0x5A, 0x5A};
        return TapBlock::compute_checksum(0x00, payload.data(), payload.size()) == 0x00;
    });

    add_test("tap: make() computes the checksum", [] {
        const Bytes payload{0xAA, 0x55};
        TapBlock b = TapBlock::make(TAP_FLAG_DATA, payload);
        return b.flag == 0xFF && b.payload == payload
            && b.checksum == static_cast<::byte>(0xFF ^ 0xAA ^ 0x55)
            && b.checksum_valid();
    });

    add_test("tap: make() with an empty payload", [] {
        TapBlock b = TapBlock::make(TAP_FLAG_DATA, nullptr, 0);
        return b.payload.empty() && b.checksum == 0xFF
            && b.checksum_valid() && b.tap_length() == 2;
    });

    add_test("tap: checksum_valid detects a corrupt checksum byte", [] {
        TapBlock b = TapBlock::make(TAP_FLAG_DATA, Bytes{0x01, 0x02});
        if (!b.checksum_valid()) return false;
        b.checksum ^= 0x01;
        return !b.checksum_valid();
    });

    add_test("tap: checksum_valid detects corrupt payload", [] {
        TapBlock b = TapBlock::make(TAP_FLAG_DATA, Bytes{0x01, 0x02});
        b.payload[0] = 0x99;
        return !b.checksum_valid();
    });

    add_test("tap: tap_length counts flag and checksum", [] {
        return TapBlock::make(TAP_FLAG_DATA, Bytes(100, 0)).tap_length() == 102
            && TapBlock::make(TAP_FLAG_HEADER, Bytes(17, 0)).tap_length() == 19;
    });

    add_test("tap: is_header / is_data", [] {
        TapBlock h = TapBlock::make(TAP_FLAG_HEADER, Bytes(17, 0));
        TapBlock d = TapBlock::make(TAP_FLAG_DATA, Bytes(4, 0));
        return h.is_header() && !h.is_data() && d.is_data() && !d.is_header();
    });

    // =============================================================== parse ==

    add_test("tap: an empty image is a valid blank tape", [] {
        std::vector<TapBlock> blocks;
        std::string err;
        return TapFile::parse(nullptr, 0, blocks, &err) && blocks.empty() && err.empty();
    });

    add_test("tap: parses a single data block", [] {
        const Bytes payload{0x11, 0x22, 0x33};
        Bytes raw = frame(TAP_FLAG_DATA, payload);
        std::vector<TapBlock> blocks;
        if (!TapFile::parse(raw.data(), raw.size(), blocks, nullptr)) return false;
        return blocks.size() == 1 && blocks[0].flag == 0xFF
            && blocks[0].payload == payload && blocks[0].checksum_valid();
    });

    add_test("tap: parses a header + data pair (one saved file)", [] {
        Bytes raw;
        concat(raw, frame(TAP_FLAG_HEADER, Bytes(17, 0x00)));
        concat(raw, frame(TAP_FLAG_DATA, Bytes{0xDE, 0xAD}));
        std::vector<TapBlock> blocks;
        if (!TapFile::parse(raw.data(), raw.size(), blocks, nullptr)) return false;
        return blocks.size() == 2
            && blocks[0].is_header() && blocks[0].payload.size() == 17
            && blocks[1].is_data()   && blocks[1].payload.size() == 2;
    });

    add_test("tap: parses several files in sequence", [] {
        Bytes raw;
        for (int i = 0; i < 3; i++) {
            concat(raw, frame(TAP_FLAG_HEADER, Bytes(17, static_cast<::byte>(i))));
            concat(raw, frame(TAP_FLAG_DATA, Bytes(8, static_cast<::byte>(i))));
        }
        std::vector<TapBlock> blocks;
        if (!TapFile::parse(raw.data(), raw.size(), blocks, nullptr)) return false;
        if (blocks.size() != 6) return false;
        for (int i = 0; i < 3; i++) {
            if (!blocks[i * 2].is_header() || !blocks[i * 2 + 1].is_data()) return false;
            if (blocks[i * 2 + 1].payload[0] != static_cast<::byte>(i)) return false;
        }
        return true;
    });

    add_test("tap: length prefix is little-endian", [] {
        // 300-byte payload -> length 302 = 0x012E, so the bytes are 2E 01.
        Bytes raw = frame(TAP_FLAG_DATA, Bytes(300, 0x00));
        if (raw[0] != 0x2E || raw[1] != 0x01) return false;   // the fixture itself
        std::vector<TapBlock> blocks;
        if (!TapFile::parse(raw.data(), raw.size(), blocks, nullptr)) return false;
        return blocks.size() == 1 && blocks[0].payload.size() == 300;
    });

    add_test("tap: a byte-swapped length is not silently accepted", [] {
        Bytes raw = frame(TAP_FLAG_DATA, Bytes(300, 0x00));
        std::swap(raw[0], raw[1]);            // 0x2E01 = 11777, way past EOF
        std::vector<TapBlock> blocks;
        return !TapFile::parse(raw.data(), raw.size(), blocks, nullptr);
    });

    add_test("tap: minimum legal block (length 2, empty payload)", [] {
        Bytes raw = frame(TAP_FLAG_DATA, Bytes{});
        std::vector<TapBlock> blocks;
        if (!TapFile::parse(raw.data(), raw.size(), blocks, nullptr)) return false;
        return blocks.size() == 1 && blocks[0].payload.empty()
            && blocks[0].checksum == 0xFF;
    });

    add_test("tap: rejects block length 0", [] {
        const Bytes raw{0x00, 0x00};
        std::vector<TapBlock> blocks;
        std::string err;
        return !TapFile::parse(raw.data(), raw.size(), blocks, &err) && !err.empty();
    });

    add_test("tap: rejects block length 1", [] {
        const Bytes raw{0x01, 0x00, 0xFF};
        std::vector<TapBlock> blocks;
        std::string err;
        return !TapFile::parse(raw.data(), raw.size(), blocks, &err) && !err.empty();
    });

    add_test("tap: rejects a truncated length prefix", [] {
        const Bytes raw{0x04};                // one byte where two are needed
        std::vector<TapBlock> blocks;
        std::string err;
        return !TapFile::parse(raw.data(), raw.size(), blocks, &err) && !err.empty();
    });

    add_test("tap: rejects a truncated block body", [] {
        Bytes raw = frame(TAP_FLAG_DATA, Bytes{0x01, 0x02, 0x03});
        raw.pop_back();                       // lose the checksum
        std::vector<TapBlock> blocks;
        std::string err;
        return !TapFile::parse(raw.data(), raw.size(), blocks, &err) && !err.empty();
    });

    add_test("tap: rejects trailing garbage after the last block", [] {
        Bytes raw = frame(TAP_FLAG_DATA, Bytes{0x01});
        raw.push_back(0x05);                  // a stray half length prefix
        std::vector<TapBlock> blocks;
        return !TapFile::parse(raw.data(), raw.size(), blocks, nullptr);
    });

    add_test("tap: blocks before an error are still returned", [] {
        Bytes raw;
        concat(raw, frame(TAP_FLAG_HEADER, Bytes(17, 0x00)));
        concat(raw, frame(TAP_FLAG_DATA, Bytes{0x01, 0x02}));
        raw.pop_back();                       // damage the final block
        std::vector<TapBlock> blocks;
        // Reports failure, but the intact leading block is usable.
        return !TapFile::parse(raw.data(), raw.size(), blocks, nullptr)
            && blocks.size() == 1 && blocks[0].is_header();
    });

    add_test("tap: parse clears any previous contents", [] {
        std::vector<TapBlock> blocks;
        blocks.push_back(TapBlock::make(TAP_FLAG_DATA, Bytes(3, 0)));
        Bytes raw = frame(TAP_FLAG_HEADER, Bytes(17, 0));
        TapFile::parse(raw.data(), raw.size(), blocks, nullptr);
        return blocks.size() == 1 && blocks[0].is_header();
    });

    add_test("tap: a bad checksum parses but reports invalid", [] {
        // A corrupt tape must load so it can be inspected -- not be rejected.
        Bytes raw = frame(TAP_FLAG_DATA, Bytes{0x01, 0x02}, 0x00 /* wrong */);
        std::vector<TapBlock> blocks;
        return TapFile::parse(raw.data(), raw.size(), blocks, nullptr)
            && blocks.size() == 1 && !blocks[0].checksum_valid();
    });

    add_test("tap: parses a maximum-size block (65533-byte payload)", [] {
        Bytes raw = frame(TAP_FLAG_DATA, Bytes(z80::TAP_MAX_PAYLOAD, 0x00));
        if (raw[0] != 0xFF || raw[1] != 0xFF) return false;   // length == 0xFFFF
        std::vector<TapBlock> blocks;
        return TapFile::parse(raw.data(), raw.size(), blocks, nullptr)
            && blocks.size() == 1
            && blocks[0].payload.size() == z80::TAP_MAX_PAYLOAD;
    });

    // =========================================================== serialise ==

    add_test("tap: serialise is byte-exact against a hand-built image", [] {
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, Bytes{0x11, 0x22}));
        // length 4 -> 04 00, then FF 11 22, checksum FF^11^22 = CC
        const Bytes expected{0x04, 0x00, 0xFF, 0x11, 0x22, 0xCC};
        return tape.serialise() == expected;
    });

    add_test("tap: an empty tape serialises to zero bytes", [] {
        return TapFile().serialise().empty();
    });

    add_test("tap: serialise round-trips through parse", [] {
        TapFile tape;
        tape.append(code_header().encode());
        tape.append(TapBlock::make(TAP_FLAG_DATA, Bytes(256, 0x5A)));
        tape.append(TapBlock::make(TAP_FLAG_DATA, Bytes{}));

        const Bytes raw = tape.serialise();
        std::vector<TapBlock> back;
        if (!TapFile::parse(raw.data(), raw.size(), back, nullptr)) return false;
        return back == tape.blocks();
    });

    add_test("tap: serialise preserves a deliberately bad checksum", [] {
        TapBlock b = TapBlock::make(TAP_FLAG_DATA, Bytes{0x01});
        b.checksum = 0x00;                    // not what compute would give
        TapFile tape;
        tape.append(b);
        const Bytes raw = tape.serialise();
        return raw.back() == 0x00;
    });

    // ============================================================ validate ==

    add_test("tap: validate accepts a normal tape", [] {
        TapFile tape;
        tape.append(code_header().encode());
        tape.append(TapBlock::make(TAP_FLAG_DATA, Bytes(1024, 0)));
        std::string err;
        return tape.validate(&err) && err.empty();
    });

    add_test("tap: validate accepts a payload of exactly the maximum", [] {
        TapFile tape;
        TapBlock b;
        b.flag = TAP_FLAG_DATA;
        b.payload.resize(z80::TAP_MAX_PAYLOAD);
        return tape.blocks().empty() && (tape.append(b), tape.validate(nullptr));
    });

    add_test("tap: validate rejects a payload one byte too large", [] {
        TapFile tape;
        TapBlock b;
        b.flag = TAP_FLAG_DATA;
        b.payload.resize(z80::TAP_MAX_PAYLOAD + 1);
        tape.append(b);
        std::string err;
        return !tape.validate(&err) && !err.empty();
    });

    // ========================================================== TapHeader ==

    add_test("tap: header encodes to a 19-byte block with the header flag", [] {
        TapBlock b = code_header().encode();
        return b.flag == TAP_FLAG_HEADER
            && b.payload.size() == z80::TAP_HEADER_PAYLOAD_SIZE
            && b.tap_length() == 19 && b.checksum_valid();
    });

    add_test("tap: header field layout is exactly as the ROM expects", [] {
        TapBlock b = code_header().encode();
        const ::byte* p = b.payload.data();
        return p[0] == 3                                  // type = Code
            && p[1] == 'T' && p[2] == 'E' && p[3] == 'S' && p[4] == 'T'
            && p[5] == ' ' && p[10] == ' '                // padded to 10
            && p[11] == 0x00 && p[12] == 0x01             // data_length 256
            && p[13] == 0x00 && p[14] == 0x80             // param1 32768
            && p[15] == 0x00 && p[16] == 0x80;            // param2 32768
    });

    add_test("tap: header round-trips", [] {
        const TapHeader original = code_header();
        TapHeader decoded;
        return TapHeader::decode(original.encode(), decoded) && decoded == original;
    });

    add_test("tap: all four file types round-trip", [] {
        for (::byte t = 0; t <= 3; t++) {
            TapHeader h;
            h.type = static_cast<TapFileType>(t);
            h.name = "X";
            TapHeader back;
            if (!TapHeader::decode(h.encode(), back)) return false;
            if (back.type != h.type) return false;
        }
        return true;
    });

    add_test("tap: a Program header keeps its autostart line", [] {
        TapHeader h;
        h.type = TapFileType::Program;
        h.name = "prog";
        h.data_length = 500;
        h.param1 = 10;        // autostart at line 10
        h.param2 = 400;       // variables begin 400 bytes in
        TapHeader back;
        return TapHeader::decode(h.encode(), back) && back == h;
    });

    add_test("tap: 'no autostart' (param1 >= 0x8000) survives", [] {
        TapHeader h;
        h.type = TapFileType::Program;
        h.param1 = 0x8000;
        TapHeader back;
        return TapHeader::decode(h.encode(), back) && back.param1 == 0x8000;
    });

    add_test("tap: an empty name pads to 10 spaces", [] {
        TapHeader h;
        h.name = "";
        TapBlock b = h.encode();
        for (size_t i = 0; i < z80::TAP_NAME_SIZE; i++) {
            if (b.payload[1 + i] != ' ') return false;
        }
        TapHeader back;
        return TapHeader::decode(b, back) && back.name.empty();
    });

    add_test("tap: a full 10-character name round-trips", [] {
        TapHeader h;
        h.name = "0123456789";
        TapHeader back;
        return TapHeader::decode(h.encode(), back) && back.name == "0123456789";
    });

    add_test("tap: a name longer than 10 is truncated, as SAVE does", [] {
        TapHeader h;
        h.name = "ABCDEFGHIJKLMNOP";
        TapBlock b = h.encode();
        if (b.payload.size() != z80::TAP_HEADER_PAYLOAD_SIZE) return false;
        TapHeader back;
        return TapHeader::decode(b, back) && back.name == "ABCDEFGHIJ";
    });

    add_test("tap: trailing spaces in a name are lost (indistinguishable from padding)", [] {
        TapHeader h;
        h.name = "AB  ";
        TapHeader back;
        // Documented lossy edge: the tape cannot tell padding from content.
        return TapHeader::decode(h.encode(), back) && back.name == "AB";
    });

    add_test("tap: interior spaces in a name are kept", [] {
        TapHeader h;
        h.name = "A B";
        TapHeader back;
        return TapHeader::decode(h.encode(), back) && back.name == "A B";
    });

    add_test("tap: high-byte name characters (tokens/graphics) survive", [] {
        TapHeader h;
        h.name = std::string("\xE6\x80\xFF", 3);   // NEW token, graphics, COPY
        TapHeader back;
        if (!TapHeader::decode(h.encode(), back)) return false;
        return back.name.size() == 3
            && static_cast<::byte>(back.name[0]) == 0xE6
            && static_cast<::byte>(back.name[1]) == 0x80
            && static_cast<::byte>(back.name[2]) == 0xFF;
    });

    add_test("tap: a name containing NUL is not truncated at the NUL", [] {
        TapHeader h;
        h.name = std::string("A\0B", 3);
        TapHeader back;
        return TapHeader::decode(h.encode(), back) && back.name.size() == 3
            && back.name[1] == '\0' && back.name[2] == 'B';
    });

    add_test("tap: 16-bit header fields are little-endian", [] {
        TapHeader h;
        h.data_length = 0x1234;
        h.param1      = 0xABCD;
        h.param2      = 0x5678;
        TapBlock b = h.encode();
        const ::byte* p = b.payload.data();
        return p[11] == 0x34 && p[12] == 0x12
            && p[13] == 0xCD && p[14] == 0xAB
            && p[15] == 0x78 && p[16] == 0x56;
    });

    add_test("tap: header fields at their extremes round-trip", [] {
        TapHeader h;
        h.type = TapFileType::CharArray;
        h.data_length = 0xFFFF;
        h.param1 = 0xFFFF;
        h.param2 = 0x0000;
        TapHeader back;
        return TapHeader::decode(h.encode(), back) && back == h;
    });

    add_test("tap: decode rejects a data block", [] {
        TapHeader out;
        return !TapHeader::decode(TapBlock::make(TAP_FLAG_DATA, Bytes(17, 0)), out);
    });

    add_test("tap: decode rejects a payload that is not 17 bytes", [] {
        TapHeader out;
        return !TapHeader::decode(TapBlock::make(TAP_FLAG_HEADER, Bytes(16, 0)), out)
            && !TapHeader::decode(TapBlock::make(TAP_FLAG_HEADER, Bytes(18, 0)), out)
            && !TapHeader::decode(TapBlock::make(TAP_FLAG_HEADER, Bytes{}), out);
    });

    add_test("tap: decode rejects an unknown file type", [] {
        Bytes payload(17, 0);
        payload[0] = 4;                       // only 0-3 exist
        TapHeader out;
        return !TapHeader::decode(TapBlock::make(TAP_FLAG_HEADER, payload), out);
    });

    add_test("tap: decode leaves the output untouched on failure", [] {
        TapHeader out = code_header();
        const TapHeader before = out;
        TapHeader::decode(TapBlock::make(TAP_FLAG_DATA, Bytes(17, 0)), out);
        return out == before;
    });

    // =========================================================== file I/O ==

    add_test("tap: save then load round-trips through the filesystem", [] {
        const std::string path = temp_path("roundtrip");
        TapFile out;
        out.append(code_header().encode());
        out.append(TapBlock::make(TAP_FLAG_DATA, Bytes(256, 0xA5)));
        if (!out.save(path)) return cleanup(path, false);

        TapFile in;
        if (!in.load(path)) return cleanup(path, false);
        return cleanup(path, in.blocks() == out.blocks() && in.last_error().empty());
    });

    add_test("tap: the saved file is byte-identical to serialise()", [] {
        const std::string path = temp_path("bytes");
        TapFile tape;
        tape.append(code_header().encode());
        tape.append(TapBlock::make(TAP_FLAG_DATA, Bytes{1, 2, 3}));
        if (!tape.save(path)) return cleanup(path, false);
        return cleanup(path, read_file(path) == tape.serialise());
    });

    add_test("tap: saving an empty tape produces a 0-byte file", [] {
        const std::string path = temp_path("blank");
        TapFile tape;
        if (!tape.save(path)) return cleanup(path, false);
        return cleanup(path, read_file(path).empty());
    });

    add_test("tap: loading a 0-byte file gives a valid blank tape", [] {
        const std::string path = temp_path("empty");
        if (!write_file(path, Bytes{})) return cleanup(path, false);
        TapFile tape;
        return cleanup(path, tape.load(path) && tape.empty()
                             && tape.last_error().empty());
    });

    add_test("tap: save truncates an existing longer file", [] {
        const std::string path = temp_path("trunc");
        if (!write_file(path, Bytes(5000, 0xEE))) return cleanup(path, false);
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, Bytes{0x01}));
        if (!tape.save(path)) return cleanup(path, false);
        return cleanup(path, read_file(path).size() == 5);  // 2 + 1 + 1 + 1
    });

    add_test("tap: load of a missing file fails with an error message", [] {
        TapFile tape;
        const bool ok = tape.load("/nonexistent-dir-xyz/nope.tap");
        return !ok && !tape.last_error().empty();
    });

    add_test("tap: load replaces previous contents", [] {
        const std::string path = temp_path("replace");
        TapFile first;
        first.append(TapBlock::make(TAP_FLAG_DATA, Bytes(3, 0x01)));
        if (!first.save(path)) return cleanup(path, false);

        TapFile tape;
        tape.append(code_header().encode());
        tape.append(TapBlock::make(TAP_FLAG_DATA, Bytes(99, 0)));
        if (!tape.load(path)) return cleanup(path, false);
        return cleanup(path, tape.size() == 1 && tape.blocks()[0].payload.size() == 3);
    });

    add_test("tap: load of a corrupt file reports the error and keeps good blocks", [] {
        const std::string path = temp_path("corrupt");
        Bytes raw;
        concat(raw, frame(TAP_FLAG_HEADER, Bytes(17, 0x00)));
        concat(raw, frame(TAP_FLAG_DATA, Bytes{0x01, 0x02}));
        raw.pop_back();
        if (!write_file(path, raw)) return cleanup(path, false);

        TapFile tape;
        const bool ok = tape.load(path);
        return cleanup(path, !ok && !tape.last_error().empty()
                             && tape.size() == 1 && tape.blocks()[0].is_header());
    });

    add_test("tap: save to an unwritable path fails with an error message", [] {
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, Bytes{0x01}));
        const bool ok = tape.save("/nonexistent-dir-xyz/nope.tap");
        return !ok && !tape.last_error().empty();
    });

    add_test("tap: save refuses an oversize block rather than writing garbage", [] {
        const std::string path = temp_path("oversize");
        TapFile tape;
        TapBlock b;
        b.flag = TAP_FLAG_DATA;
        b.payload.resize(z80::TAP_MAX_PAYLOAD + 1);
        tape.append(b);
        const bool ok = tape.save(path);
        return cleanup(path, !ok && !tape.last_error().empty());
    });

    add_test("tap: last_error is cleared by a subsequent success", [] {
        const std::string path = temp_path("clearerr");
        TapFile tape;
        tape.load("/nonexistent-dir-xyz/nope.tap");
        if (tape.last_error().empty()) return false;
        tape.append(TapBlock::make(TAP_FLAG_DATA, Bytes{0x01}));
        if (!tape.save(path)) return cleanup(path, false);
        return cleanup(path, tape.last_error().empty());
    });

    // ========================================================= whole tape ==

    add_test("tap: a three-file tape round-trips with all fields intact", [] {
        const std::string path = temp_path("multi");

        struct Spec { TapFileType type; const char* name; word length; word p1; };
        const Spec specs[] = {
            { TapFileType::Program, "loader",  120, 10     },
            { TapFileType::Code,    "screen", 6912, 16384  },
            { TapFileType::Code,    "main",   1024, 32768  },
        };

        TapFile out;
        for (const Spec& s : specs) {
            TapHeader h;
            h.type = s.type; h.name = s.name;
            h.data_length = s.length; h.param1 = s.p1; h.param2 = 0x8000;
            out.append(h.encode());
            out.append(TapBlock::make(TAP_FLAG_DATA,
                                      Bytes(s.length, static_cast<::byte>(s.p1 & 0xFF))));
        }
        if (!out.save(path)) return cleanup(path, false);

        TapFile in;
        if (!in.load(path)) return cleanup(path, false);
        if (in.size() != 6) return cleanup(path, false);

        for (size_t i = 0; i < 3; i++) {
            TapHeader h;
            if (!TapHeader::decode(in.blocks()[i * 2], h)) return cleanup(path, false);
            if (h.type != specs[i].type)              return cleanup(path, false);
            if (h.name != specs[i].name)              return cleanup(path, false);
            if (h.data_length != specs[i].length)     return cleanup(path, false);
            if (h.param1 != specs[i].p1)              return cleanup(path, false);

            const TapBlock& data = in.blocks()[i * 2 + 1];
            if (!data.is_data())                       return cleanup(path, false);
            if (data.payload.size() != specs[i].length) return cleanup(path, false);
            if (!data.checksum_valid())                return cleanup(path, false);
            // The header must agree with the data block that follows it.
            if (h.data_length != data.payload.size())  return cleanup(path, false);
        }
        return cleanup(path, true);
    });

    add_test("tap: every block of a round-tripped tape passes its checksum", [] {
        TapFile tape;
        for (int i = 0; i < 20; i++) {
            Bytes payload(i * 37 + 1);
            for (size_t j = 0; j < payload.size(); j++) {
                payload[j] = static_cast<::byte>(j * 7 + i);
            }
            tape.append(TapBlock::make(TAP_FLAG_DATA, payload));
        }
        const Bytes raw = tape.serialise();
        std::vector<TapBlock> back;
        if (!TapFile::parse(raw.data(), raw.size(), back, nullptr)) return false;
        if (back.size() != 20) return false;
        for (const TapBlock& b : back) {
            if (!b.checksum_valid()) return false;
        }
        return true;
    });
}
