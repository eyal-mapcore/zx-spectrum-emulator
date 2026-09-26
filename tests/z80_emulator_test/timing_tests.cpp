// The free-running T-state clock (milestone 1 of the beeper work).
//
// Sound on a Spectrum is one bit toggled at a rate the software chooses, so
// the pitch falls straight out of instruction timing. This clock is what will
// timestamp those toggles, and the spot checks at the bottom are the first
// thing in this project to exercise states() at all.

#include "z80/z80_cpu.h"
#include "test_harness.h"

#include <string>
#include <vector>

using testing::add_test;
using z80::Cpu;
using z80::RegisterState;

namespace {

/// Places `bytes` at `addr`, points PC there, and runs one instruction.
/// Returns the T-states the clock charged for it.
uint64_t cost_of(std::initializer_list<byte> bytes, RegisterState initial = {}) {
    Cpu cpu;
    cpu.reset();
    byte* mem = cpu.get_memory()->data();
    word i = 0x8000;
    for (byte b : bytes) mem[i++] = b;
    initial.pc = 0x8000;
    if (initial.sp == 0) initial.sp = 0xFF00;
    cpu.set_registers(initial);

    const uint64_t before = cpu.total_cycles();
    cpu.step();
    return cpu.total_cycles() - before;
}

} // namespace

