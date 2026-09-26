// ROM write protection. On a 48K Spectrum 0x0000-0x3FFF is ROM and the
// hardware ignores stores there -- and the ROM *depends* on that: SKIP-CONS at
// 0x33FB does `LD DE,$0000` then calls STK-CONST, so that stacking a 5-byte
// floating-point constant into address 0 throws it away. Every numeric literal
// typed into a BASIC line goes through that path.

#include "z80/z80_cpu.h"
#include "test_harness.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using testing::add_test;
using z80::Cpu;
using z80::Memory;
using z80::RegisterState;

namespace {

/// Puts `bytes` at `addr` (through data(), which bypasses protection, the way
/// a loader would) and runs one instruction from there.
RegisterState run_at(Cpu& cpu, word addr, std::initializer_list<byte> bytes,
                     RegisterState initial) {
    byte* mem = cpu.get_memory()->data();
    word i = addr;
    for (byte b : bytes) mem[i++] = b;
    initial.pc = addr;
    cpu.set_registers(initial);
    cpu.step();
    return cpu.get_registers();
}

constexpr word ROM_ADDR = 0x1234;   // inside ROM
constexpr word RAM_ADDR = 0x9000;   // inside RAM

} // namespace

void register_rom_protect_tests() {

    // ======================================================= Memory itself ==

    add_test("rom: write() stores in RAM", [] {
        Memory m;
        m.write(RAM_ADDR, 0xA5);
        return m.read(RAM_ADDR) == 0xA5;
    });

    add_test("rom: write() is discarded in ROM", [] {
        Memory m;
        m.data()[ROM_ADDR] = 0x11;       // as the ROM loader would
        m.write(ROM_ADDR, 0xA5);
        return m.read(ROM_ADDR) == 0x11;
    });

    add_test("rom: the boundary is exactly ROM_SIZE", [] {
        Memory m;
        m.write(static_cast<word>(ROM_SIZE - 1), 0xAA);   // last ROM byte
        m.write(static_cast<word>(ROM_SIZE), 0xBB);       // first RAM byte
        return m.read(static_cast<word>(ROM_SIZE - 1)) == 0x00
            && m.read(static_cast<word>(ROM_SIZE)) == 0xBB;
    });

    add_test("rom: writable() gives the real cell in RAM", [] {
        Memory m;
        *m.writable(RAM_ADDR) = 0x5A;
        return m.read(RAM_ADDR) == 0x5A;
    });

    add_test("rom: writable() in ROM reads true but discards the store", [] {
        Memory m;
        m.data()[ROM_ADDR] = 0x3C;
        byte* p = m.writable(ROM_ADDR);
        if (*p != 0x3C) return false;    // the read half must be correct
        *p = 0xFF;
        return m.read(ROM_ADDR) == 0x3C; // the store half must vanish
    });

    add_test("rom: set_rom_end(0) makes the whole space writable", [] {
        Memory m;
        m.set_rom_end(0);
        m.write(0x0000, 0x7E);
        return m.read(0x0000) == 0x7E && m.rom_end() == 0;
    });

    add_test("rom: load_rom bypasses protection", [] {
        Memory m;
        std::string path = ROM_FILE_NAME;
        return m.load_rom(path, ROM_START_ADDRESS, ROM_SIZE)
            && m.read(0x0000) == 0xF3;
    });

    // ============================================== stores from the Z80 ==

    add_test("rom: LD (HL),A into ROM is discarded", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[ROM_ADDR] = 0x11;
        RegisterState init{}; init.sp = 0xFF00; init.hl = ROM_ADDR; init.a = 0xA5;
        run_at(cpu, RAM_ADDR, {0x77}, init);
        return cpu.get_memory()->read(ROM_ADDR) == 0x11;
    });

    add_test("rom: LD (HL),A into RAM still works", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0xFF00; init.hl = 0x9100; init.a = 0xA5;
        run_at(cpu, RAM_ADDR, {0x77}, init);
        return cpu.get_memory()->read(0x9100) == 0xA5;
    });

    add_test("rom: LD (DE),A into address 0 is discarded (the SKIP-CONS case)", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[0x0000] = 0xF3;
        RegisterState init{}; init.sp = 0xFF00; init.de = 0x0000; init.a = 0x81;
        run_at(cpu, RAM_ADDR, {0x12}, init);
        return cpu.get_memory()->read(0x0000) == 0xF3;
    });

    add_test("rom: LD (nn),A into ROM is discarded", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[ROM_ADDR] = 0x22;
        RegisterState init{}; init.sp = 0xFF00; init.a = 0x99;
        run_at(cpu, RAM_ADDR, {0x32, 0x34, 0x12}, init);
        return cpu.get_memory()->read(ROM_ADDR) == 0x22;
    });

    add_test("rom: LD (HL),n into ROM is discarded", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[ROM_ADDR] = 0x33;
        RegisterState init{}; init.sp = 0xFF00; init.hl = ROM_ADDR;
        run_at(cpu, RAM_ADDR, {0x36, 0x77}, init);
        return cpu.get_memory()->read(ROM_ADDR) == 0x33;
    });

    add_test("rom: LD (nn),HL into ROM is discarded", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[ROM_ADDR] = 0x44;
        cpu.get_memory()->data()[ROM_ADDR + 1] = 0x55;
        RegisterState init{}; init.sp = 0xFF00; init.hl = 0xBEEF;
        run_at(cpu, RAM_ADDR, {0x22, 0x34, 0x12}, init);
        return cpu.get_memory()->read(ROM_ADDR) == 0x44
            && cpu.get_memory()->read(ROM_ADDR + 1) == 0x55;
    });

    add_test("rom: PUSH into ROM is discarded", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[0x1000] = 0x66;
        cpu.get_memory()->data()[0x1001] = 0x77;
        RegisterState init{}; init.sp = 0x1002; init.bc = 0x1234;
        auto r = run_at(cpu, RAM_ADDR, {0xC5}, init);   // PUSH BC
        // SP still moves -- only the store is swallowed.
        return r.sp == 0x1000
            && cpu.get_memory()->read(0x1000) == 0x66
            && cpu.get_memory()->read(0x1001) == 0x77;
    });

    // ============================== read-modify-write must still read right ==

    add_test("rom: INC (HL) on ROM sets flags from the real value but stores nothing", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[ROM_ADDR] = 0xFF;
        RegisterState init{}; init.sp = 0xFF00; init.hl = ROM_ADDR;
        auto r = run_at(cpu, RAM_ADDR, {0x34}, init);   // INC (HL)
        // 0xFF + 1 = 0x00, so Z must be set from the true ROM contents.
        return cpu.get_memory()->read(ROM_ADDR) == 0xFF && (r.f & 0x40) != 0;
    });

    add_test("rom: SET b,(HL) on ROM is discarded", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[ROM_ADDR] = 0x00;
        RegisterState init{}; init.sp = 0xFF00; init.hl = ROM_ADDR;
        run_at(cpu, RAM_ADDR, {0xCB, 0xC6}, init);      // SET 0,(HL)
        return cpu.get_memory()->read(ROM_ADDR) == 0x00;
    });

    add_test("rom: BIT b,(HL) on ROM still reads the real value", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[ROM_ADDR] = 0x01;
        RegisterState init{}; init.sp = 0xFF00; init.hl = ROM_ADDR;
        auto r = run_at(cpu, RAM_ADDR, {0xCB, 0x46}, init);  // BIT 0,(HL)
        return (r.f & 0x40) == 0;                            // bit set -> Z clear
    });

    add_test("rom: SET b,(IX+d) into ROM is discarded", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[ROM_ADDR] = 0x00;
        RegisterState init{}; init.sp = 0xFF00; init.ix = ROM_ADDR - 2;
        run_at(cpu, RAM_ADDR, {0xDD, 0xCB, 0x02, 0xC6}, init);
        return cpu.get_memory()->read(ROM_ADDR) == 0x00;
    });

    add_test("rom: LDI into ROM is discarded but still counts down", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[RAM_ADDR + 0x100] = 0xAB;
        cpu.get_memory()->data()[ROM_ADDR] = 0xCD;
        RegisterState init{};
        init.sp = 0xFF00; init.hl = RAM_ADDR + 0x100; init.de = ROM_ADDR; init.bc = 4;
        auto r = run_at(cpu, RAM_ADDR, {0xED, 0xA0}, init);
        return cpu.get_memory()->read(ROM_ADDR) == 0xCD
            && r.bc == 3 && r.de == ROM_ADDR + 1;
    });

    // ================================================== every opcode swept ==

    add_test("rom: no opcode on any page can write to ROM", [] {
        // Aims every memory-addressing register at ROM and runs one
        // instruction for each opcode on each page. A write path added later
        // without protection shows up here rather than as silent corruption
        // -- which is how the LD (nn),HL case was found, since it used
        // `data() + offset` rather than indexing.
        Cpu cpu;
        std::string path = ROM_FILE_NAME;
        if (!cpu.get_memory()->load_rom(path, ROM_START_ADDRESS, ROM_SIZE)) return false;
        std::vector<byte> pristine(cpu.get_memory()->data(),
                                   cpu.get_memory()->data() + ROM_SIZE);
        cpu.set_port_in_callback([](word) -> byte { return 0x5A; });

        const std::vector<std::vector<byte>> prefixes = {
            {}, {0xCB}, {0xED}, {0xDD}, {0xFD}, {0xDD, 0xCB, 0x02}, {0xFD, 0xCB, 0x02},
        };

        for (const std::vector<byte>& prefix : prefixes) {
            for (int op = 0; op <= 0xFF; op++) {
                byte* mem = cpu.get_memory()->data();
                std::copy(pristine.begin(), pristine.end(), mem);   // restore ROM
                cpu.reset();

                word pc = 0x8000;
                for (byte b : prefix) mem[pc++] = b;
                mem[pc++] = static_cast<byte>(op);
                mem[pc++] = 0x34; mem[pc++] = 0x12;   // any immediate operands

                RegisterState st{};
                st.pc = 0x8000;
                st.hl = 0x1000; st.de = 0x1100; st.bc = 0x0004;
                st.ix = 0x1200; st.iy = 0x1300; st.sp = 0x1400;
                st.a = 0xA5;
                cpu.set_registers(st);
                cpu.step();

                if (std::memcmp(cpu.get_memory()->data(), pristine.data(),
                                ROM_SIZE) != 0) {
                    return false;
                }
            }
        }
        return true;
    });

    // ========================================= the bug that prompted all this ==

    add_test("rom: a BASIC numeric literal no longer scribbles on the reset vector", [] {
        // SKIP-CONS writes a 5-byte FP constant to address 0 on purpose. Before
        // write protection, typing any number corrupted 0x0000-0x0004.
        Cpu cpu;
        std::string path = ROM_FILE_NAME;
        if (!cpu.get_memory()->load_rom(path, ROM_START_ADDRESS, ROM_SIZE)) return false;

        Memory pristine;
        std::string again = ROM_FILE_NAME;
        if (!pristine.load_rom(again, ROM_START_ADDRESS, ROM_SIZE)) return false;

        cpu.reset();
        cpu.set_realtime(false);   // emulated time is what matters here
        static uint8_t keys[8];
        for (int i = 0; i < 8; i++) keys[i] = 0x1F;
        long frames = 0;
        cpu.set_frame_callback([&frames](Cpu*) { frames++; });
        cpu.set_port_in_callback([](word port) -> byte {
            if ((port & 1) != 0) return 0xFF;
            const byte sel = static_cast<byte>(port >> 8);
            byte k = 0x1F;
            for (int r = 0; r < 8; r++) if ((sel & (1 << r)) == 0) k &= keys[r];
            return static_cast<byte>(k | 0xE0);
        });

        auto advance = [&](long n) { const long t = frames + n; while (frames < t) cpu.step(); };
        auto tap = [&](int row, int bit) {
            keys[row] = static_cast<uint8_t>(keys[row] & ~(1 << bit));
            advance(4);
            keys[row] = static_cast<uint8_t>(keys[row] | (1 << bit));
            advance(8);
        };

        advance(140);                 // boot
        tap(5, 0);                    // P -> PRINT
        tap(3, 0);                    // 1
        tap(6, 0);                    // ENTER
        advance(40);

        for (size_t a = 0; a < ROM_SIZE; a++) {
            if (cpu.get_memory()->read(static_cast<word>(a))
                != pristine.read(static_cast<word>(a))) {
                return false;
            }
        }
        return true;
    });
}
