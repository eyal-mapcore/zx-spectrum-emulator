// Tests for TapeDeck and the LD-BYTES trap (milestone 3 of the tape module).

#include "z80/tape_trap.h"
#include "test_harness.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using testing::add_test;
using z80::Cpu;
using z80::Memory;
using z80::RegisterState;
using z80::TapBlock;
using z80::TapeDeck;
using z80::TapeTrap;
using z80::TapFile;
using z80::TapFileType;
using z80::TapHeader;
using z80::TAP_FLAG_DATA;
using z80::TAP_FLAG_HEADER;

namespace {

using Bytes = std::vector<::byte>;
using Result = TapeDeck::LoadResult;

constexpr ::byte FLAG_CARRY = 0x01;

std::string temp_path(const char* tag) {
    const char* dir = std::getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    static int counter = 0;
    return std::string(dir) + "/tapedeck_" + tag + "_"
         + std::to_string(counter++) + ".tap";
}

bool cleanup(const std::string& path, bool result) {
    std::remove(path.c_str());
    return result;
}

/// A header + data pair, as `SAVE "NAME" CODE addr,len` would produce.
void append_code_file(TapFile& tape, const char* name, word addr,
                      const Bytes& data) {
    TapHeader h;
    h.type = TapFileType::Code;
    h.name = name;
    h.data_length = static_cast<word>(data.size());
    h.param1 = addr;
    h.param2 = 0x8000;
    tape.append(h.encode());
    tape.append(TapBlock::make(TAP_FLAG_DATA, data));
}

/// Writes a .tap to disk and inserts it into a deck.
bool make_deck(TapeDeck& deck, const std::string& path, const TapFile& tape) {
    return tape.save(path) && deck.insert(path);
}

Bytes ramp(size_t n, ::byte start = 0) {
    Bytes b(n);
    for (size_t i = 0; i < n; i++) b[i] = static_cast<::byte>(start + i);
    return b;
}

/// Runs one trapped LD-BYTES with the given keys held down, and reports
/// whether the trap served the load (true) or declined to the ROM (false).
/// `caps` and `space` are the two halves of BREAK.
bool serves_load_with_keys(bool caps, bool space) {
    static int counter = 0;
    const char* dir = std::getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    const std::string path = std::string(dir) + "/tapebrk_"
                           + std::to_string(counter++) + ".tap";
    TapFile tape;
    tape.append(TapBlock::make(TAP_FLAG_DATA, Bytes{0x11, 0x22, 0x33, 0x44}));
    TapeDeck deck;
    if (!tape.save(path) || !deck.insert(path)) return cleanup(path, false);

    Cpu cpu; cpu.reset();
    std::string rom = ROM_FILE_NAME;
    if (!cpu.get_memory()->load_rom(rom, ROM_START_ADDRESS, ROM_SIZE)) {
        return cleanup(path, false);
    }
    // Keys read active-low; row 0 bit 0 is CAPS SHIFT, row 7 bit 0 is SPACE.
    cpu.set_port_in_callback([caps, space](word port) -> ::byte {
        const ::byte row_select = static_cast<::byte>(port >> 8);
        ::byte keys = 0x1F;
        if (caps  && (row_select & 0x01) == 0) keys &= 0x1E;
        if (space && (row_select & 0x80) == 0) keys &= 0x1E;
        return static_cast<::byte>(keys | 0xE0);
    });
    TapeTrap trap(cpu, deck);
    trap.install();

    ::byte* mem = cpu.get_memory()->data();
    mem[0x8000] = 0xCD; mem[0x8001] = 0x56; mem[0x8002] = 0x05;
    RegisterState init{};
    init.pc = 0x8000; init.sp = 0xFF00;
    init.a = TAP_FLAG_DATA; init.ix = 0x6000; init.de = 4;
    init.f = FLAG_CARRY;
    cpu.set_registers(init);
    cpu.step();
    cpu.step();
    const bool served = trap.load_calls() == 1;
    std::remove(path.c_str());
    return served;
}

/// Calls LD-BYTES the way the ROM does -- via a CALL, so the trap has a return
/// address to pop -- and reports the resulting registers.
RegisterState call_ld_bytes(Cpu& cpu, ::byte flag, word dest, word length,
                            bool load = true) {
    ::byte* mem = cpu.get_memory()->data();
    mem[0x8000] = 0xCD; mem[0x8001] = 0x56; mem[0x8002] = 0x05;  // CALL 0x0556

    RegisterState r{};
    r.pc = 0x8000;
    r.sp = 0xFF00;
    r.a  = flag;
    r.ix = dest;
    r.de = length;
    r.f  = load ? FLAG_CARRY : 0;
    cpu.set_registers(r);

    cpu.step();     // the CALL
    cpu.step();     // LD-BYTES -- trapped
    return cpu.get_registers();
}

/// The same, for SA-BYTES.
RegisterState call_sa_bytes(Cpu& cpu, ::byte flag, word src, word length) {
    ::byte* mem = cpu.get_memory()->data();
    mem[0x8000] = 0xCD; mem[0x8001] = 0xC2; mem[0x8002] = 0x04;  // CALL 0x04C2

    RegisterState r = cpu.get_registers();
    r.pc = 0x8000;
    r.sp = 0xFF00;
    r.a  = flag;
    r.ix = src;
    r.de = length;
    cpu.set_registers(r);

    cpu.step();     // the CALL
    cpu.step();     // SA-BYTES -- trapped
    return cpu.get_registers();
}

} // namespace

