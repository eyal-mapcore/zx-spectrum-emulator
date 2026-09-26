// Tests for the address space itself: that all 64K is reachable, that the top
// byte is both addressable and executable, and that load_rom's bounds hold.

#include "z80/z80_cpu.h"
#include "test_harness.h"

#include <string>

using testing::add_test;
using z80::Cpu;
using z80::Memory;
using z80::RegisterState;

void register_memory_tests() {

    // ============================================== the full address space ==

    add_test("mem: MEMORY_SIZE spans the whole 16-bit address space", [] {
        return MEMORY_SIZE == 0x10000;
    });

    add_test("mem: the top address 0xFFFF is addressable", [] {
        Memory m;
        m.data()[0xFFFF] = 0xA5;
        return m.read(0xFFFF) == 0xA5;
    });

    add_test("mem: every address from 0 to 0xFFFF round-trips", [] {
        Memory m;
        for (size_t a = 0; a < MEMORY_SIZE; a++) {
            m.data()[a] = static_cast<byte>(a * 7 + 1);
        }
        for (size_t a = 0; a < MEMORY_SIZE; a++) {
            if (m.read(static_cast<word>(a)) != static_cast<byte>(a * 7 + 1)) return false;
        }
        return true;
    });

    add_test("mem: construction zeroes the whole space, top byte included", [] {
        Memory m;
        for (size_t a = 0; a < MEMORY_SIZE; a++) {
            if (m.data()[a] != 0) return false;
        }
        return true;
    });

    add_test("mem: writing the top byte does not disturb its neighbour", [] {
        Memory m;
        m.data()[0xFFFE] = 0x11;
        m.data()[0xFFFF] = 0x22;
        return m.read(0xFFFE) == 0x11 && m.read(0xFFFF) == 0x22;
    });

    // ============================================================ load_rom ==

    add_test("mem: load_rom succeeds at the default size", [] {
        // Regression: size_ used to be a word, so MEMORY_SIZE truncated to 0
        // and this bounds check rejected every load.
        Memory m;
        std::string path = ROM_FILE_NAME;
        return m.load_rom(path, ROM_START_ADDRESS, ROM_SIZE)
            && m.read(0x0000) == 0xF3;          // the ROM opens with DI
    });

    add_test("mem: load_rom accepts a region ending exactly at the top", [] {
        Memory m;
        std::string path = ROM_FILE_NAME;
        return m.load_rom(path, static_cast<word>(MEMORY_SIZE - ROM_SIZE), ROM_SIZE);
    });

    add_test("mem: load_rom rejects a region that would overrun the top", [] {
        Memory m;
        std::string path = ROM_FILE_NAME;
        return !m.load_rom(path, static_cast<word>(MEMORY_SIZE - ROM_SIZE + 1), ROM_SIZE);
    });

    add_test("mem: load_rom rejects a missing file", [] {
        Memory m;
        std::string path = "/nonexistent-dir-xyz/none.rom";
        return !m.load_rom(path, 0, ROM_SIZE);
    });

    // ================================= executing at the top of the address ==

    add_test("mem: a 1-byte instruction at 0xFFFF executes and wraps PC to 0", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[0xFFFF] = 0x3C;    // INC A
        RegisterState init{};
        init.pc = 0xFFFF; init.sp = 0x8000;
        cpu.set_registers(init);
        cpu.step();
        auto r = cpu.get_registers();
        // Previously this halted instead: the guard was '>=' and 0xFFFF was
        // outside the array in the first place.
        return r.a == 1 && r.pc == 0x0000 && !r.halted;
    });

    add_test("mem: a 2-byte instruction ending on the last byte executes", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[0xFFFE] = 0x3E;    // LD A,n
        cpu.get_memory()->data()[0xFFFF] = 0x7B;
        RegisterState init{};
        init.pc = 0xFFFE; init.sp = 0x8000;
        cpu.set_registers(init);
        cpu.step();
        auto r = cpu.get_registers();
        return r.a == 0x7B && r.pc == 0x0000 && !r.halted;
    });

    add_test("mem: a 4-byte instruction ending on the last byte executes", [] {
        Cpu cpu; cpu.reset();
        byte* mem = cpu.get_memory()->data();
        mem[0xFFFC] = 0xDD; mem[0xFFFD] = 0xCB;     // DD CB d op
        mem[0xFFFE] = 0x02; mem[0xFFFF] = 0xC6;     // SET 0,(IX+2)
        RegisterState init{};
        init.pc = 0xFFFC; init.sp = 0x8000; init.ix = 0x6000;
        cpu.set_registers(init);
        cpu.step();
        auto r = cpu.get_registers();
        return cpu.get_memory()->read(0x6002) == 0x01
            && r.pc == 0x0000 && !r.halted;
    });

    add_test("mem: an instruction running past the top halts instead", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[0xFFFF] = 0x3E;    // LD A,n -- operand is off the end
        RegisterState init{};
        init.pc = 0xFFFF; init.sp = 0x8000;
        cpu.set_registers(init);
        cpu.step();
        auto r = cpu.get_registers();
        return r.halted && r.pc == 0xFFFF;
    });

    add_test("mem: a 4-byte opcode fetched at 0xFFFF halts cleanly", [] {
        // decode() reads its bytes before the length is known, so this fetch
        // reads into the guard bytes. It must halt rather than misbehave.
        Cpu cpu; cpu.reset();
        byte* mem = cpu.get_memory()->data();
        mem[0xFFFF] = 0xDD;                         // start of a DD CB d op
        RegisterState init{};
        init.pc = 0xFFFF; init.sp = 0x8000;
        cpu.set_registers(init);
        cpu.step();
        return cpu.get_registers().halted;
    });

    add_test("mem: the ROM still boots correctly across the whole address space", [] {
        // End-to-end regression for the MEMORY_SIZE change. Deliberately does
        // not assert an exact SP: initialisation is only complete around frame
        // 120, by which point PC is usually inside the 0x0038 interrupt
        // handler with its own pushes on the stack, so SP is not stable.
        Cpu cpu;
        std::string path = ROM_FILE_NAME;
        if (!cpu.get_memory()->load_rom(path, ROM_START_ADDRESS, ROM_SIZE)) return false;
        cpu.reset();
        cpu.set_realtime(false);   // emulated time is what matters here
        long frames = 0;
        cpu.set_frame_callback([&frames](Cpu*) { frames++; });
        cpu.set_port_in_callback([](word) -> byte { return 0xFF; });
        while (frames < 120) cpu.step();

        // Every cell black-on-white: the boot screen's attribute value.
        for (size_t a = ATTRIBUTE_MEMORY_START; a <= ATTRIBUTE_MEMORY_END; a++) {
            if (cpu.get_memory()->read(static_cast<word>(a)) != 0x38) return false;
        }
        // Initialisation ran to completion (REPDEL/REPPER set at ROM 0x1273).
        if (cpu.get_memory()->read(0x5C09) != 35) return false;
        if (cpu.get_memory()->read(0x5C0A) != 5) return false;
        // Something was actually drawn -- the copyright line.
        int inked = 0;
        for (size_t a = SCREEN_MEMORY_START; a <= SCREEN_MEMORY_END; a++) {
            if (cpu.get_memory()->read(static_cast<word>(a)) != 0) inked++;
        }
        if (inked == 0) return false;
        // And the ROM itself is untouched: a wild write into ROM is exactly
        // the class of bug that address-space arithmetic causes.
        Memory pristine;
        std::string again = ROM_FILE_NAME;
        if (!pristine.load_rom(again, ROM_START_ADDRESS, ROM_SIZE)) return false;
        for (size_t a = 0; a < ROM_SIZE; a++) {
            if (cpu.get_memory()->read(static_cast<word>(a)) != pristine.read(static_cast<word>(a))) {
                return false;
            }
        }
        return true;
    });
}
