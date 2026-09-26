// Unit tests for the Cpu PC-trap mechanism (milestone 2 of the tape module).
// The trap is what lets a peripheral stand in for a ROM routine without
// patching the ROM image.

#include "z80/z80_cpu.h"
#include "test_harness.h"

#include <string>
#include <vector>

using testing::add_test;
using z80::Cpu;
using z80::RegisterState;

namespace {

constexpr byte TRAP_FLAG_C = 0x01;

/// Puts `bytes` at `addr` and sets PC there, leaving everything else default.
void place(Cpu& cpu, word addr, std::initializer_list<byte> bytes) {
    byte* mem = cpu.get_memory()->data();
    word i = addr;
    for (byte b : bytes) mem[i++] = b;
    RegisterState r{};
    r.pc = addr;
    r.sp = 0x8000;
    cpu.set_registers(r);
}

/// Writes a word into memory low byte first, the way the Z80 does.
void poke_word(Cpu& cpu, word addr, word value) {
    cpu.get_memory()->data()[addr]     = static_cast<byte>(value & 0xFF);
    cpu.get_memory()->data()[addr + 1] = static_cast<byte>(value >> 8);
}

} // namespace

void register_trap_tests() {

    // ========================================================== basic gate ==

    add_test("trap: with no trap set the instruction executes normally", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x3C});             // INC A
        cpu.step();
        auto r = cpu.get_registers();
        return r.a == 1 && r.pc == 0x1001;
    });

    add_test("trap: a trap runs instead of the instruction", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x3C});             // INC A -- must NOT run
        bool called = false;
        cpu.set_trap(0x1000, [&called](Cpu* c) {
            called = true;
            c->set_pc(0x2000);                  // stand in for a jump
            return true;
        });
        cpu.step();
        auto r = cpu.get_registers();
        return called && r.a == 0 && r.pc == 0x2000;
    });

    add_test("trap: returning false declines and the real instruction runs", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x3C});             // INC A
        int calls = 0;
        cpu.set_trap(0x1000, [&calls](Cpu*) { calls++; return false; });
        cpu.step();
        auto r = cpu.get_registers();
        // The callback was consulted, declined, and INC A went ahead.
        return calls == 1 && r.a == 1 && r.pc == 0x1001;
    });

    add_test("trap: only fires at its own address", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x3C, 0x3C});       // INC A; INC A
        int calls = 0;
        cpu.set_trap(0x2000, [&calls](Cpu*) { calls++; return true; });
        cpu.step();
        cpu.step();
        auto r = cpu.get_registers();
        return calls == 0 && r.a == 2 && r.pc == 0x1002;
    });

    add_test("trap: fires again every time PC returns to the address", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x00});
        int calls = 0;
        cpu.set_trap(0x1000, [&calls](Cpu* c) {
            calls++;
            c->set_pc(0x1000);                  // stay put
            return true;
        });
        for (int i = 0; i < 5; i++) cpu.step();
        return calls == 5;
    });

    // ===================================================== set/clear/query ==

    add_test("trap: has_trap reflects set and clear", [] {
        Cpu cpu; cpu.reset();
        if (cpu.has_trap(0x1234)) return false;
        cpu.set_trap(0x1234, [](Cpu*) { return true; });
        if (!cpu.has_trap(0x1234)) return false;
        cpu.clear_trap(0x1234);
        return !cpu.has_trap(0x1234);
    });

    add_test("trap: clear_trap stops it firing", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x3C});
        int calls = 0;
        cpu.set_trap(0x1000, [&calls](Cpu*) { calls++; return true; });
        cpu.clear_trap(0x1000);
        cpu.step();
        auto r = cpu.get_registers();
        return calls == 0 && r.a == 1;
    });

    add_test("trap: clear_trap on an address with no trap is harmless", [] {
        Cpu cpu; cpu.reset();
        cpu.clear_trap(0x4321);
        return !cpu.has_trap(0x4321);
    });

    add_test("trap: clear_all_traps removes every trap", [] {
        Cpu cpu; cpu.reset();
        for (word a = 0x1000; a < 0x1010; a++) {
            cpu.set_trap(a, [](Cpu*) { return true; });
        }
        cpu.clear_all_traps();
        for (word a = 0x1000; a < 0x1010; a++) {
            if (cpu.has_trap(a)) return false;
        }
        return true;
    });

    add_test("trap: setting the same address twice replaces the callback", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x00});
        int first = 0, second = 0;
        cpu.set_trap(0x1000, [&first](Cpu* c) { first++; c->set_pc(0x2000); return true; });
        cpu.set_trap(0x1000, [&second](Cpu* c) { second++; c->set_pc(0x3000); return true; });
        cpu.step();
        return first == 0 && second == 1 && cpu.get_registers().pc == 0x3000;
    });

    add_test("trap: several traps coexist at different addresses", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x00, 0x00, 0x00});
        std::vector<word> hits;
        for (word a = 0x1000; a <= 0x1002; a++) {
            cpu.set_trap(a, [&hits, a](Cpu* c) {
                hits.push_back(a);
                c->set_pc(static_cast<word>(a + 1));
                return true;
            });
        }
        for (int i = 0; i < 3; i++) cpu.step();
        return hits == std::vector<word>{0x1000, 0x1001, 0x1002};
    });

    // ======================================================== address edges ==

    add_test("trap: works at address 0x0000", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x0000, {0x3C});
        bool called = false;
        cpu.set_trap(0x0000, [&called](Cpu* c) {
            called = true; c->set_pc(0x0100); return true;
        });
        cpu.step();
        return called && cpu.get_registers().a == 0;
    });

    add_test("trap: works at the top address 0xFFFF", [] {
        // The trap bitmap must span the whole 16-bit space or this address
        // would be out of bounds.
        Cpu cpu; cpu.reset();
        RegisterState r{};
        r.pc = 0xFFFF;
        r.sp = 0x8000;
        cpu.set_registers(r);
        bool called = false;
        cpu.set_trap(0xFFFF, [&called](Cpu* c) {
            called = true; c->set_pc(0x0000); return true;
        });
        cpu.step();
        return called && cpu.get_registers().pc == 0x0000;
    });

    // =================================== standing in for a CALLed routine ==

    add_test("trap: pop_word returns the address pushed by CALL", [] {
        // CALL 0x2000 at 0x1000, with a trap on the callee. The trap must see
        // 0x1003 (the instruction after the CALL) on top of the stack.
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0xCD, 0x00, 0x20});   // CALL 0x2000
        word seen = 0;
        cpu.set_trap(0x2000, [&seen](Cpu* c) {
            seen = c->pop_word();
            c->set_pc(seen);                      // act as RET
            return true;
        });
        cpu.step();                               // the CALL
        if (cpu.get_registers().pc != 0x2000) return false;
        cpu.step();                               // the trap
        auto r = cpu.get_registers();
        return seen == 0x1003 && r.pc == 0x1003 && r.sp == 0x8000;
    });

    add_test("trap: acting as RET leaves SP where the real RET would", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0xCD, 0x00, 0x20});
        cpu.set_trap(0x2000, [](Cpu* c) { c->set_pc(c->pop_word()); return true; });
        cpu.step();
        const word sp_in_call = cpu.get_registers().sp;
        cpu.step();
        return sp_in_call == 0x7FFE && cpu.get_registers().sp == 0x8000;
    });

    add_test("trap: push_word is visible to the emulated code", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x00});
        cpu.set_trap(0x1000, [](Cpu* c) {
            c->push_word(0xBEEF);
            c->set_pc(0x1001);
            return true;
        });
        cpu.step();
        auto r = cpu.get_registers();
        return r.sp == 0x7FFE
            && cpu.get_memory()->read(0x7FFE) == 0xEF
            && cpu.get_memory()->read(0x7FFF) == 0xBE;
    });

    add_test("trap: a trap can set the carry flag, as LD-BYTES does on success", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0xCD, 0x56, 0x05});   // CALL LD-BYTES
        cpu.set_trap(0x0556, [](Cpu* c) {
            auto r = c->get_registers();
            r.f |= TRAP_FLAG_C;
            r.pc = 0;                             // overwritten below
            c->set_registers(r);
            c->set_pc(c->pop_word());
            return true;
        });
        cpu.step();
        cpu.step();
        auto r = cpu.get_registers();
        return (r.f & TRAP_FLAG_C) != 0 && r.pc == 0x1003;
    });

    add_test("trap: a trap can move a block of bytes into memory", [] {
        // The shape the LD-BYTES hook will use: IX = destination, DE = count.
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x00});
        RegisterState init{};
        init.pc = 0x1000; init.sp = 0x8000;
        init.ix = 0x6000; init.de = 4;
        cpu.set_registers(init);
        cpu.set_trap(0x1000, [](Cpu* c) {
            auto r = c->get_registers();
            byte* mem = c->get_memory()->data();
            for (word i = 0; i < r.de; i++) {
                mem[r.ix + i] = static_cast<byte>(0xA0 + i);
            }
            r.ix = static_cast<word>(r.ix + r.de);
            r.de = 0;
            c->set_registers(r);
            c->set_pc(0x1001);
            return true;
        });
        cpu.step();
        auto r = cpu.get_registers();
        return cpu.get_memory()->read(0x6000) == 0xA0
            && cpu.get_memory()->read(0x6003) == 0xA3
            && r.ix == 0x6004 && r.de == 0;
    });

    add_test("trap: a declining trap lets the real ROM routine run", [] {
        // The fallback path: RET at the trapped address must still execute.
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0xCD, 0x00, 0x20});
        cpu.get_memory()->data()[0x2000] = 0xC9;   // RET
        int consulted = 0;
        cpu.set_trap(0x2000, [&consulted](Cpu*) { consulted++; return false; });
        cpu.step();                                // CALL
        cpu.step();                                // trap declines -> real RET
        auto r = cpu.get_registers();
        return consulted == 1 && r.pc == 0x1003 && r.sp == 0x8000;
    });

    // ====================================================== T-state charge ==

    add_test("trap: charges its configured T-states", [] {
        // Observable through the frame callback: a single trap costing a whole
        // frame's worth of T-states must end the frame.
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x00});
        int frames = 0;
        cpu.set_frame_callback([&frames](Cpu*) { frames++; });
        cpu.set_trap(0x1000, [](Cpu* c) { c->set_pc(0x1000); return true; },
                     CYCLES_PER_FRAME);
        cpu.step();
        return frames == 1;
    });

    add_test("trap: a cheap trap does not end the frame", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x00});
        int frames = 0;
        cpu.set_frame_callback([&frames](Cpu*) { frames++; });
        cpu.set_trap(0x1000, [](Cpu* c) { c->set_pc(0x1000); return true; }, 4);
        for (int i = 0; i < 100; i++) cpu.step();
        return frames == 0;
    });

    add_test("trap: a declined trap is charged as the real instruction", [] {
        // Declining must not double-charge or mis-charge the cycle budget.
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x00});                 // NOP, 4 T-states
        int frames = 0;
        cpu.set_frame_callback([&frames](Cpu*) { frames++; });
        cpu.set_trap(0x1000, [](Cpu* c) { c->set_pc(0x1000); return false; },
                     CYCLES_PER_FRAME);
        cpu.step();
        // The expensive trap cost must not be applied when it declines.
        return frames == 0;
    });

    // ============================================================== halted ==

    add_test("trap: does not fire while the CPU is halted", [] {
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x76});                 // HALT
        cpu.step();
        if (!cpu.get_registers().halted) return false;
        int calls = 0;
        cpu.set_trap(cpu.get_registers().pc, [&calls](Cpu*) { calls++; return true; });
        for (int i = 0; i < 10; i++) cpu.step();
        return calls == 0;
    });

    // ================================================= interaction with INT ==

    add_test("trap: an interrupt can vector into a trapped address", [] {
        // The ISR entry itself is trappable, which is the same mechanism the
        // tape hook relies on.
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0x00});
        RegisterState init{};
        init.pc = 0x1000; init.sp = 0x8000; init.iff1 = true; init.im = 1;
        cpu.set_registers(init);
        bool called = false;
        cpu.set_trap(0x0038, [&called](Cpu* c) {
            called = true;
            c->set_pc(c->pop_word());
            return true;
        });
        cpu.interrupt();
        if (cpu.get_registers().pc != 0x0038) return false;
        cpu.step();
        return called && cpu.get_registers().pc == 0x1000;
    });

    // ============================================== the real ROM trap sites ==

    add_test("trap: LD-BYTES at 0x0556 is reachable via the ROM's own CALL", [] {
        // LD-BLOCK at 0x0802 is `CALL 0x0556; RET C`. Running it with a trap
        // on LD-BYTES is exactly what milestone 3 will do.
        Cpu cpu; cpu.reset();
        byte* mem = cpu.get_memory()->data();
        mem[0x0802] = 0xCD; mem[0x0803] = 0x56; mem[0x0804] = 0x05;  // CALL 0x0556
        mem[0x0805] = 0xD8;                                          // RET C
        poke_word(cpu, 0x7FFE, 0x9000);            // where the outer RET goes

        RegisterState init{};
        init.pc = 0x0802; init.sp = 0x7FFE;
        cpu.set_registers(init);

        bool called = false;
        cpu.set_trap(0x0556, [&called](Cpu* c) {
            called = true;
            auto r = c->get_registers();
            r.f |= TRAP_FLAG_C;                    // report success
            c->set_registers(r);
            c->set_pc(c->pop_word());
            return true;
        });

        cpu.step();                                 // CALL 0x0556
        cpu.step();                                 // trap fires, returns
        if (!called || cpu.get_registers().pc != 0x0805) return false;
        cpu.step();                                 // RET C -- carry is set
        return cpu.get_registers().pc == 0x9000;
    });

    add_test("trap: SA-BYTES at 0x04C2 traps before the routine's own PUSH", [] {
        // SA-BYTES begins `LD HL,$053F; PUSH HL`. Trapping the first
        // instruction means that push never happens, so the stack top is still
        // the caller's return address.
        Cpu cpu; cpu.reset();
        place(cpu, 0x1000, {0xCD, 0xC2, 0x04});     // CALL 0x04C2
        word seen = 0;
        cpu.set_trap(0x04C2, [&seen](Cpu* c) {
            seen = c->pop_word();
            c->set_pc(seen);
            return true;
        });
        cpu.step();
        cpu.step();
        auto r = cpu.get_registers();
        return seen == 0x1003 && r.pc == 0x1003 && r.sp == 0x8000;
    });
}