void register_tape_trap_tests() {

    // ============================================================ TapeDeck ==

    add_test("deck: an empty deck reports NoTape", [] {
        TapeDeck deck;
        Memory mem;
        auto o = deck.load_block(TAP_FLAG_HEADER, mem, 0x6000, 17, false);
        return o.result == Result::NoTape && o.transferred == 0
            && !deck.inserted();
    });

    add_test("deck: insert of a missing file fails with an error", [] {
        TapeDeck deck;
        return !deck.insert("/nonexistent-dir-xyz/none.tap")
            && !deck.inserted() && !deck.last_error().empty();
    });

    add_test("deck: insert reads the blocks and rewinds", [] {
        const std::string path = temp_path("insert");
        TapFile tape;
        append_code_file(tape, "A", 0x6000, ramp(8));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);
        return cleanup(path, deck.inserted() && deck.block_count() == 2
                             && deck.position() == 0 && !deck.at_end());
    });

    add_test("deck: a matching data block transfers into memory", [] {
        const std::string path = temp_path("data");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(4, 0xA0)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Memory mem;
        auto o = deck.load_block(TAP_FLAG_DATA, mem, 0x6000, 4, false);
        return cleanup(path, o.result == Result::Ok && o.transferred == 4
                             && mem.read(0x6000) == 0xA0
                             && mem.read(0x6003) == 0xA3
                             && deck.position() == 1);
    });

    add_test("deck: a header block decodes after loading", [] {
        const std::string path = temp_path("hdr");
        TapFile tape;
        append_code_file(tape, "SCREEN", 0x4000, ramp(32));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Memory mem;
        auto o = deck.load_block(TAP_FLAG_HEADER, mem, 0x5B00, 17, false);
        if (o.result != Result::Ok) return cleanup(path, false);
        // Rebuild the block from what landed in memory and decode it.
        Bytes payload(17);
        for (size_t i = 0; i < 17; i++) payload[i] = mem.read(static_cast<word>(0x5B00 + i));
        TapHeader h;
        if (!TapHeader::decode(TapBlock::make(TAP_FLAG_HEADER, payload), h)) {
            return cleanup(path, false);
        }
        return cleanup(path, h.type == TapFileType::Code && h.name == "SCREEN"
                             && h.param1 == 0x4000 && h.data_length == 32);
    });

    add_test("deck: the wrong flag reports FlagMismatch and still advances", [] {
        // This is what lets the ROM's `JR NC,LD-LOOK-H` retry loop skip blocks.
        const std::string path = temp_path("skip");
        TapFile tape;
        append_code_file(tape, "one", 0x6000, ramp(4));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Memory mem;
        // Ask for a data block; the tape starts with a header.
        auto o = deck.load_block(TAP_FLAG_DATA, mem, 0x6000, 4, false);
        return cleanup(path, o.result == Result::FlagMismatch
                             && o.transferred == 0 && deck.position() == 1);
    });

    add_test("deck: retrying past a wrong block reaches the right one", [] {
        const std::string path = temp_path("retry");
        TapFile tape;
        append_code_file(tape, "first",  0x6000, ramp(4, 0x10));
        append_code_file(tape, "second", 0x7000, ramp(4, 0x20));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Memory mem;
        // Simulate LD-LOOK-H: keep asking for a header until one arrives.
        int tries = 0;
        Result res = Result::NoTape;
        while (tries < 8) {
            res = deck.load_block(TAP_FLAG_HEADER, mem, 0x5B00, 17, false).result;
            tries++;
            if (res == Result::Ok) break;
        }
        return cleanup(path, res == Result::Ok && tries == 1);
    });

    add_test("deck: playing past the last block reports EndOfTape", [] {
        const std::string path = temp_path("eot");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(2)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Memory mem;
        deck.load_block(TAP_FLAG_DATA, mem, 0x6000, 2, false);
        if (!deck.at_end()) return cleanup(path, false);
        auto o = deck.load_block(TAP_FLAG_DATA, mem, 0x6000, 2, false);
        return cleanup(path, o.result == Result::EndOfTape);
    });

    add_test("deck: rewind replays from the first block", [] {
        const std::string path = temp_path("rewind");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(2, 0x55)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Memory mem;
        deck.load_block(TAP_FLAG_DATA, mem, 0x6000, 2, false);
        deck.rewind();
        auto o = deck.load_block(TAP_FLAG_DATA, mem, 0x7000, 2, false);
        return cleanup(path, o.result == Result::Ok && deck.position() == 1
                             && mem.read(0x7000) == 0x55);
    });

    add_test("deck: a shorter block than asked for reports TooShort", [] {
        const std::string path = temp_path("short");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(4)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Memory mem;
        auto o = deck.load_block(TAP_FLAG_DATA, mem, 0x6000, 10, false);
        return cleanup(path, o.result == Result::TooShort && o.transferred == 0
                             && mem.read(0x6000) == 0);      // nothing stored
    });

    add_test("deck: a longer block than asked for reports LengthMismatch", [] {
        const std::string path = temp_path("long");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(10)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Memory mem;
        auto o = deck.load_block(TAP_FLAG_DATA, mem, 0x6000, 4, false);
        // The bytes asked for are still stored -- only the checksum fails.
        return cleanup(path, o.result == Result::LengthMismatch
                             && o.transferred == 4
                             && mem.read(0x6003) == 0x03);
    });

    add_test("deck: a corrupt checksum still leaves the bytes in memory", [] {
        // Real hardware stores each byte as it arrives and tests the checksum
        // only at the end, so a failed load leaves partial data behind.
        const std::string path = temp_path("badsum");
        TapFile tape;
        TapBlock b = TapBlock::make(TAP_FLAG_DATA, ramp(4, 0x70));
        b.checksum ^= 0xFF;
        tape.append(b);
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Memory mem;
        auto o = deck.load_block(TAP_FLAG_DATA, mem, 0x6000, 4, false);
        return cleanup(path, o.result == Result::BadChecksum
                             && o.transferred == 4
                             && mem.read(0x6000) == 0x70);
    });

    add_test("deck: verify mode does not write to memory", [] {
        const std::string path = temp_path("verify");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(4, 0x30)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Memory mem;
        for (size_t i = 0; i < 4; i++) mem.data()[0x6000 + i] = static_cast<::byte>(0x30 + i);
        auto o = deck.load_block(TAP_FLAG_DATA, mem, 0x6000, 4, true);
        if (o.result != Result::Ok) return cleanup(path, false);

        // Same block against different memory must now disagree.
        deck.rewind();
        mem.data()[0x6002] = 0x99;
        auto bad = deck.load_block(TAP_FLAG_DATA, mem, 0x6000, 4, true);
        return cleanup(path, bad.result == Result::VerifyMismatch
                             && bad.transferred == 2
                             && mem.read(0x6002) == 0x99);   // untouched
    });

    add_test("deck: a transfer running off the top of memory wraps", [] {
        const std::string path = temp_path("wrap");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(4, 0xC0)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        // All RAM, so the wrap is observable on its own; with the Spectrum's
        // real memory map the wrapped half lands in ROM and is discarded,
        // which the next test covers.
        Memory mem;
        mem.set_rom_end(0);
        auto o = deck.load_block(TAP_FLAG_DATA, mem, 0xFFFE, 4, false);
        return cleanup(path, o.result == Result::Ok
                             && mem.read(0xFFFE) == 0xC0 && mem.read(0xFFFF) == 0xC1
                             && mem.read(0x0000) == 0xC2 && mem.read(0x0001) == 0xC3);
    });

    add_test("deck: a load into ROM is discarded, as the hardware would", [] {
        const std::string path = temp_path("loadrom");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(4, 0xC0)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Memory mem;                       // default map: 0x0000-0x3FFF is ROM
        auto o = deck.load_block(TAP_FLAG_DATA, mem, 0x3FFE, 4, false);
        // The transfer still "happens" and reports success -- the ROM's loader
        // has no idea the bytes went nowhere -- but ROM is untouched and only
        // the halves in RAM stick.
        return cleanup(path, o.result == Result::Ok && o.transferred == 4
                             && mem.read(0x3FFE) == 0x00 && mem.read(0x3FFF) == 0x00
                             && mem.read(0x4000) == 0xC2 && mem.read(0x4001) == 0xC3);
    });

    add_test("deck: re-inserting rewinds a partly played tape", [] {
        const std::string path = temp_path("reinsert");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(2, 0x01)));
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(2, 0x02)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Memory mem;
        deck.load_block(TAP_FLAG_DATA, mem, 0x6000, 2, false);
        if (deck.position() != 1) return cleanup(path, false);
        if (!deck.insert()) return cleanup(path, false);
        return cleanup(path, deck.position() == 0 && !deck.at_end());
    });

    add_test("deck: eject empties the deck", [] {
        const std::string path = temp_path("eject");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(2)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);
        deck.eject();
        Memory mem;
        return cleanup(path, !deck.inserted() && deck.block_count() == 0
                             && deck.load_block(TAP_FLAG_DATA, mem, 0, 2, false)
                                    .result == Result::NoTape);
    });

    // ====================================================== TapeDeck: save ==

    add_test("deck: save with no filename reports NoPath", [] {
        TapeDeck deck;
        Memory mem;
        return deck.save_block(TAP_FLAG_DATA, mem, 0x6000, 4)
            == TapeDeck::SaveResult::NoPath;
    });

    add_test("deck: saving a data block writes a parseable .tap", [] {
        const std::string path = temp_path("sv1");
        TapeDeck deck(path);
        Memory mem;
        for (size_t i = 0; i < 4; i++) mem.data()[0x6000 + i] = static_cast<::byte>(0xB0 + i);

        if (deck.save_block(TAP_FLAG_DATA, mem, 0x6000, 4) != TapeDeck::SaveResult::Ok) {
            return cleanup(path, false);
        }
        TapFile back;
        if (!back.load(path)) return cleanup(path, false);
        return cleanup(path, back.size() == 1
                             && back.blocks()[0].flag == TAP_FLAG_DATA
                             && back.blocks()[0].payload == ramp(4, 0xB0)
                             && back.blocks()[0].checksum_valid());
    });

    add_test("deck: the saved checksum is the XOR the ROM would compute", [] {
        const std::string path = temp_path("sv2");
        TapeDeck deck(path);
        Memory mem;
        mem.data()[0x6000] = 0x0F;
        mem.data()[0x6001] = 0xF0;
        deck.save_block(TAP_FLAG_DATA, mem, 0x6000, 2);
        TapFile back;
        if (!back.load(path)) return cleanup(path, false);
        // 0xFF ^ 0x0F ^ 0xF0 = 0x00
        return cleanup(path, back.blocks()[0].checksum == 0x00);
    });

    add_test("deck: saving never destroys what is already on the tape", [] {
        // The reason append beats truncate: a commercial image whose game saves
        // a high score would otherwise wipe the tape it was loaded from.
        const std::string path = temp_path("sv3");
        TapFile existing;
        append_code_file(existing, "game", 0x6000, ramp(64));
        TapeDeck deck;
        if (!make_deck(deck, path, existing)) return cleanup(path, false);
        if (deck.block_count() != 2) return cleanup(path, false);

        Memory mem;
        deck.save_block(TAP_FLAG_HEADER, mem, 0x5B00, 17);
        deck.save_block(TAP_FLAG_DATA, mem, 0x6000, 8);

        TapFile back;
        if (!back.load(path)) return cleanup(path, false);
        TapHeader original;
        return cleanup(path, deck.block_count() == 4 && back.size() == 4
                             && TapHeader::decode(back.blocks()[0], original)
                             && original.name == "game"
                             && back.blocks()[1].payload.size() == 64);
    });

    add_test("deck: saving does not move the read head", [] {
        // Recording appends at the end; the head stays put, so what follows it
        // is still whatever the tape held there.
        const std::string path = temp_path("svhead");
        TapFile existing;
        append_code_file(existing, "old", 0x6000, ramp(8));
        TapeDeck deck;
        if (!make_deck(deck, path, existing)) return cleanup(path, false);

        Memory mem;
        deck.load_block(TAP_FLAG_HEADER, mem, 0x5B00, 17, false);
        if (deck.position() != 1) return cleanup(path, false);

        deck.save_block(TAP_FLAG_HEADER, mem, 0x5B00, 17);
        deck.save_block(TAP_FLAG_DATA, mem, 0x6000, 4);
        return cleanup(path, deck.position() == 1 && deck.block_count() == 4);
    });

    add_test("deck: on a fresh tape, save then load back needs no rewind", [] {
        // The reason the head is left alone: the common case still works.
        const std::string path = temp_path("svfresh");
        TapeDeck deck(path);
        Memory mem;
        for (size_t i = 0; i < 4; i++) mem.data()[0x6000 + i] = static_cast<::byte>(0x91 + i);
        deck.save_block(TAP_FLAG_HEADER, mem, 0x5B00, 17);
        deck.save_block(TAP_FLAG_DATA, mem, 0x6000, 4);
        if (deck.position() != 0) return cleanup(path, false);

        auto h = deck.load_block(TAP_FLAG_HEADER, mem, 0x5C00, 17, false);
        auto d = deck.load_block(TAP_FLAG_DATA, mem, 0x7000, 4, false);
        return cleanup(path, h.result == Result::Ok && d.result == Result::Ok
                             && mem.read(0x7000) == 0x91);
    });

    add_test("deck: a data block appends to the header just written", [] {
        const std::string path = temp_path("sv4");
        TapeDeck deck(path);
        Memory mem;
        deck.save_block(TAP_FLAG_HEADER, mem, 0x5B00, 17);
        deck.save_block(TAP_FLAG_DATA, mem, 0x6000, 8);
        TapFile back;
        if (!back.load(path)) return cleanup(path, false);
        return cleanup(path, back.size() == 2 && back.blocks()[0].is_header()
                             && back.blocks()[1].is_data()
                             && back.blocks()[1].payload.size() == 8);
    });

    add_test("deck: repeated saves build a multi-file tape", [] {
        const std::string path = temp_path("sv5");
        TapeDeck deck(path);
        Memory mem;
        for (int file = 0; file < 3; file++) {
            TapHeader h;
            h.type = TapFileType::Code;
            h.name = std::string("f") + static_cast<char>('0' + file);
            h.data_length = static_cast<word>(4 + file);
            h.param1 = 0x9000; h.param2 = 0x8000;
            const TapBlock hb = h.encode();
            for (size_t i = 0; i < hb.payload.size(); i++) {
                mem.data()[0x5B00 + i] = hb.payload[i];
            }
            deck.save_block(TAP_FLAG_HEADER, mem, 0x5B00, 17);
            deck.save_block(TAP_FLAG_DATA, mem, 0x6000, static_cast<word>(4 + file));
        }
        TapFile back;
        if (!back.load(path)) return cleanup(path, false);
        if (back.size() != 6) return cleanup(path, false);
        for (int file = 0; file < 3; file++) {
            TapHeader h;
            if (!TapHeader::decode(back.blocks()[file * 2], h)) return cleanup(path, false);
            if (h.name != std::string("f") + static_cast<char>('0' + file)) {
                return cleanup(path, false);
            }
            if (back.blocks()[file * 2 + 1].payload.size() != size_t(4 + file)) {
                return cleanup(path, false);
            }
        }
        return cleanup(path, true);
    });

    add_test("deck: new_tape clears the blocks and rewinds", [] {
        const std::string path = temp_path("newt");
        TapFile existing;
        append_code_file(existing, "old", 0x6000, ramp(8));
        TapeDeck deck;
        if (!make_deck(deck, path, existing)) return cleanup(path, false);
        Memory mem;
        deck.load_block(TAP_FLAG_HEADER, mem, 0x5B00, 17, false);

        deck.new_tape();
        return cleanup(path, deck.block_count() == 0 && deck.position() == 0
                             && deck.inserted() && deck.at_end());
    });

    add_test("deck: new_tape leaves the file on disk alone", [] {
        // F7 must be non-destructive: the file survives until the next save.
        const std::string path = temp_path("newt2");
        TapFile existing;
        append_code_file(existing, "keepme", 0x6000, ramp(8));
        TapeDeck deck;
        if (!make_deck(deck, path, existing)) return cleanup(path, false);

        deck.new_tape();
        TapFile still;
        if (!still.load(path)) return cleanup(path, false);
        if (still.size() != 2) return cleanup(path, false);

        // ...and re-inserting brings it straight back, which is what F6 does.
        if (!deck.insert()) return cleanup(path, false);
        return cleanup(path, deck.block_count() == 2);
    });

    add_test("deck: saving after new_tape writes only the new content", [] {
        const std::string path = temp_path("newt3");
        TapFile existing;
        append_code_file(existing, "old", 0x6000, ramp(64));
        TapeDeck deck;
        if (!make_deck(deck, path, existing)) return cleanup(path, false);

        deck.new_tape();
        Memory mem;
        deck.save_block(TAP_FLAG_HEADER, mem, 0x5B00, 17);
        deck.save_block(TAP_FLAG_DATA, mem, 0x6000, 4);

        TapFile back;
        if (!back.load(path)) return cleanup(path, false);
        return cleanup(path, back.size() == 2
                             && back.blocks()[1].payload.size() == 4);
    });

    add_test("deck: save makes the tape immediately loadable", [] {
        const std::string path = temp_path("sv6");
        TapeDeck deck(path);
        Memory mem;
        for (size_t i = 0; i < 6; i++) mem.data()[0x6000 + i] = static_cast<::byte>(0x50 + i);
        deck.save_block(TAP_FLAG_HEADER, mem, 0x5B00, 17);
        deck.save_block(TAP_FLAG_DATA, mem, 0x6000, 6);
        if (!deck.inserted() || deck.position() != 0) return cleanup(path, false);

        // Read it straight back into a different address.
        auto h = deck.load_block(TAP_FLAG_HEADER, mem, 0x5C00, 17, false);
        auto d = deck.load_block(TAP_FLAG_DATA, mem, 0x7000, 6, false);
        bool same = true;
        for (size_t i = 0; i < 6; i++) {
            if (mem.read(static_cast<word>(0x7000 + i)) != 0x50 + i) same = false;
        }
        return cleanup(path, h.result == Result::Ok && d.result == Result::Ok && same);
    });

    add_test("deck: save reads memory wrapping at the top", [] {
        const std::string path = temp_path("sv7");
        TapeDeck deck(path);
        Memory mem;
        mem.data()[0xFFFE] = 0x01; mem.data()[0xFFFF] = 0x02;
        mem.data()[0x0000] = 0x03; mem.data()[0x0001] = 0x04;
        deck.save_block(TAP_FLAG_DATA, mem, 0xFFFE, 4);
        TapFile back;
        if (!back.load(path)) return cleanup(path, false);
        return cleanup(path, back.blocks()[0].payload == Bytes{0x01, 0x02, 0x03, 0x04});
    });

    add_test("deck: an oversize block reports TooLarge and writes nothing", [] {
        const std::string path = temp_path("sv8");
        TapeDeck deck(path);
        Memory mem;
        auto r = deck.save_block(TAP_FLAG_DATA, mem, 0x0000, 0xFFFF);
        return cleanup(path, r == TapeDeck::SaveResult::TooLarge
                             && deck.block_count() == 0);
    });

    add_test("deck: an unwritable path reports WriteFailed", [] {
        TapeDeck deck("/nonexistent-dir-xyz/none.tap");
        Memory mem;
        return deck.save_block(TAP_FLAG_DATA, mem, 0x6000, 4)
                   == TapeDeck::SaveResult::WriteFailed
            && !deck.last_error().empty();
    });

    add_test("deck: a round trip through a real header preserves its fields", [] {
        const std::string path = temp_path("sv9");
        TapeDeck deck(path);
        Memory mem;
        // Lay a real 17-byte header out in memory, as the ROM's workspace does.
        TapHeader written;
        written.type = TapFileType::Code;
        written.name = "ROUNDTRIP";
        written.data_length = 12;
        written.param1 = 0x8000;
        written.param2 = 0x8000;
        const TapBlock hb = written.encode();
        for (size_t i = 0; i < hb.payload.size(); i++) {
            mem.data()[0x5B00 + i] = hb.payload[i];
        }
        for (size_t i = 0; i < 12; i++) mem.data()[0x8000 + i] = static_cast<::byte>(i * 3);

        deck.save_block(TAP_FLAG_HEADER, mem, 0x5B00, 17);
        deck.save_block(TAP_FLAG_DATA, mem, 0x8000, 12);

        TapFile back;
        if (!back.load(path)) return cleanup(path, false);
        TapHeader read;
        if (!TapHeader::decode(back.blocks()[0], read)) return cleanup(path, false);
        return cleanup(path, read == written
                             && back.blocks()[1].payload.size() == 12
                             && back.blocks()[1].payload[3] == 9);
    });

    // =========================================================== TapeTrap ==

    add_test("trap/tape: install and uninstall track both CPU traps", [] {
        Cpu cpu; cpu.reset();
        TapeDeck deck;
        TapeTrap trap(cpu, deck);
        if (cpu.has_trap(TapeTrap::LD_BYTES_ENTRY)) return false;
        if (cpu.has_trap(TapeTrap::SA_BYTES_ENTRY)) return false;
        trap.install();
        if (!trap.installed()) return false;
        if (!cpu.has_trap(TapeTrap::LD_BYTES_ENTRY)) return false;
        if (!cpu.has_trap(TapeTrap::SA_BYTES_ENTRY)) return false;
        trap.uninstall();
        return !trap.installed()
            && !cpu.has_trap(TapeTrap::LD_BYTES_ENTRY)
            && !cpu.has_trap(TapeTrap::SA_BYTES_ENTRY);
    });

    add_test("trap/tape: the destructor uninstalls", [] {
        Cpu cpu; cpu.reset();
        TapeDeck deck;
        {
            TapeTrap trap(cpu, deck);
            trap.install();
        }
        return !cpu.has_trap(TapeTrap::LD_BYTES_ENTRY);
    });

    add_test("trap/tape: a successful load sets carry and returns to the caller", [] {
        const std::string path = temp_path("ok");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(6, 0x41)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Cpu cpu; cpu.reset();
        TapeTrap trap(cpu, deck);
        trap.install();

        auto r = call_ld_bytes(cpu, TAP_FLAG_DATA, 0x6000, 6);
        return cleanup(path, (r.f & FLAG_CARRY) != 0
                             && r.pc == 0x8003                 // after the CALL
                             && r.sp == 0xFF00                 // stack balanced
                             && r.ix == 0x6006                 // IX advanced
                             && r.de == 0                      // DE counted down
                             && cpu.get_memory()->read(0x6000) == 0x41
                             && cpu.get_memory()->read(0x6005) == 0x46
                             && trap.load_calls() == 1
                             && trap.last_load_result() == Result::Ok);
    });

    add_test("trap/tape: a failed load clears carry", [] {
        const std::string path = temp_path("fail");
        TapFile tape;
        append_code_file(tape, "x", 0x6000, ramp(4));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Cpu cpu; cpu.reset();
        TapeTrap trap(cpu, deck);
        trap.install();

        // Ask for data; the tape starts with a header.
        auto r = call_ld_bytes(cpu, TAP_FLAG_DATA, 0x6000, 4);
        return cleanup(path, (r.f & FLAG_CARRY) == 0
                             && r.pc == 0x8003 && r.sp == 0xFF00
                             && trap.last_load_result() == Result::FlagMismatch);
    });

    add_test("trap/tape: verify mode is taken from the entry carry flag", [] {
        const std::string path = temp_path("vfy");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(4, 0x11)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Cpu cpu; cpu.reset();
        TapeTrap trap(cpu, deck);
        trap.install();

        // Enter with carry clear: VERIFY. Memory must be left alone.
        call_ld_bytes(cpu, TAP_FLAG_DATA, 0x6000, 4, /*load=*/false);
        return cleanup(path, cpu.get_memory()->read(0x6000) == 0
                             && trap.last_load_result() == Result::VerifyMismatch);
    });

    add_test("trap/tape: with no tape the trap declines to the real ROM", [] {
        Cpu cpu; cpu.reset();
        std::string rom = ROM_FILE_NAME;
        if (!cpu.get_memory()->load_rom(rom, ROM_START_ADDRESS, ROM_SIZE)) return false;
        TapeDeck deck;                       // nothing inserted
        TapeTrap trap(cpu, deck);
        trap.install();

        ::byte* mem = cpu.get_memory()->data();
        mem[0x8000] = 0xCD; mem[0x8001] = 0x56; mem[0x8002] = 0x05;
        RegisterState init{};
        init.pc = 0x8000; init.sp = 0xFF00; init.f = FLAG_CARRY;
        cpu.set_registers(init);
        cpu.step();                          // CALL
        cpu.step();                          // trap declines -> real LD-BYTES
        // The real routine's first instruction is INC D, and it has not
        // returned -- so PC is inside the ROM, not back at the caller.
        auto r = cpu.get_registers();
        return trap.load_calls() == 0 && r.pc > TapeTrap::LD_BYTES_ENTRY
            && r.pc < 0x0600;
    });

    add_test("trap/tape: BREAK makes the trap decline so the ROM can abort", [] {
        const std::string path = temp_path("brk");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(4)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Cpu cpu; cpu.reset();
        std::string rom = ROM_FILE_NAME;
        if (!cpu.get_memory()->load_rom(rom, ROM_START_ADDRESS, ROM_SIZE)) {
            return cleanup(path, false);
        }
        // CAPS SHIFT (row 0) + SPACE (row 7) both held down, active-low.
        cpu.set_port_in_callback([](word port) -> ::byte {
            const ::byte row_select = static_cast<::byte>(port >> 8);
            ::byte keys = 0x1F;
            if ((row_select & 0x01) == 0) keys &= 0x1E;   // CAPS SHIFT
            if ((row_select & 0x80) == 0) keys &= 0x1E;   // SPACE
            return static_cast<::byte>(keys | 0xE0);
        });
        TapeTrap trap(cpu, deck);
        trap.install();

        ::byte* mem = cpu.get_memory()->data();
        mem[0x8000] = 0xCD; mem[0x8001] = 0x56; mem[0x8002] = 0x05;
        RegisterState init{};
        init.pc = 0x8000; init.sp = 0xFF00; init.f = FLAG_CARRY;
        cpu.set_registers(init);
        cpu.step();
        cpu.step();
        // Declined: the tape did not move and the ROM routine is running.
        return cleanup(path, trap.load_calls() == 0 && deck.position() == 0
                             && cpu.get_registers().pc != 0x8003);
    });

    add_test("trap/tape: no keys held -- the trap serves the load", [] {
        return serves_load_with_keys(false, false);
    });

    add_test("trap/tape: SPACE alone is not BREAK", [] {
        // BREAK is both keys together; either one alone must not abort a load.
        return serves_load_with_keys(false, true);
    });

    add_test("trap/tape: CAPS SHIFT alone is not BREAK", [] {
        return serves_load_with_keys(true, false);
    });

    add_test("trap/tape: CAPS SHIFT + SPACE together is BREAK", [] {
        return !serves_load_with_keys(true, true);
    });

    add_test("trap/tape: header then data, the way LOAD drives LD-BYTES", [] {
        const std::string path = temp_path("pair");
        const Bytes payload = ramp(16, 0x90);
        TapFile tape;
        append_code_file(tape, "PROG", 0x7000, payload);
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Cpu cpu; cpu.reset();
        TapeTrap trap(cpu, deck);
        trap.install();

        // 1. LD-LOOK-H asks for a 17-byte header into the workspace.
        auto r1 = call_ld_bytes(cpu, TAP_FLAG_HEADER, 0x5B00, 17);
        if ((r1.f & FLAG_CARRY) == 0) return cleanup(path, false);

        Bytes hdr(17);
        for (size_t i = 0; i < 17; i++) hdr[i] = cpu.get_memory()->read(static_cast<word>(0x5B00 + i));
        TapHeader h;
        if (!TapHeader::decode(TapBlock::make(TAP_FLAG_HEADER, hdr), h)) {
            return cleanup(path, false);
        }

        // 2. LD-BLOCK then asks for the data, using the header's own address
        //    and length -- exactly what LOAD ""CODE does.
        auto r2 = call_ld_bytes(cpu, TAP_FLAG_DATA, h.param1, h.data_length);
        if ((r2.f & FLAG_CARRY) == 0) return cleanup(path, false);

        for (size_t i = 0; i < payload.size(); i++) {
            if (cpu.get_memory()->read(static_cast<word>(0x7000 + i)) != payload[i]) {
                return cleanup(path, false);
            }
        }
        return cleanup(path, trap.load_calls() == 2 && deck.at_end());
    });

    add_test("trap/tape: SA-BYTES writes a block and returns to the caller", [] {
        const std::string path = temp_path("trapsave");
        Cpu cpu; cpu.reset();
        TapeDeck deck(path);
        TapeTrap trap(cpu, deck);
        trap.install();

        for (size_t i = 0; i < 5; i++) {
            cpu.get_memory()->data()[0x6000 + i] = static_cast<::byte>(0xD0 + i);
        }
        auto r = call_sa_bytes(cpu, TAP_FLAG_DATA, 0x6000, 5);

        TapFile back;
        if (!back.load(path)) return cleanup(path, false);
        return cleanup(path, r.pc == 0x8003 && r.sp == 0xFF00
                             && r.ix == 0x6005 && r.de == 0
                             && trap.save_calls() == 1
                             && trap.last_save_result() == TapeDeck::SaveResult::Ok
                             && back.size() == 1
                             && back.blocks()[0].payload == ramp(5, 0xD0));
    });

    add_test("trap/tape: SA-BYTES leaves the flags alone", [] {
        // SA-BYTES has no success convention; the real routine preserves AF.
        const std::string path = temp_path("svflags");
        Cpu cpu; cpu.reset();
        TapeDeck deck(path);
        TapeTrap trap(cpu, deck);
        trap.install();

        ::byte* mem = cpu.get_memory()->data();
        mem[0x8000] = 0xCD; mem[0x8001] = 0xC2; mem[0x8002] = 0x04;
        RegisterState init{};
        init.pc = 0x8000; init.sp = 0xFF00;
        init.a = TAP_FLAG_DATA; init.ix = 0x6000; init.de = 2;
        init.f = 0xD7;                       // an arbitrary flag pattern
        cpu.set_registers(init);
        cpu.step();
        cpu.step();
        return cleanup(path, cpu.get_registers().f == 0xD7);
    });

    add_test("trap/tape: with no filename the save trap declines to the ROM", [] {
        Cpu cpu; cpu.reset();
        std::string rom = ROM_FILE_NAME;
        if (!cpu.get_memory()->load_rom(rom, ROM_START_ADDRESS, ROM_SIZE)) return false;
        TapeDeck deck;                        // no path
        TapeTrap trap(cpu, deck);
        trap.install();

        ::byte* mem = cpu.get_memory()->data();
        mem[0x8000] = 0xCD; mem[0x8001] = 0xC2; mem[0x8002] = 0x04;
        RegisterState init{};
        init.pc = 0x8000; init.sp = 0xFF00; init.a = TAP_FLAG_DATA; init.de = 2;
        cpu.set_registers(init);
        cpu.step();
        cpu.step();
        auto r = cpu.get_registers();
        // Declined: PC is inside the real SA-BYTES, not back at the caller.
        return trap.save_calls() == 0 && r.pc != 0x8003
            && r.pc >= TapeTrap::SA_BYTES_ENTRY && r.pc < 0x0540;
    });

    add_test("trap/tape: BREAK makes the save trap decline too", [] {
        // Same escape hatch as the load side: the trap bypasses SA-BYTES, so
        // the ROM's own BREAK check never runs unless the trap steps aside.
        const std::string path = temp_path("svbrk");
        Cpu cpu; cpu.reset();
        std::string rom = ROM_FILE_NAME;
        if (!cpu.get_memory()->load_rom(rom, ROM_START_ADDRESS, ROM_SIZE)) {
            return cleanup(path, false);
        }
        cpu.set_port_in_callback([](word port) -> ::byte {
            const ::byte row_select = static_cast<::byte>(port >> 8);
            ::byte keys = 0x1F;
            if ((row_select & 0x01) == 0) keys &= 0x1E;   // CAPS SHIFT
            if ((row_select & 0x80) == 0) keys &= 0x1E;   // SPACE
            return static_cast<::byte>(keys | 0xE0);
        });
        TapeDeck deck(path);
        TapeTrap trap(cpu, deck);
        trap.install();

        ::byte* mem = cpu.get_memory()->data();
        mem[0x8000] = 0xCD; mem[0x8001] = 0xC2; mem[0x8002] = 0x04;
        RegisterState init{};
        init.pc = 0x8000; init.sp = 0xFF00;
        init.a = TAP_FLAG_DATA; init.ix = 0x6000; init.de = 4;
        cpu.set_registers(init);
        cpu.step();
        cpu.step();
        // Declined: nothing written, and the real ROM routine is running.
        return cleanup(path, trap.save_calls() == 0 && deck.block_count() == 0
                             && cpu.get_registers().pc != 0x8003);
    });

    add_test("trap/tape: header then data through the trap, as SAVE drives it", [] {
        const std::string path = temp_path("svpair");
        Cpu cpu; cpu.reset();
        TapeDeck deck(path);
        TapeTrap trap(cpu, deck);
        trap.install();

        TapHeader h;
        h.type = TapFileType::Code; h.name = "VIA TRAP";
        h.data_length = 10; h.param1 = 0x9000; h.param2 = 0x8000;
        const TapBlock hb = h.encode();
        for (size_t i = 0; i < hb.payload.size(); i++) {
            cpu.get_memory()->data()[0x5B00 + i] = hb.payload[i];
        }
        for (size_t i = 0; i < 10; i++) {
            cpu.get_memory()->data()[0x9000 + i] = static_cast<::byte>(0x21 + i);
        }

        call_sa_bytes(cpu, TAP_FLAG_HEADER, 0x5B00, 17);
        call_sa_bytes(cpu, TAP_FLAG_DATA, 0x9000, 10);

        TapFile back;
        if (!back.load(path)) return cleanup(path, false);
        TapHeader read;
        if (back.size() != 2) return cleanup(path, false);
        if (!TapHeader::decode(back.blocks()[0], read)) return cleanup(path, false);
        return cleanup(path, read == h && trap.save_calls() == 2
                             && back.blocks()[1].payload == ramp(10, 0x21));
    });

    add_test("trap/tape: save then load round-trips through the CPU", [] {
        // The milestone's own acceptance test: SAVE it, wipe memory, LOAD it
        // back, and the bytes must return -- all through the trapped routines.
        const std::string path = temp_path("rt");
        Cpu cpu; cpu.reset();
        TapeDeck deck(path);
        TapeTrap trap(cpu, deck);
        trap.install();

        TapHeader h;
        h.type = TapFileType::Code; h.name = "RT";
        h.data_length = 24; h.param1 = 0x9000; h.param2 = 0x8000;
        const TapBlock hb = h.encode();
        for (size_t i = 0; i < hb.payload.size(); i++) {
            cpu.get_memory()->data()[0x5B00 + i] = hb.payload[i];
        }
        for (size_t i = 0; i < 24; i++) {
            cpu.get_memory()->data()[0x9000 + i] = static_cast<::byte>(i * 11 + 3);
        }

        call_sa_bytes(cpu, TAP_FLAG_HEADER, 0x5B00, 17);
        call_sa_bytes(cpu, TAP_FLAG_DATA, 0x9000, 24);

        // Wipe both the source data and the header workspace.
        for (size_t i = 0; i < 24; i++) cpu.get_memory()->data()[0x9000 + i] = 0;
        for (size_t i = 0; i < 17; i++) cpu.get_memory()->data()[0x5B00 + i] = 0;

        // A SAVE leaves the tape rewound, so LOAD can read it straight back.
        auto r1 = call_ld_bytes(cpu, TAP_FLAG_HEADER, 0x5B00, 17);
        if ((r1.f & FLAG_CARRY) == 0) return cleanup(path, false);
        Bytes payload(17);
        for (size_t i = 0; i < 17; i++) payload[i] = cpu.get_memory()->read(static_cast<word>(0x5B00 + i));
        TapHeader read;
        if (!TapHeader::decode(TapBlock::make(TAP_FLAG_HEADER, payload), read)) {
            return cleanup(path, false);
        }
        if (read != h) return cleanup(path, false);

        auto r2 = call_ld_bytes(cpu, TAP_FLAG_DATA, read.param1, read.data_length);
        if ((r2.f & FLAG_CARRY) == 0) return cleanup(path, false);
        for (size_t i = 0; i < 24; i++) {
            if (cpu.get_memory()->read(static_cast<word>(0x9000 + i)) != (i * 11 + 3) % 256) {
                return cleanup(path, false);
            }
        }
        return cleanup(path, true);
    });

    add_test("trap/tape: save three files, rewind, load the middle one by name", [] {
        // The whole append feature end to end, through the trapped routines:
        // build a multi-file tape with SA-BYTES, rewind, then run the ROM's own
        // search algorithm (ask for a header, reject it, ask again) to pick out
        // a file that is neither first nor last.
        const std::string path = temp_path("multisave");
        Cpu cpu; cpu.reset();
        TapeDeck deck(path);
        TapeTrap trap(cpu, deck);
        trap.install();

        const char* names[3] = { "one", "two", "three" };
        for (int f = 0; f < 3; f++) {
            TapHeader h;
            h.type = TapFileType::Code;
            h.name = names[f];
            h.data_length = 8;
            h.param1 = static_cast<word>(0x9000 + f * 0x100);
            h.param2 = 0x8000;
            const TapBlock hb = h.encode();
            for (size_t i = 0; i < hb.payload.size(); i++) {
                cpu.get_memory()->data()[0x5B00 + i] = hb.payload[i];
            }
            for (size_t i = 0; i < 8; i++) {
                cpu.get_memory()->data()[0x6000 + i] = static_cast<::byte>(0x10 * (f + 1) + i);
            }
            call_sa_bytes(cpu, TAP_FLAG_HEADER, 0x5B00, 17);
            call_sa_bytes(cpu, TAP_FLAG_DATA, 0x6000, 8);
        }
        if (deck.block_count() != 6) return cleanup(path, false);

        deck.rewind();                       // what F5 does

        // The ROM's LOAD "two" loop: keep asking for a header until the name
        // matches. A rejected header leaves its data block next, which fails
        // the flag check and is skipped in turn -- that is what walks the tape.
        TapHeader found;
        bool matched = false;
        for (int attempt = 0; attempt < 12 && !matched; attempt++) {
            auto r = call_ld_bytes(cpu, TAP_FLAG_HEADER, 0x5B00, 17);
            if ((r.f & FLAG_CARRY) == 0) continue;          // not a header; retry
            Bytes hdr(17);
            for (size_t i = 0; i < 17; i++) {
                hdr[i] = cpu.get_memory()->read(static_cast<word>(0x5B00 + i));
            }
            if (!TapHeader::decode(TapBlock::make(TAP_FLAG_HEADER, hdr), found)) continue;
            matched = (found.name == "two");
        }
        if (!matched) return cleanup(path, false);

        auto d = call_ld_bytes(cpu, TAP_FLAG_DATA, found.param1, found.data_length);
        if ((d.f & FLAG_CARRY) == 0) return cleanup(path, false);

        // 'two' is the second file, so its bytes start at 0x20 and land at 0x9100.
        for (size_t i = 0; i < 8; i++) {
            if (cpu.get_memory()->read(static_cast<word>(0x9100 + i)) != 0x20 + i) {
                return cleanup(path, false);
            }
        }
        return cleanup(path, found.param1 == 0x9100 && found.data_length == 8);
    });

    add_test("trap/tape: the ROM's own LD-BLOCK sequence takes the RET C", [] {
        // LD-BLOCK at 0x0802 is `CALL LD-BYTES; RET C`. Running the genuine ROM
        // code with the trap installed must fall through the RET C on success.
        const std::string path = temp_path("ldblock");
        TapFile tape;
        tape.append(TapBlock::make(TAP_FLAG_DATA, ramp(4, 0x77)));
        TapeDeck deck;
        if (!make_deck(deck, path, tape)) return cleanup(path, false);

        Cpu cpu; cpu.reset();
        std::string rom = ROM_FILE_NAME;
        if (!cpu.get_memory()->load_rom(rom, ROM_START_ADDRESS, ROM_SIZE)) {
            return cleanup(path, false);
        }
        TapeTrap trap(cpu, deck);
        trap.install();

        // Return address for LD-BLOCK's own RET.
        cpu.get_memory()->data()[0xFEFE] = 0x00;
        cpu.get_memory()->data()[0xFEFF] = 0x90;      // -> 0x9000

        RegisterState init{};
        init.pc = 0x0802;             // LD-BLOCK
        init.sp = 0xFEFE;
        init.a  = TAP_FLAG_DATA;
        init.ix = 0x6000;
        init.de = 4;
        init.f  = FLAG_CARRY;
        cpu.set_registers(init);

        cpu.step();                   // CALL 0x0556
        cpu.step();                   // trapped LD-BYTES
        cpu.step();                   // RET C -- carry is set, so taken
        auto r = cpu.get_registers();
        return cleanup(path, r.pc == 0x9000
                             && cpu.get_memory()->read(0x6000) == 0x77);
    });
}