void register_timing_tests() {

    // ============================================================ the clock ==

    add_test("clock: starts at zero", [] {
        Cpu cpu;
        return cpu.total_cycles() == 0;
    });

    add_test("clock: a NOP advances it", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[0x8000] = 0x00;
        RegisterState init{}; init.pc = 0x8000; init.sp = 0xFF00;
        cpu.set_registers(init);
        cpu.step();
        return cpu.total_cycles() == 4;
    });

    add_test("clock: never goes backwards over many instructions", [] {
        Cpu cpu; cpu.reset();
        byte* mem = cpu.get_memory()->data();
        for (int i = 0; i < 64; i++) mem[0x8000 + i] = 0x00;   // NOPs
        RegisterState init{}; init.pc = 0x8000; init.sp = 0xFF00;
        cpu.set_registers(init);

        uint64_t last = cpu.total_cycles();
        for (int i = 0; i < 64; i++) {
            cpu.step();
            const uint64_t now = cpu.total_cycles();
            if (now <= last) return false;
            last = now;
        }
        return last == 64 * 4;
    });

    add_test("clock: reset() does not rewind it", [] {
        // The contract a timestamp consumer relies on. reset() clears the
        // frame budget, but the clock has to keep running.
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[0x8000] = 0x00;
        RegisterState init{}; init.pc = 0x8000; init.sp = 0xFF00;
        cpu.set_registers(init);
        cpu.step();
        const uint64_t before = cpu.total_cycles();
        if (before == 0) return false;
        cpu.reset();
        return cpu.total_cycles() == before;
    });

    add_test("clock: HALT keeps it running at 4 T-states a step", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[0x8000] = 0x76;    // HALT
        RegisterState init{}; init.pc = 0x8000; init.sp = 0xFF00;
        cpu.set_registers(init);
        cpu.step();                                  // executes the HALT
        const uint64_t after_halt = cpu.total_cycles();
        for (int i = 0; i < 10; i++) cpu.step();     // spinning in HALT
        return cpu.get_registers().halted
            && cpu.total_cycles() == after_halt + 10 * 4;
    });

    add_test("clock: a trap is charged its configured cost", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[0x8000] = 0x00;
        RegisterState init{}; init.pc = 0x8000; init.sp = 0xFF00;
        cpu.set_registers(init);
        cpu.set_trap(0x8000, [](Cpu* c) { c->set_pc(0x8001); return true; }, 250);
        cpu.step();
        return cpu.total_cycles() == 250;
    });

    add_test("clock: a declined trap is charged the real instruction instead", [] {
        Cpu cpu; cpu.reset();
        cpu.get_memory()->data()[0x8000] = 0x00;    // NOP, 4 T-states
        RegisterState init{}; init.pc = 0x8000; init.sp = 0xFF00;
        cpu.set_registers(init);
        cpu.set_trap(0x8000, [](Cpu*) { return false; }, 250);
        cpu.step();
        return cpu.total_cycles() == 4;
    });

    add_test("clock: an accepted interrupt is charged 13 T-states in IM 1", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.pc = 0x8000; init.sp = 0xFF00;
        init.iff1 = true; init.im = 1;
        cpu.set_registers(init);
        const uint64_t before = cpu.total_cycles();
        const int charged = cpu.interrupt();
        // interrupt() reports its cost; step()'s frame boundary is what adds
        // it to the clock, so calling it directly must not move the clock.
        return charged == 13 && cpu.total_cycles() == before;
    });

    // ===================================================== frame boundaries ==

    add_test("clock: survives the frame boundary subtracting a frame", [] {
        // end_of_frame() does cycle_count_ -= CYCLES_PER_FRAME. If that leaked
        // into the free-running clock it would stall at one frame's worth.
        Cpu cpu; cpu.reset();
        byte* mem = cpu.get_memory()->data();
        for (int i = 0; i < 256; i++) mem[0x8000 + i] = 0x00;
        mem[0x80FF] = 0xC3; mem[0x8100] = 0x00; mem[0x8101] = 0x80;  // JP 0x8000

        RegisterState init{}; init.pc = 0x8000; init.sp = 0xFF00;
        cpu.set_registers(init);
        cpu.set_realtime(false);

        int frames = 0;
        cpu.set_frame_callback([&frames](Cpu*) { frames++; });
        while (frames < 2) cpu.step();

        // Two frames of T-states, plus each frame's interrupt acknowledge and
        // at most one instruction of overshoot.
        const uint64_t two_frames = 2ull * CYCLES_PER_FRAME;
        return cpu.total_cycles() >= two_frames
            && cpu.total_cycles() < two_frames + 100;
    });

    add_test("clock: matches the frame budget within one instruction", [] {
        Cpu cpu; cpu.reset();
        byte* mem = cpu.get_memory()->data();
        for (int i = 0; i < 256; i++) mem[0x8000 + i] = 0x00;
        mem[0x80FF] = 0xC3; mem[0x8100] = 0x00; mem[0x8101] = 0x80;
        RegisterState init{}; init.pc = 0x8000; init.sp = 0xFF00;
        cpu.set_registers(init);
        cpu.set_realtime(false);

        int frames = 0;
        cpu.set_frame_callback([&frames](Cpu*) { frames++; });
        while (frames < 1) cpu.step();
        // One frame elapsed, so the clock should read about CYCLES_PER_FRAME.
        const uint64_t t = cpu.total_cycles();
        return t >= CYCLES_PER_FRAME && t < CYCLES_PER_FRAME + 50;
    });

    // ================================== first spot checks of states() itself ==
    //
    // Pitch accuracy depends entirely on these being right, so a handful of
    // instructions with unambiguous published timings are worth pinning now.

    add_test("states: NOP is 4", []          { return cost_of({0x00}) == 4; });
    add_test("states: LD A,n is 7", []       { return cost_of({0x3E, 0x42}) == 7; });
    add_test("states: INC HL is 6", []       { return cost_of({0x23}) == 6; });
    add_test("states: INC A is 4", []        { return cost_of({0x3C}) == 4; });
    add_test("states: JP nn is 10", []       { return cost_of({0xC3, 0x00, 0x90}) == 10; });
    add_test("states: CALL nn is 17", []     { return cost_of({0xCD, 0x00, 0x90}) == 17; });
    add_test("states: RET is 10", []         { return cost_of({0xC9}) == 10; });
    add_test("states: PUSH BC is 11", []     { return cost_of({0xC5}) == 11; });
    add_test("states: POP BC is 10", []      { return cost_of({0xC1}) == 10; });
    add_test("states: OUT (n),A is 11", []   { return cost_of({0xD3, 0xFE}) == 11; });
    add_test("states: IN A,(n) is 11", []    { return cost_of({0xDB, 0xFE}) == 11; });
    add_test("states: EX (SP),HL is 19", []  { return cost_of({0xE3}) == 19; });

    add_test("states: LD (HL),A is 7", [] {
        RegisterState init{}; init.hl = 0x9000;
        return cost_of({0x77}, init) == 7;
    });

    add_test("states: DJNZ is 13 taken, 8 not taken", [] {
        RegisterState taken{};     taken.bc = 0x0500;      // B = 5
        RegisterState not_taken{}; not_taken.bc = 0x0100;  // B = 1 -> falls through
        return cost_of({0x10, 0x02}, taken) == 13
            && cost_of({0x10, 0x02}, not_taken) == 8;
    });

    add_test("states: JR is 12 taken, 7 not taken", [] {
        RegisterState zero_set{};   zero_set.f = 0x40;     // Z set
        RegisterState zero_clear{}; zero_clear.f = 0x00;
        // JR Z,d
        return cost_of({0x28, 0x02}, zero_set) == 12
            && cost_of({0x28, 0x02}, zero_clear) == 7;
    });

    // The block-repeat forms share DJNZ's hazard: their cost depends on a
    // counter they themselves modify, so reading it after execution gives the
    // answer for the *next* iteration.

    add_test("states: LDIR is 21 repeating, 16 on the last transfer", [] {
        RegisterState more{}; more.hl = 0x9000; more.de = 0x9100; more.bc = 5;
        RegisterState last{}; last.hl = 0x9000; last.de = 0x9100; last.bc = 1;
        return cost_of({0xED, 0xB0}, more) == 21
            && cost_of({0xED, 0xB0}, last) == 16;
    });

    add_test("states: LDDR is 21 repeating, 16 on the last transfer", [] {
        RegisterState more{}; more.hl = 0x9000; more.de = 0x9100; more.bc = 5;
        RegisterState last{}; last.hl = 0x9000; last.de = 0x9100; last.bc = 1;
        return cost_of({0xED, 0xB8}, more) == 21
            && cost_of({0xED, 0xB8}, last) == 16;
    });

    add_test("states: CPIR is 16 once it finds a match", [] {
        // Memory is zeroed, so A = 0 matches at once and A = 0xFF does not.
        RegisterState hunting{}; hunting.hl = 0x9000; hunting.bc = 5; hunting.a = 0xFF;
        RegisterState found{};   found.hl   = 0x9000; found.bc   = 5; found.a   = 0x00;
        RegisterState spent{};   spent.hl   = 0x9000; spent.bc   = 1; spent.a   = 0xFF;
        return cost_of({0xED, 0xB1}, hunting) == 21
            && cost_of({0xED, 0xB1}, found)   == 16
            && cost_of({0xED, 0xB1}, spent)   == 16;
    });

    add_test("states: INIR is 21 repeating, 16 on the last byte", [] {
        RegisterState more{}; more.hl = 0x9000; more.bc = 0x0500;   // B = 5
        RegisterState last{}; last.hl = 0x9000; last.bc = 0x0100;   // B = 1
        return cost_of({0xED, 0xB2}, more) == 21
            && cost_of({0xED, 0xB2}, last) == 16;
    });

    add_test("states: OTIR is 21 repeating, 16 on the last byte", [] {
        RegisterState more{}; more.hl = 0x9000; more.bc = 0x0500;
        RegisterState last{}; last.hl = 0x9000; last.bc = 0x0100;
        return cost_of({0xED, 0xB3}, more) == 21
            && cost_of({0xED, 0xB3}, last) == 16;
    });

    add_test("states: the non-repeating block forms are a flat 16", [] {
        RegisterState r{}; r.hl = 0x9000; r.de = 0x9100; r.bc = 5;
        return cost_of({0xED, 0xA0}, r) == 16      // LDI
            && cost_of({0xED, 0xA8}, r) == 16      // LDD
            && cost_of({0xED, 0xA1}, r) == 16      // CPI
            && cost_of({0xED, 0xA2}, r) == 16;     // INI
    });

    add_test("states: conditional RET is 11 taken, 5 not taken", [] {
        RegisterState zero_set{};   zero_set.f = 0x40;
        RegisterState zero_clear{}; zero_clear.f = 0x00;
        // RET Z
        return cost_of({0xC8}, zero_set) == 11
            && cost_of({0xC8}, zero_clear) == 5;
    });

    add_test("states: every conditional RET and CALL times both branches", [] {
        // Flag patterns that make each condition true, and that make it false.
        struct Cond { byte true_f, false_f; };
        const Cond conds[8] = {
            { 0x00, 0x40 },   // NZ: Z clear
            { 0x40, 0x00 },   // Z
            { 0x00, 0x01 },   // NC: C clear
            { 0x01, 0x00 },   // C
            { 0x00, 0x04 },   // PO: P/V clear
            { 0x04, 0x00 },   // PE
            { 0x00, 0x80 },   // P: S clear
            { 0x80, 0x00 },   // M
        };
        for (int cc = 0; cc < 8; cc++) {
            RegisterState yes{}; yes.f = conds[cc].true_f;
            RegisterState no{};  no.f  = conds[cc].false_f;

            const byte ret_op  = static_cast<byte>(0xC0 + cc * 8);
            const byte call_op = static_cast<byte>(0xC4 + cc * 8);
            if (cost_of({ret_op}, yes) != 11) return false;
            if (cost_of({ret_op}, no)  != 5)  return false;
            if (cost_of({call_op, 0x00, 0x90}, yes) != 17) return false;
            if (cost_of({call_op, 0x00, 0x90}, no)  != 10) return false;

            // JP cc is 10 either way -- no variable timing to get wrong.
            const byte jp_op = static_cast<byte>(0xC2 + cc * 8);
            if (cost_of({jp_op, 0x00, 0x90}, yes) != 10) return false;
            if (cost_of({jp_op, 0x00, 0x90}, no)  != 10) return false;
        }
        return true;
    });

    add_test("states: every conditional JR times both branches", [] {
        // Only four of the eight conditions have a JR form.
        struct Cond { byte op, true_f, false_f; };
        const Cond conds[4] = {
            { 0x20, 0x00, 0x40 },   // JR NZ
            { 0x28, 0x40, 0x00 },   // JR Z
            { 0x30, 0x00, 0x01 },   // JR NC
            { 0x38, 0x01, 0x00 },   // JR C
        };
        for (const Cond& c : conds) {
            RegisterState yes{}; yes.f = c.true_f;
            RegisterState no{};  no.f  = c.false_f;
            if (cost_of({c.op, 0x02}, yes) != 12) return false;
            if (cost_of({c.op, 0x02}, no)  != 7)  return false;
        }
        return true;
    });

    // ======================================= whole-page structural sweeps ==
    //
    // Rather than a hand-typed table of 1792 timings -- the sort of thing that
    // produced the errors in the first place -- these assert the relationships
    // the pages are built on, which catches a whole class at a time.

    add_test("states: every CB-page opcode follows the 8 / 15 / 12 pattern", [] {
        for (int op = 0; op <= 0xFF; op++) {
            RegisterState r{}; r.hl = 0x9000;
            const uint64_t c = cost_of({0xCB, static_cast<byte>(op)}, r);
            uint64_t expected;
            if ((op & 7) != 6)                    expected = 8;   // register
            else if (op >= 0x40 && op <= 0x7F)    expected = 12;  // BIT b,(HL)
            else                                  expected = 15;  // rot/res/set
            if (c != expected) return false;
        }
        return true;
    });

    add_test("states: every DD CB opcode is 20 for BIT and 23 otherwise", [] {
        for (int op = 0; op <= 0xFF; op++) {
            RegisterState r{}; r.ix = 0x9000;
            const uint64_t c = cost_of({0xDD, 0xCB, 0x02, static_cast<byte>(op)}, r);
            const uint64_t expected = (op >= 0x40 && op <= 0x7F) ? 20 : 23;
            if (c != expected) return false;
        }
        return true;
    });

    add_test("states: an ignored DD prefix costs exactly 4 more than the base", [] {
        // Regression: the indexed pages used to read a flat table here, so a
        // DD before a conditional always charged the not-taken cost -- DD C0
        // came out 9 whether or not the RET was taken.
        for (int op = 0; op <= 0xFF; op++) {
            if (op == 0xCB || op == 0xDD || op == 0xED || op == 0xFD) continue;

            // Skip anything that actually uses the (HL) slot: those become
            // (IX+d) forms and cost much more than the prefix alone.
            bool hl_slot = false;
            if (op >= 0x40 && op <= 0x7F)      hl_slot = ((op & 7) == 6) || (((op >> 3) & 7) == 6);
            else if (op >= 0x80 && op <= 0xBF) hl_slot = ((op & 7) == 6);
            else if ((op & 0xC7) == 0x04 || (op & 0xC7) == 0x05 || (op & 0xC7) == 0x06)
                                               hl_slot = (((op >> 3) & 7) == 6);
            if (hl_slot) continue;

            RegisterState r{};
            r.hl = 0x9000; r.ix = 0x9000; r.iy = 0x9000; r.bc = 0x0303;
            const uint64_t base = cost_of({static_cast<byte>(op), 0x00, 0x00}, r);
            const uint64_t dd   = cost_of({0xDD, static_cast<byte>(op), 0x00, 0x00}, r);
            if (dd != base + 4) return false;
        }
        return true;
    });
}
