#include "z80/z80_cpu.h"
#include "test_harness.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using z80::Cpu;
using z80::RegisterState;
using testing::add_test;
using testing::tests;

namespace {

constexpr byte FLAG_C  = 0x01;
constexpr byte FLAG_N  = 0x02;
constexpr byte FLAG_PV = 0x04;
constexpr byte FLAG_H  = 0x10;
constexpr byte FLAG_Z  = 0x40;
constexpr byte FLAG_S  = 0x80;

word make_word(byte lo, byte hi) { return static_cast<word>(lo) | (static_cast<word>(hi) << 8); }
byte lo_byte(word w) { return static_cast<byte>(w & 0xFF); }
byte hi_byte(word w) { return static_cast<byte>((w >> 8) & 0xFF); }

// Writes `bytes` at `addr`, applies `initial` as the starting register state
// (pc forced to addr), executes exactly one instruction via step(), and
// returns the resulting register snapshot.
RegisterState run(Cpu& cpu, word addr, std::initializer_list<byte> bytes, RegisterState initial) {
    initial.pc = addr;
    cpu.set_registers(initial);
    byte* mem = cpu.get_memory()->data();
    word i = addr;
    for (byte b : bytes) mem[i++] = b;
    cpu.step();
    return cpu.get_registers();
}

std::string to_hex(byte v) {
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02X", v);
    return buf;
}

struct RegSlot { const char* name; uint8_t code; };
const RegSlot kRegSlots[8] = {
    {"B", 0}, {"C", 1}, {"D", 2}, {"E", 3}, {"H", 4}, {"L", 5}, {"(HL)", 6}, {"A", 7},
};

// Sets an 8-bit operand slot (matching production resolve_r's
// {B,C,D,E,H,L,(HL),A} ordering) within a RegisterState. code==6 ((HL)) is
// not handled here -- callers write memory directly for that slot.
void set_reg_by_slot(RegisterState& regs, uint8_t code, byte value) {
    switch (code & 0x07) {
        case 0: regs.bc = make_word(lo_byte(regs.bc), value); break;
        case 1: regs.bc = make_word(value, hi_byte(regs.bc)); break;
        case 2: regs.de = make_word(lo_byte(regs.de), value); break;
        case 3: regs.de = make_word(value, hi_byte(regs.de)); break;
        case 4: regs.hl = make_word(lo_byte(regs.hl), value); break;
        case 5: regs.hl = make_word(value, hi_byte(regs.hl)); break;
        case 7: regs.a = value; break;
        default: break;
    }
}

byte get_reg_by_slot(const RegisterState& regs, Cpu& cpu, uint8_t code) {
    switch (code & 0x07) {
        case 0: return hi_byte(regs.bc);
        case 1: return lo_byte(regs.bc);
        case 2: return hi_byte(regs.de);
        case 3: return lo_byte(regs.de);
        case 4: return hi_byte(regs.hl);
        case 5: return lo_byte(regs.hl);
        case 6: return cpu.get_memory()->read(regs.hl);
        default: return regs.a;
    }
}

// ---- CB page: BIT/RES/SET (0x40-0xFF, 192 combinations) -------------------
void add_bit_res_set_tests() {
    for (const auto& slot : kRegSlots) {
        for (uint8_t bit = 0; bit < 8; ++bit) {
            byte opBit = static_cast<byte>(0x40 | (bit << 3) | slot.code);
            byte opRes = static_cast<byte>(0x80 | (bit << 3) | slot.code);
            byte opSet = static_cast<byte>(0xC0 | (bit << 3) | slot.code);
            int bitNum = bit;

            add_test("CB" + to_hex(opBit) + " BIT " + std::to_string(bitNum) + "," + slot.name,
                [slot, bitNum, opBit] {
                    Cpu cpu; cpu.reset();
                    RegisterState init{};
                    init.f = FLAG_C; // must be preserved
                    byte testValue = static_cast<byte>(~(1 << bitNum));
                    if (slot.code == 6) {
                        init.hl = 0x9000;
                        cpu.set_registers(init);
                        cpu.get_memory()->data()[0x9000] = testValue;
                    } else {
                        set_reg_by_slot(init, slot.code, testValue);
                    }
                    auto r = run(cpu, 0, {0xCB, opBit}, init);
                    return (r.f & FLAG_Z) && (r.f & FLAG_H) && !(r.f & FLAG_N) && (r.f & FLAG_C);
                });

            add_test("CB" + to_hex(opRes) + " RES " + std::to_string(bitNum) + "," + slot.name,
                [slot, bitNum, opRes] {
                    Cpu cpu; cpu.reset();
                    RegisterState init{};
                    byte testValue = 0xFF;
                    if (slot.code == 6) {
                        init.hl = 0x9000;
                        cpu.set_registers(init);
                        cpu.get_memory()->data()[0x9000] = testValue;
                    } else {
                        set_reg_by_slot(init, slot.code, testValue);
                    }
                    auto r = run(cpu, 0, {0xCB, opRes}, init);
                    byte expected = static_cast<byte>(~(1 << bitNum));
                    if (slot.code == 6) return cpu.get_memory()->read(0x9000) == expected;
                    return get_reg_by_slot(r, cpu, slot.code) == expected;
                });

            add_test("CB" + to_hex(opSet) + " SET " + std::to_string(bitNum) + "," + slot.name,
                [slot, bitNum, opSet] {
                    Cpu cpu; cpu.reset();
                    RegisterState init{};
                    byte testValue = 0x00;
                    if (slot.code == 6) {
                        init.hl = 0x9000;
                        cpu.set_registers(init);
                        cpu.get_memory()->data()[0x9000] = testValue;
                    } else {
                        set_reg_by_slot(init, slot.code, testValue);
                    }
                    auto r = run(cpu, 0, {0xCB, opSet}, init);
                    byte expected = static_cast<byte>(1 << bitNum);
                    if (slot.code == 6) return cpu.get_memory()->read(0x9000) == expected;
                    return get_reg_by_slot(r, cpu, slot.code) == expected;
                });
        }
    }
}

// ---- CB page: remaining rotate/shift families (SLA (HL)/A, SRA, SLL, SRL) --
void add_cb_shift_family_tests() {
    struct ShiftOp { const char* name; byte base; byte expected; };
    // Input 0x81 (1000 0001) exercises both ends of the byte for every family.
    const ShiftOp ops[4] = {
        {"SLA", 0x20, 0x02},
        {"SRA", 0x28, 0xC0},
        {"SLL", 0x30, 0x03},
        {"SRL", 0x38, 0x40},
    };
    for (const auto& op : ops) {
        for (const auto& slot : kRegSlots) {
            byte opcode = static_cast<byte>(op.base | slot.code);
            if (opcode >= 0x20 && opcode <= 0x25) continue; // already hand-tested above

            byte expected = op.expected;
            add_test("CB" + to_hex(opcode) + " " + op.name + " " + slot.name,
                [slot, opcode, expected] {
                    Cpu cpu; cpu.reset();
                    RegisterState init{};
                    byte testValue = 0x81;
                    if (slot.code == 6) {
                        init.hl = 0x9000;
                        cpu.set_registers(init);
                        cpu.get_memory()->data()[0x9000] = testValue;
                    } else {
                        set_reg_by_slot(init, slot.code, testValue);
                    }
                    auto r = run(cpu, 0, {0xCB, opcode}, init);
                    byte actual = (slot.code == 6) ? cpu.get_memory()->read(0x9000)
                                                    : get_reg_by_slot(r, cpu, slot.code);
                    return actual == expected && (r.f & FLAG_C);
                });
        }
    }
}

// ---- Base page: LD r,r' (0x40-0x7F excluding 0x76=HALT, 63 combinations) --
void add_ld_rr_tests() {
    for (const auto& dst : kRegSlots) {
        for (const auto& src : kRegSlots) {
            byte opcode = static_cast<byte>(0x40 | (dst.code << 3) | src.code);
            if (opcode == 0x76) continue; // HALT, not LD (HL),(HL)

            add_test("0x" + to_hex(opcode) + " LD " + dst.name + "," + src.name,
                [dst, src, opcode] {
                    Cpu cpu; cpu.reset();
                    RegisterState init{};
                    byte value = 0x5A;
                    if (dst.code == 6 || src.code == 6) init.hl = 0x9000;
                    // dst==(HL) and src==H/L share storage with the address
                    // itself, so the transferred value isn't independently
                    // choosable -- it's whichever byte of 0x9000 that is.
                    bool addrConflict = (dst.code == 6) && (src.code == 4 || src.code == 5);
                    if (src.code == 6) {
                        cpu.set_registers(init);
                        cpu.get_memory()->data()[0x9000] = value;
                    } else if (!addrConflict) {
                        set_reg_by_slot(init, src.code, value);
                    }
                    if (addrConflict) {
                        value = (src.code == 4) ? hi_byte(init.hl) : lo_byte(init.hl);
                    }
                    auto r = run(cpu, 0, {opcode}, init);
                    if (dst.code == 6) return cpu.get_memory()->read(0x9000) == value;
                    return get_reg_by_slot(r, cpu, dst.code) == value;
                });
        }
    }
}

// ---- Base page: 8-bit ALU-with-A (0x80-0xBF, 64 combinations) -------------
struct AluExpected { byte result; bool z, s, h, pv, n, c; };

AluExpected reference_alu(int op, byte a, byte b, bool carryIn) {
    int result = 0;
    bool bCarry = false, bHalf = false, bOverflow = false, bSub = false;
    switch (op) {
        case 0: result = a + b; bCarry = result > 0xFF; bHalf = ((a & 0xF) + (b & 0xF)) > 0xF; bOverflow = (~(a ^ b) & (a ^ result) & 0x80) != 0; break;
        case 1: result = a + b + (carryIn ? 1 : 0); bCarry = result > 0xFF; bHalf = ((a & 0xF) + (b & 0xF) + (carryIn ? 1 : 0)) > 0xF; bOverflow = (~(a ^ b) & (a ^ result) & 0x80) != 0; break;
        case 2: result = a - b; bCarry = result < 0; bHalf = ((a & 0xF) - (b & 0xF)) < 0; bOverflow = ((a ^ b) & (a ^ result) & 0x80) != 0; bSub = true; break;
        case 3: result = a - b - (carryIn ? 1 : 0); bCarry = result < 0; bHalf = ((a & 0xF) - (b & 0xF) - (carryIn ? 1 : 0)) < 0; bOverflow = ((a ^ b) & (a ^ result) & 0x80) != 0; bSub = true; break;
        case 4: result = a & b; bHalf = true; bOverflow = !__builtin_parity(static_cast<byte>(result)); break;
        case 5: result = a ^ b; bOverflow = !__builtin_parity(static_cast<byte>(result)); break;
        case 6: result = a | b; bOverflow = !__builtin_parity(static_cast<byte>(result)); break;
        case 7: result = a - b; bCarry = result < 0; bHalf = ((a & 0xF) - (b & 0xF)) < 0; bOverflow = ((a ^ b) & (a ^ result) & 0x80) != 0; bSub = true; break;
    }
    byte br = static_cast<byte>(result & 0xFF);
    AluExpected e;
    e.result = br; e.z = (br == 0); e.s = (br & 0x80) != 0; e.h = bHalf; e.pv = bOverflow; e.n = bSub; e.c = bCarry;
    return e;
}

void add_alu_tests() {
    const char* opNames[8] = {"ADD", "ADC", "SUB", "SBC", "AND", "XOR", "OR", "CP"};
    for (int op = 0; op < 8; ++op) {
        for (const auto& src : kRegSlots) {
            byte opcode = static_cast<byte>(0x80 | (op << 3) | src.code);
            add_test("0x" + to_hex(opcode) + " " + opNames[op] + " A," + src.name,
                [src, op, opcode] {
                    Cpu cpu; cpu.reset();
                    RegisterState init{};
                    byte aInit = 0x3C;
                    byte operand = (src.code == 7) ? aInit : 0x0F;
                    init.a = aInit;
                    init.f = 0; // carry-in = 0 for this pass
                    if (src.code == 6) {
                        init.hl = 0x9000;
                        cpu.set_registers(init);
                        cpu.get_memory()->data()[0x9000] = operand;
                    } else if (src.code != 7) {
                        set_reg_by_slot(init, src.code, operand);
                    }
                    auto r = run(cpu, 0, {opcode}, init);
                    AluExpected exp = reference_alu(op, aInit, operand, false);
                    bool aOk = (op == 7) ? (r.a == aInit) : (r.a == exp.result); // CP doesn't store
                    bool flagsOk =
                        ((r.f & FLAG_Z) != 0) == exp.z &&
                        ((r.f & FLAG_S) != 0) == exp.s &&
                        ((r.f & FLAG_H) != 0) == exp.h &&
                        ((r.f & FLAG_PV) != 0) == exp.pv &&
                        ((r.f & FLAG_N) != 0) == exp.n &&
                        ((r.f & FLAG_C) != 0) == exp.c;
                    return aOk && flagsOk;
                });
        }
    }
}

// ---- Base page: conditional RET/JP/CALL (0xC0-0xFC, 8 conditions x 3 forms) -
struct CondSlot { const char* name; uint8_t cc; byte flagBit; bool activeWhenSet; };
const CondSlot kConds[8] = {
    {"NZ", 0, FLAG_Z,  false},
    {"Z",  1, FLAG_Z,  true},
    {"NC", 2, FLAG_C,  false},
    {"C",  3, FLAG_C,  true},
    {"PO", 4, FLAG_PV, false},
    {"PE", 5, FLAG_PV, true},
    {"P",  6, FLAG_S,  false},
    {"M",  7, FLAG_S,  true},
};

void add_conditional_tests() {
    for (const auto& c : kConds) {
        byte retOp = static_cast<byte>(0xC0 | (c.cc << 3));
        add_test("0x" + to_hex(retOp) + " RET " + c.name + " (taken)", [c, retOp] {
            Cpu cpu; cpu.reset();
            RegisterState init{};
            init.f = c.activeWhenSet ? c.flagBit : 0;
            init.sp = 0x9000;
            cpu.set_registers(init);
            cpu.get_memory()->data()[0x9000] = 0x34;
            cpu.get_memory()->data()[0x9001] = 0x12;
            auto r = run(cpu, 0, {retOp}, init);
            return r.pc == 0x1234 && r.sp == 0x9002;
        });
        add_test("0x" + to_hex(retOp) + " RET " + c.name + " (not taken)", [c, retOp] {
            Cpu cpu; cpu.reset();
            RegisterState init{};
            init.f = c.activeWhenSet ? 0 : c.flagBit;
            init.sp = 0x9000;
            auto r = run(cpu, 0, {retOp}, init);
            return r.pc == 1 && r.sp == 0x9000;
        });

        byte jpOp = static_cast<byte>(0xC2 | (c.cc << 3));
        add_test("0x" + to_hex(jpOp) + " JP " + c.name + ",nn (taken)", [c, jpOp] {
            Cpu cpu; cpu.reset();
            RegisterState init{};
            init.f = c.activeWhenSet ? c.flagBit : 0;
            auto r = run(cpu, 0, {jpOp, 0x34, 0x12}, init);
            return r.pc == 0x1234;
        });
        add_test("0x" + to_hex(jpOp) + " JP " + c.name + ",nn (not taken)", [c, jpOp] {
            Cpu cpu; cpu.reset();
            RegisterState init{};
            init.f = c.activeWhenSet ? 0 : c.flagBit;
            auto r = run(cpu, 0, {jpOp, 0x34, 0x12}, init);
            return r.pc == 3;
        });

        byte callOp = static_cast<byte>(0xC4 | (c.cc << 3));
        add_test("0x" + to_hex(callOp) + " CALL " + c.name + ",nn (taken)", [c, callOp] {
            Cpu cpu; cpu.reset();
            RegisterState init{};
            init.f = c.activeWhenSet ? c.flagBit : 0;
            init.sp = 0x9000;
            auto r = run(cpu, 0, {callOp, 0x34, 0x12}, init);
            bool returnAddrOk = cpu.get_memory()->read(0x8FFE) == 0x03 && cpu.get_memory()->read(0x8FFF) == 0x00;
            return r.pc == 0x1234 && r.sp == 0x8FFE && returnAddrOk;
        });
        add_test("0x" + to_hex(callOp) + " CALL " + c.name + ",nn (not taken)", [c, callOp] {
            Cpu cpu; cpu.reset();
            RegisterState init{};
            init.f = c.activeWhenSet ? 0 : c.flagBit;
            init.sp = 0x9000;
            auto r = run(cpu, 0, {callOp, 0x34, 0x12}, init);
            return r.pc == 3 && r.sp == 0x9000;
        });
    }
}

void add_rst_tests() {
    for (int i = 0; i < 8; ++i) {
        byte opcode = static_cast<byte>(0xC7 | (i << 3));
        word vector = static_cast<word>(i * 8);
        add_test("0x" + to_hex(opcode) + " RST " + to_hex(static_cast<byte>(vector)) + "H",
            [opcode, vector] {
                Cpu cpu; cpu.reset();
                RegisterState init{}; init.sp = 0x9000;
                auto r = run(cpu, 0, {opcode}, init);
                return r.pc == vector && r.sp == 0x8FFE &&
                       cpu.get_memory()->read(0x8FFE) == 0x01 && cpu.get_memory()->read(0x8FFF) == 0x00;
            });
    }
}

void register_tests() {
    // ---- Base page: 0x00-0x0F ----------------------------------------

    add_test("0x00 NOP", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.bc = 0x1234;
        auto r = run(cpu, 0, {0x00}, init);
        return r.pc == 1 && r.bc == 0x1234;
    });

    add_test("0x01 LD BC,nn", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0x01, 0x34, 0x12}, {});
        return r.bc == 0x1234 && r.pc == 3;
    });

    add_test("0x02 LD (BC),A", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.a = 0x42;
        init.bc = 0x9000;
        auto r = run(cpu, 0, {0x02}, init);
        return cpu.get_memory()->read(0x9000) == 0x42;
    });

    add_test("0x03 INC BC (wraps)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.bc = 0xFFFF;
        auto r = run(cpu, 0, {0x03}, init);
        return r.bc == 0x0000;
    });

    add_test("0x04 INC B", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.bc = make_word(0x00, 0x05);
        auto r = run(cpu, 0, {0x04}, init);
        return hi_byte(r.bc) == 0x06 && !(r.f & FLAG_Z) && !(r.f & FLAG_N);
    });

    add_test("0x05 DEC B", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.bc = make_word(0x00, 0x05);
        auto r = run(cpu, 0, {0x05}, init);
        return hi_byte(r.bc) == 0x04 && (r.f & FLAG_N);
    });

    add_test("0x06 LD B,n", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0x06, 0x77}, {});
        return hi_byte(r.bc) == 0x77;
    });

    add_test("0x07 RLCA", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.a = 0x81;
        init.f = FLAG_Z | FLAG_S | FLAG_PV; // should be preserved by RLCA
        auto r = run(cpu, 0, {0x07}, init);
        return r.a == 0x03 && (r.f & FLAG_C) && (r.f & FLAG_Z) && (r.f & FLAG_S) && (r.f & FLAG_PV);
    });

    add_test("0x08 EX AF,AF' (round-trip)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.a = 0x11;
        init.f = 0x22;
        init.pc = 0;
        cpu.set_registers(init);
        byte* mem = cpu.get_memory()->data();
        mem[0] = 0x08;
        mem[1] = 0x08;
        cpu.step(); // swap out to shadow
        cpu.step(); // swap back
        auto r = cpu.get_registers();
        return r.a == 0x11 && r.f == 0x22 && r.pc == 2;
    });

    add_test("0x09 ADD HL,BC (carry)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.hl = 0xFFFF;
        init.bc = 0x0001;
        auto r = run(cpu, 0, {0x09}, init);
        return r.hl == 0x0000 && (r.f & FLAG_C);
    });

    add_test("0x0A LD A,(BC)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.bc = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x55;
        auto r = run(cpu, 0, {0x0A}, init);
        return r.a == 0x55;
    });

    add_test("0x0B DEC BC (wraps)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.bc = 0x0000;
        auto r = run(cpu, 0, {0x0B}, init);
        return r.bc == 0xFFFF;
    });

    add_test("0x0C INC C", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.bc = make_word(0x0F, 0x00);
        auto r = run(cpu, 0, {0x0C}, init);
        return lo_byte(r.bc) == 0x10;
    });

    add_test("0x0D DEC C", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.bc = make_word(0x01, 0x00);
        auto r = run(cpu, 0, {0x0D}, init);
        return lo_byte(r.bc) == 0x00 && (r.f & FLAG_Z);
    });

    add_test("0x0E LD C,n", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0x0E, 0x99}, {});
        return lo_byte(r.bc) == 0x99;
    });

    add_test("0x0F RRCA", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.a = 0x01;
        init.f = FLAG_Z | FLAG_S | FLAG_PV; // should be preserved by RRCA
        auto r = run(cpu, 0, {0x0F}, init);
        return r.a == 0x80 && (r.f & FLAG_C) && (r.f & FLAG_Z) && (r.f & FLAG_S) && (r.f & FLAG_PV);
    });

    // ---- Base page: 0x10-0x1F ------------------------------------------

    add_test("0x10 DJNZ e (taken)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.bc = make_word(0x00, 0x02);
        auto r = run(cpu, 0, {0x10, 0x05}, init);
        return hi_byte(r.bc) == 0x01 && r.pc == 7; // 0 + 2 (len) + 5 (offset)
    });

    add_test("0x11 LD DE,nn", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0x11, 0x78, 0x56}, {});
        return r.de == 0x5678;
    });

    add_test("0x12 LD (DE),A", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.a = 0x99;
        init.de = 0x9000;
        auto r = run(cpu, 0, {0x12}, init);
        return cpu.get_memory()->read(0x9000) == 0x99;
    });

    add_test("0x13 INC DE (wraps)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.de = 0xFFFF;
        auto r = run(cpu, 0, {0x13}, init);
        return r.de == 0x0000;
    });

    add_test("0x14 INC D (overflow)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.de = make_word(0x00, 0x7F);
        auto r = run(cpu, 0, {0x14}, init);
        return hi_byte(r.de) == 0x80 && (r.f & FLAG_PV) && (r.f & FLAG_S);
    });

    add_test("0x15 DEC D (overflow)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.de = make_word(0x00, 0x80);
        auto r = run(cpu, 0, {0x15}, init);
        return hi_byte(r.de) == 0x7F && (r.f & FLAG_PV) && (r.f & FLAG_N);
    });

    add_test("0x16 LD D,n", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0x16, 0xAB}, {});
        return hi_byte(r.de) == 0xAB;
    });

    add_test("0x17 RLA (preserves S/Z/PV)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.a = 0x80;
        init.f = 0; // C=0, Z=0, S=0, PV=0
        auto r = run(cpu, 0, {0x17}, init);
        return r.a == 0x00 && (r.f & FLAG_C) && !(r.f & FLAG_Z);
    });

    add_test("0x18 JR e", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0x18, 0x0A}, {});
        return r.pc == 12; // 0 + 2 (len) + 10 (offset)
    });

    add_test("0x19 ADD HL,DE (carry)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.hl = 0xFFFF;
        init.de = 0x0001;
        auto r = run(cpu, 0, {0x19}, init);
        return r.hl == 0x0000 && (r.f & FLAG_C);
    });

    add_test("0x1A LD A,(DE)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.de = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x77;
        auto r = run(cpu, 0, {0x1A}, init);
        return r.a == 0x77;
    });

    add_test("0x1B DEC DE (wraps)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.de = 0x0000;
        auto r = run(cpu, 0, {0x1B}, init);
        return r.de == 0xFFFF;
    });

    add_test("0x1C INC E (wraps)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.de = make_word(0xFF, 0x00);
        auto r = run(cpu, 0, {0x1C}, init);
        return lo_byte(r.de) == 0x00 && (r.f & FLAG_Z);
    });

    add_test("0x1D DEC E (wraps)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.de = make_word(0x00, 0x00);
        auto r = run(cpu, 0, {0x1D}, init);
        return lo_byte(r.de) == 0xFF && !(r.f & FLAG_Z) && (r.f & FLAG_S);
    });

    add_test("0x1E LD E,n", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0x1E, 0xCC}, {});
        return lo_byte(r.de) == 0xCC;
    });

    add_test("0x1F RRA (preserves S/Z/PV)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.a = 0x01;
        init.f = 0; // C=0, Z=0, S=0, PV=0
        auto r = run(cpu, 0, {0x1F}, init);
        return r.a == 0x00 && (r.f & FLAG_C) && !(r.f & FLAG_Z);
    });

    // ---- Base page: 0x20-0x25 -------------------------------------------

    add_test("0x20 JR NZ,e (taken)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.f = 0; // Z=0 -> not zero -> jump taken
        auto r = run(cpu, 0, {0x20, 0x08}, init);
        return r.pc == 10; // 0 + 2 (len) + 8 (offset)
    });

    add_test("0x21 LD HL,nn", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0x21, 0xEF, 0xBE}, {});
        return r.hl == 0xBEEF;
    });

    add_test("0x22 LD (nn),HL", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.hl = 0x1234;
        auto r = run(cpu, 0, {0x22, 0x00, 0x90}, init);
        return cpu.get_memory()->read(0x9000) == 0x34 && cpu.get_memory()->read(0x9001) == 0x12;
    });

    add_test("0x23 INC HL (wraps)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.hl = 0xFFFF;
        auto r = run(cpu, 0, {0x23}, init);
        return r.hl == 0x0000;
    });

    add_test("0x24 INC H (overflow)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.hl = make_word(0x00, 0x7F);
        auto r = run(cpu, 0, {0x24}, init);
        return hi_byte(r.hl) == 0x80 && (r.f & FLAG_PV) && (r.f & FLAG_S);
    });

    add_test("0x25 DEC H (overflow)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.hl = make_word(0x00, 0x80);
        auto r = run(cpu, 0, {0x25}, init);
        return hi_byte(r.hl) == 0x7F && (r.f & FLAG_PV) && (r.f & FLAG_N);
    });

    // ---- CB page: RLC group (CB00-07) ------------------------------------

    add_test("CB00 RLC B", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = make_word(0x00, 0x81);
        auto r = run(cpu, 0, {0xCB, 0x00}, init);
        return hi_byte(r.bc) == 0x03 && (r.f & FLAG_C);
    });
    add_test("CB01 RLC C", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = make_word(0x81, 0x00);
        auto r = run(cpu, 0, {0xCB, 0x01}, init);
        return lo_byte(r.bc) == 0x03 && (r.f & FLAG_C);
    });
    add_test("CB02 RLC D", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.de = make_word(0x00, 0x81);
        auto r = run(cpu, 0, {0xCB, 0x02}, init);
        return hi_byte(r.de) == 0x03 && (r.f & FLAG_C);
    });
    add_test("CB03 RLC E", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.de = make_word(0x81, 0x00);
        auto r = run(cpu, 0, {0xCB, 0x03}, init);
        return lo_byte(r.de) == 0x03 && (r.f & FLAG_C);
    });
    add_test("CB04 RLC H", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = make_word(0x00, 0x81);
        auto r = run(cpu, 0, {0xCB, 0x04}, init);
        return hi_byte(r.hl) == 0x03 && (r.f & FLAG_C);
    });
    add_test("CB05 RLC L", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = make_word(0x81, 0x00);
        auto r = run(cpu, 0, {0xCB, 0x05}, init);
        return lo_byte(r.hl) == 0x03 && (r.f & FLAG_C);
    });
    add_test("CB06 RLC (HL)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x81;
        auto r = run(cpu, 0, {0xCB, 0x06}, init);
        return cpu.get_memory()->read(0x9000) == 0x03 && (r.f & FLAG_C);
    });
    add_test("CB07 RLC A", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x81;
        auto r = run(cpu, 0, {0xCB, 0x07}, init);
        return r.a == 0x03 && (r.f & FLAG_C);
    });

    // ---- CB page: RRC group (CB08-0F) -------------------------------------

    add_test("CB08 RRC B", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = make_word(0x00, 0x01);
        auto r = run(cpu, 0, {0xCB, 0x08}, init);
        return hi_byte(r.bc) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB09 RRC C", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = make_word(0x01, 0x00);
        auto r = run(cpu, 0, {0xCB, 0x09}, init);
        return lo_byte(r.bc) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB0A RRC D", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.de = make_word(0x00, 0x01);
        auto r = run(cpu, 0, {0xCB, 0x0A}, init);
        return hi_byte(r.de) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB0B RRC E", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.de = make_word(0x01, 0x00);
        auto r = run(cpu, 0, {0xCB, 0x0B}, init);
        return lo_byte(r.de) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB0C RRC H", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = make_word(0x00, 0x01);
        auto r = run(cpu, 0, {0xCB, 0x0C}, init);
        return hi_byte(r.hl) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB0D RRC L", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = make_word(0x01, 0x00);
        auto r = run(cpu, 0, {0xCB, 0x0D}, init);
        return lo_byte(r.hl) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB0E RRC (HL)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x01;
        auto r = run(cpu, 0, {0xCB, 0x0E}, init);
        return cpu.get_memory()->read(0x9000) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB0F RRC A", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x01;
        auto r = run(cpu, 0, {0xCB, 0x0F}, init);
        return r.a == 0x80 && (r.f & FLAG_C);
    });

    // ---- CB page: RL group (CB10-17), through carry ------------------------

    add_test("CB10 RL B", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = make_word(0x00, 0x80); init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x10}, init);
        return hi_byte(r.bc) == 0x01 && (r.f & FLAG_C);
    });
    add_test("CB11 RL C", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = make_word(0x80, 0x00); init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x11}, init);
        return lo_byte(r.bc) == 0x01 && (r.f & FLAG_C);
    });
    add_test("CB12 RL D", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.de = make_word(0x00, 0x80); init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x12}, init);
        return hi_byte(r.de) == 0x01 && (r.f & FLAG_C);
    });
    add_test("CB13 RL E", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.de = make_word(0x80, 0x00); init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x13}, init);
        return lo_byte(r.de) == 0x01 && (r.f & FLAG_C);
    });
    add_test("CB14 RL H", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = make_word(0x00, 0x80); init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x14}, init);
        return hi_byte(r.hl) == 0x01 && (r.f & FLAG_C);
    });
    add_test("CB15 RL L", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = make_word(0x80, 0x00); init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x15}, init);
        return lo_byte(r.hl) == 0x01 && (r.f & FLAG_C);
    });
    add_test("CB16 RL (HL)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000; init.f = FLAG_C;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x80;
        auto r = run(cpu, 0, {0xCB, 0x16}, init);
        return cpu.get_memory()->read(0x9000) == 0x01 && (r.f & FLAG_C);
    });
    add_test("CB17 RL A", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x80; init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x17}, init);
        return r.a == 0x01 && (r.f & FLAG_C);
    });

    // ---- CB page: RR group (CB18-1F), through carry ------------------------

    add_test("CB18 RR B", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = make_word(0x00, 0x01); init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x18}, init);
        return hi_byte(r.bc) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB19 RR C", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = make_word(0x01, 0x00); init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x19}, init);
        return lo_byte(r.bc) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB1A RR D", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.de = make_word(0x00, 0x01); init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x1A}, init);
        return hi_byte(r.de) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB1B RR E", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.de = make_word(0x01, 0x00); init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x1B}, init);
        return lo_byte(r.de) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB1C RR H", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = make_word(0x00, 0x01); init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x1C}, init);
        return hi_byte(r.hl) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB1D RR L", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = make_word(0x01, 0x00); init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x1D}, init);
        return lo_byte(r.hl) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB1E RR (HL)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000; init.f = FLAG_C;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x01;
        auto r = run(cpu, 0, {0xCB, 0x1E}, init);
        return cpu.get_memory()->read(0x9000) == 0x80 && (r.f & FLAG_C);
    });
    add_test("CB1F RR A", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x01; init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCB, 0x1F}, init);
        return r.a == 0x80 && (r.f & FLAG_C);
    });

    // ---- CB page: SLA group (CB20-25 implemented so far) -------------------

    add_test("CB20 SLA B", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = make_word(0x00, 0x81);
        auto r = run(cpu, 0, {0xCB, 0x20}, init);
        return hi_byte(r.bc) == 0x02 && (r.f & FLAG_C);
    });
    add_test("CB21 SLA C", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = make_word(0x81, 0x00);
        auto r = run(cpu, 0, {0xCB, 0x21}, init);
        return lo_byte(r.bc) == 0x02 && (r.f & FLAG_C);
    });
    add_test("CB22 SLA D", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.de = make_word(0x00, 0x81);
        auto r = run(cpu, 0, {0xCB, 0x22}, init);
        return hi_byte(r.de) == 0x02 && (r.f & FLAG_C);
    });
    add_test("CB23 SLA E", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.de = make_word(0x81, 0x00);
        auto r = run(cpu, 0, {0xCB, 0x23}, init);
        return lo_byte(r.de) == 0x02 && (r.f & FLAG_C);
    });
    add_test("CB24 SLA H", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = make_word(0x00, 0x81);
        auto r = run(cpu, 0, {0xCB, 0x24}, init);
        return hi_byte(r.hl) == 0x02 && (r.f & FLAG_C);
    });
    add_test("CB25 SLA L", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = make_word(0x81, 0x00);
        auto r = run(cpu, 0, {0xCB, 0x25}, init);
        return lo_byte(r.hl) == 0x02 && (r.f & FLAG_C);
    });

    // ---- Base page: 0x26-0x3F -------------------------------------------

    add_test("0x26 LD H,n", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0x26, 0x9A}, {});
        return hi_byte(r.hl) == 0x9A;
    });
    add_test("0x27 DAA (8+8 BCD = 16)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        init.a = 0x10; // raw binary result of adding two BCD-8 digits
        init.f = FLAG_H; // half-carry occurred during that add
        auto r = run(cpu, 0, {0x27}, init);
        return r.a == 0x16 && !(r.f & FLAG_C);
    });
    add_test("0x28 JR Z,e (taken)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.f = FLAG_Z;
        auto r = run(cpu, 0, {0x28, 0x05}, init);
        return r.pc == 7;
    });
    add_test("0x29 ADD HL,HL (doubling+carry)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x8001;
        auto r = run(cpu, 0, {0x29}, init);
        return r.hl == 0x0002 && (r.f & FLAG_C);
    });
    add_test("0x2A LD HL,(nn)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x34;
        cpu.get_memory()->data()[0x9001] = 0x12;
        auto r = run(cpu, 0, {0x2A, 0x00, 0x90}, init);
        return r.hl == 0x1234;
    });
    add_test("0x2B DEC HL (wraps)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x0000;
        auto r = run(cpu, 0, {0x2B}, init);
        return r.hl == 0xFFFF;
    });
    add_test("0x2C INC L", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = make_word(0x0F, 0x00);
        auto r = run(cpu, 0, {0x2C}, init);
        return lo_byte(r.hl) == 0x10;
    });
    add_test("0x2D DEC L", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = make_word(0x01, 0x00);
        auto r = run(cpu, 0, {0x2D}, init);
        return lo_byte(r.hl) == 0x00 && (r.f & FLAG_Z);
    });
    add_test("0x2E LD L,n", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0x2E, 0x77}, {});
        return lo_byte(r.hl) == 0x77;
    });
    add_test("0x2F CPL", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x3C;
        auto r = run(cpu, 0, {0x2F}, init);
        return r.a == 0xC3 && (r.f & FLAG_H) && (r.f & FLAG_N);
    });
    add_test("0x30 JR NC,e (taken)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.f = 0;
        auto r = run(cpu, 0, {0x30, 0x04}, init);
        return r.pc == 6;
    });
    add_test("0x31 LD SP,nn", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0x31, 0x00, 0x80}, {});
        return r.sp == 0x8000;
    });
    add_test("0x32 LD (nn),A", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x5A;
        auto r = run(cpu, 0, {0x32, 0x00, 0x90}, init);
        return cpu.get_memory()->read(0x9000) == 0x5A;
    });
    add_test("0x33 INC SP (wraps)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0xFFFF;
        auto r = run(cpu, 0, {0x33}, init);
        return r.sp == 0x0000;
    });
    add_test("0x34 INC (HL)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x0F;
        auto r = run(cpu, 0, {0x34}, init);
        return cpu.get_memory()->read(0x9000) == 0x10 && (r.f & FLAG_H);
    });
    add_test("0x35 DEC (HL)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x01;
        auto r = run(cpu, 0, {0x35}, init);
        return cpu.get_memory()->read(0x9000) == 0x00 && (r.f & FLAG_Z);
    });
    add_test("0x36 LD (HL),n", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000;
        auto r = run(cpu, 0, {0x36, 0x99}, init);
        return cpu.get_memory()->read(0x9000) == 0x99;
    });
    add_test("0x37 SCF", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.f = FLAG_H | FLAG_N;
        auto r = run(cpu, 0, {0x37}, init);
        return (r.f & FLAG_C) && !(r.f & FLAG_H) && !(r.f & FLAG_N);
    });
    add_test("0x38 JR C,e (taken)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.f = FLAG_C;
        auto r = run(cpu, 0, {0x38, 0x03}, init);
        return r.pc == 5;
    });
    add_test("0x39 ADD HL,SP", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x0001; init.sp = 0xFFFF;
        auto r = run(cpu, 0, {0x39}, init);
        return r.hl == 0x0000 && (r.f & FLAG_C);
    });
    add_test("0x3A LD A,(nn)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x66;
        auto r = run(cpu, 0, {0x3A, 0x00, 0x90}, init);
        return r.a == 0x66;
    });
    add_test("0x3B DEC SP (wraps)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0x0000;
        auto r = run(cpu, 0, {0x3B}, init);
        return r.sp == 0xFFFF;
    });
    add_test("0x3C INC A", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x0F;
        auto r = run(cpu, 0, {0x3C}, init);
        return r.a == 0x10 && (r.f & FLAG_H);
    });
    add_test("0x3D DEC A", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x01;
        auto r = run(cpu, 0, {0x3D}, init);
        return r.a == 0x00 && (r.f & FLAG_Z);
    });
    add_test("0x3E LD A,n", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0x3E, 0xAB}, {});
        return r.a == 0xAB;
    });
    add_test("0x3F CCF", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.f = FLAG_C;
        auto r = run(cpu, 0, {0x3F}, init);
        return !(r.f & FLAG_C) && (r.f & FLAG_H) && !(r.f & FLAG_N);
    });

    // ---- Base page: LD r,r' / HALT / 8-bit ALU (0x40-0xBF) ----------------
    add_ld_rr_tests();
    add_test("0x76 HALT (pc stops advancing)", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0x76}, {});
        word before = r.pc;
        cpu.step(); // should spin in place, no interrupt controller yet
        word after = cpu.get_registers().pc;
        return r.halted && before == after;
    });
    add_alu_tests();
    add_test("0x86 ADD A,(HL) overflow+carry+zero", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0xFF; init.hl = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x01;
        auto r = run(cpu, 0, {0x86}, init);
        return r.a == 0x00 && (r.f & FLAG_Z) && (r.f & FLAG_C) && (r.f & FLAG_H) && !(r.f & FLAG_PV);
    });
    add_test("0x8F ADC A,A carry-in propagation", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x00; init.f = FLAG_C;
        auto r = run(cpu, 0, {0x8F}, init); // ADC A,A: 0+0+1=1
        return r.a == 0x01 && !(r.f & FLAG_Z) && !(r.f & FLAG_C);
    });
    add_test("0x97 SUB A (self, always zero)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x42;
        auto r = run(cpu, 0, {0x97}, init);
        return r.a == 0x00 && (r.f & FLAG_Z) && !(r.f & FLAG_C) && (r.f & FLAG_N);
    });
    add_test("0x9F SBC A,A with carry-in (underflow)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x00; init.f = FLAG_C;
        auto r = run(cpu, 0, {0x9F}, init); // SBC A,A: 0-0-1 = 0xFF
        return r.a == 0xFF && (r.f & FLAG_C) && (r.f & FLAG_S);
    });
    add_test("0xBF CP A (equal, A unchanged)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x77;
        auto r = run(cpu, 0, {0xBF}, init);
        return r.a == 0x77 && (r.f & FLAG_Z);
    });

    // ---- Base page: 0xC0-0xFF -------------------------------------------
    add_conditional_tests();
    add_rst_tests();

    add_test("0xC1 POP BC", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x34;
        cpu.get_memory()->data()[0x9001] = 0x12;
        auto r = run(cpu, 0, {0xC1}, init);
        return r.bc == 0x1234 && r.sp == 0x9002;
    });
    add_test("0xC3 JP nn", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0xC3, 0x34, 0x12}, {});
        return r.pc == 0x1234;
    });
    add_test("0xC5 PUSH BC", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = 0x1234; init.sp = 0x9000;
        auto r = run(cpu, 0, {0xC5}, init);
        return r.sp == 0x8FFE && cpu.get_memory()->read(0x8FFE) == 0x34 && cpu.get_memory()->read(0x8FFF) == 0x12;
    });
    add_test("0xC6 ADD A,n", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x14;
        auto r = run(cpu, 0, {0xC6, 0x02}, init);
        return r.a == 0x16;
    });
    add_test("0xC9 RET (unconditional)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x34;
        cpu.get_memory()->data()[0x9001] = 0x12;
        auto r = run(cpu, 0, {0xC9}, init);
        return r.pc == 0x1234;
    });
    add_test("0xCD CALL nn (unconditional)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0x9000;
        auto r = run(cpu, 0, {0xCD, 0x34, 0x12}, init);
        return r.pc == 0x1234 && r.sp == 0x8FFE;
    });
    add_test("0xCE ADC A,n (carry-in)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x00; init.f = FLAG_C;
        auto r = run(cpu, 0, {0xCE, 0x00}, init);
        return r.a == 0x01;
    });
    add_test("0xD1 POP DE", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x78;
        cpu.get_memory()->data()[0x9001] = 0x56;
        auto r = run(cpu, 0, {0xD1}, init);
        return r.de == 0x5678;
    });
    add_test("0xD3 OUT (n),A (stub: no crash, A unchanged)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x42;
        auto r = run(cpu, 0, {0xD3, 0x01}, init);
        return r.a == 0x42;
    });
    add_test("0xD5 PUSH DE", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.de = 0x5678; init.sp = 0x9000;
        auto r = run(cpu, 0, {0xD5}, init);
        return cpu.get_memory()->read(0x8FFE) == 0x78 && cpu.get_memory()->read(0x8FFF) == 0x56;
    });
    add_test("0xD6 SUB n", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x10;
        auto r = run(cpu, 0, {0xD6, 0x01}, init);
        return r.a == 0x0F && (r.f & FLAG_H);
    });
    add_test("0xD9 EXX", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = 0x1111; init.de = 0x2222; init.hl = 0x3333;
        auto r = run(cpu, 0, {0xD9}, init);
        return r.bc == 0 && r.de == 0 && r.hl == 0; // shadow regs default to 0, swapped in
    });
    add_test("0xDB IN A,(n) (stub returns 0xFF)", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0xDB, 0x01}, {});
        return r.a == 0xFF;
    });
    add_test("0xDE SBC A,n (carry-in)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x00; init.f = FLAG_C;
        auto r = run(cpu, 0, {0xDE, 0x00}, init);
        return r.a == 0xFF && (r.f & FLAG_C);
    });
    add_test("0xE1 POP HL", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0xCD;
        cpu.get_memory()->data()[0x9001] = 0xAB;
        auto r = run(cpu, 0, {0xE1}, init);
        return r.hl == 0xABCD;
    });
    add_test("0xE3 EX (SP),HL", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x1234; init.sp = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x78;
        cpu.get_memory()->data()[0x9001] = 0x56;
        auto r = run(cpu, 0, {0xE3}, init);
        return r.hl == 0x5678 && cpu.get_memory()->read(0x9000) == 0x34 && cpu.get_memory()->read(0x9001) == 0x12;
    });
    add_test("0xE5 PUSH HL", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0xABCD; init.sp = 0x9000;
        auto r = run(cpu, 0, {0xE5}, init);
        return cpu.get_memory()->read(0x8FFE) == 0xCD && cpu.get_memory()->read(0x8FFF) == 0xAB;
    });
    add_test("0xE6 AND n", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0xF0;
        auto r = run(cpu, 0, {0xE6, 0x0F}, init);
        return r.a == 0x00 && (r.f & FLAG_Z) && (r.f & FLAG_H);
    });
    add_test("0xE9 JP (HL)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x1234;
        auto r = run(cpu, 0, {0xE9}, init);
        return r.pc == 0x1234;
    });
    add_test("0xEB EX DE,HL", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.de = 0x1111; init.hl = 0x2222;
        auto r = run(cpu, 0, {0xEB}, init);
        return r.de == 0x2222 && r.hl == 0x1111;
    });
    add_test("0xEE XOR n", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0xFF;
        auto r = run(cpu, 0, {0xEE, 0x0F}, init);
        return r.a == 0xF0;
    });
    add_test("0xF1 POP AF", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x44; // F
        cpu.get_memory()->data()[0x9001] = 0x22; // A
        auto r = run(cpu, 0, {0xF1}, init);
        return r.a == 0x22 && r.f == 0x44;
    });
    add_test("0xF3 DI", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.iff1 = true; init.iff2 = true;
        auto r = run(cpu, 0, {0xF3}, init);
        return !r.iff1 && !r.iff2;
    });
    add_test("0xF5 PUSH AF", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x22; init.f = 0x44; init.sp = 0x9000;
        auto r = run(cpu, 0, {0xF5}, init);
        return cpu.get_memory()->read(0x8FFE) == 0x44 && cpu.get_memory()->read(0x8FFF) == 0x22;
    });
    add_test("0xF6 OR n", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0xF0;
        auto r = run(cpu, 0, {0xF6, 0x0F}, init);
        return r.a == 0xFF;
    });
    add_test("0xF9 LD SP,HL", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000;
        auto r = run(cpu, 0, {0xF9}, init);
        return r.sp == 0x9000;
    });
    add_test("0xFB EI", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.iff1 = false; init.iff2 = false;
        auto r = run(cpu, 0, {0xFB}, init);
        return r.iff1 && r.iff2;
    });
    add_test("0xFE CP n (equal)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x42;
        auto r = run(cpu, 0, {0xFE, 0x42}, init);
        return r.a == 0x42 && (r.f & FLAG_Z);
    });

    // ---- CB page: remaining rotate/shift families -------------------------
    add_cb_shift_family_tests();

    // ---- ED page ----------------------------------------------------------
    add_test("ED42 SBC HL,BC", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x0000; init.bc = 0x0001;
        auto r = run(cpu, 0, {0xED, 0x42}, init);
        return r.hl == 0xFFFF && (r.f & FLAG_C) && (r.f & FLAG_S);
    });
    add_test("ED4A ADC HL,BC", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0xFFFF; init.bc = 0x0000; init.f = FLAG_C;
        auto r = run(cpu, 0, {0xED, 0x4A}, init);
        return r.hl == 0x0000 && (r.f & FLAG_C) && (r.f & FLAG_Z);
    });
    add_test("ED52 SBC HL,DE", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x0000; init.de = 0x0001;
        auto r = run(cpu, 0, {0xED, 0x52}, init);
        return r.hl == 0xFFFF && (r.f & FLAG_C);
    });
    add_test("ED5A ADC HL,DE", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x0001; init.de = 0x0001; init.f = FLAG_C;
        auto r = run(cpu, 0, {0xED, 0x5A}, init);
        return r.hl == 0x0003;
    });
    add_test("ED62 SBC HL,HL (with carry)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x0000; init.f = FLAG_C;
        auto r = run(cpu, 0, {0xED, 0x62}, init);
        return r.hl == 0xFFFF && (r.f & FLAG_C) && (r.f & FLAG_S);
    });
    add_test("ED6A ADC HL,HL", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x8000;
        auto r = run(cpu, 0, {0xED, 0x6A}, init);
        return r.hl == 0x0000 && (r.f & FLAG_C) && (r.f & FLAG_Z);
    });
    add_test("ED72 SBC HL,SP", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x0000; init.sp = 0x0001;
        auto r = run(cpu, 0, {0xED, 0x72}, init);
        return r.hl == 0xFFFF && (r.f & FLAG_C);
    });
    add_test("ED7A ADC HL,SP", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x0001; init.sp = 0x0001; init.f = FLAG_C;
        auto r = run(cpu, 0, {0xED, 0x7A}, init);
        return r.hl == 0x0003;
    });
    add_test("ED43 LD (nn),BC", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = 0x1234;
        auto r = run(cpu, 0, {0xED, 0x43, 0x00, 0x90}, init);
        return cpu.get_memory()->read(0x9000) == 0x34 && cpu.get_memory()->read(0x9001) == 0x12;
    });
    add_test("ED4B LD BC,(nn)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x34;
        cpu.get_memory()->data()[0x9001] = 0x12;
        auto r = run(cpu, 0, {0xED, 0x4B, 0x00, 0x90}, init);
        return r.bc == 0x1234;
    });
    add_test("ED53 LD (nn),DE", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.de = 0x5678;
        auto r = run(cpu, 0, {0xED, 0x53, 0x00, 0x90}, init);
        return cpu.get_memory()->read(0x9000) == 0x78 && cpu.get_memory()->read(0x9001) == 0x56;
    });
    add_test("ED5B LD DE,(nn)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x78;
        cpu.get_memory()->data()[0x9001] = 0x56;
        auto r = run(cpu, 0, {0xED, 0x5B, 0x00, 0x90}, init);
        return r.de == 0x5678;
    });
    add_test("ED73 LD (nn),SP", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0x9ABC;
        auto r = run(cpu, 0, {0xED, 0x73, 0x00, 0x90}, init);
        return cpu.get_memory()->read(0x9000) == 0xBC && cpu.get_memory()->read(0x9001) == 0x9A;
    });
    add_test("ED7B LD SP,(nn)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0xBC;
        cpu.get_memory()->data()[0x9001] = 0x9A;
        auto r = run(cpu, 0, {0xED, 0x7B, 0x00, 0x90}, init);
        return r.sp == 0x9ABC;
    });
    add_test("ED44 NEG", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x01;
        auto r = run(cpu, 0, {0xED, 0x44}, init);
        return r.a == 0xFF && (r.f & FLAG_C) && (r.f & FLAG_N);
    });
    add_test("ED44 NEG of 0 (no carry)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x00;
        auto r = run(cpu, 0, {0xED, 0x44}, init);
        return r.a == 0x00 && !(r.f & FLAG_C) && (r.f & FLAG_Z);
    });
    add_test("ED4C NEG (undocumented duplicate)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x01;
        auto r = run(cpu, 0, {0xED, 0x4C}, init);
        return r.a == 0xFF;
    });
    add_test("ED45 RETN", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0x9000; init.iff2 = true; init.iff1 = false;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x34;
        cpu.get_memory()->data()[0x9001] = 0x12;
        auto r = run(cpu, 0, {0xED, 0x45}, init);
        return r.pc == 0x1234 && r.iff1 == true; // restored from iff2
    });
    add_test("ED4D RETI", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x34;
        cpu.get_memory()->data()[0x9001] = 0x12;
        auto r = run(cpu, 0, {0xED, 0x4D}, init);
        return r.pc == 0x1234;
    });
    add_test("ED46 IM 0", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0xED, 0x46}, {});
        return r.im == 0;
    });
    add_test("ED56 IM 1", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0xED, 0x56}, {});
        return r.im == 1;
    });
    add_test("ED5E IM 2", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0xED, 0x5E}, {});
        return r.im == 2;
    });
    add_test("ED47 LD I,A", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x5A;
        auto r = run(cpu, 0, {0xED, 0x47}, init);
        return r.i == 0x5A;
    });
    add_test("ED4F LD R,A", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x5A;
        auto r = run(cpu, 0, {0xED, 0x4F}, init);
        return r.r == 0x5A;
    });
    add_test("ED57 LD A,I", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.i = 0x00; init.iff2 = true;
        auto r = run(cpu, 0, {0xED, 0x57}, init);
        return r.a == 0x00 && (r.f & FLAG_Z) && (r.f & FLAG_PV);
    });
    add_test("ED5F LD A,R", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.r = 0x7F; init.iff2 = false;
        auto r = run(cpu, 0, {0xED, 0x5F}, init);
        return r.a == 0x7F && !(r.f & FLAG_PV);
    });
    add_test("ED67 RRD", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x84; init.hl = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x20;
        auto r = run(cpu, 0, {0xED, 0x67}, init);
        return r.a == 0x80 && cpu.get_memory()->read(0x9000) == 0x42;
    });
    add_test("ED6F RLD", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x84; init.hl = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x20;
        auto r = run(cpu, 0, {0xED, 0x6F}, init);
        return r.a == 0x82 && cpu.get_memory()->read(0x9000) == 0x04;
    });
    add_test("ED00 undefined ED opcode behaves as NOP", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x42;
        auto r = run(cpu, 0, {0xED, 0x00}, init);
        return r.a == 0x42 && r.pc == 2;
    });
    add_test("EDA0 LDI", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000; init.de = 0x9010; init.bc = 0x0002;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x77;
        auto r = run(cpu, 0, {0xED, 0xA0}, init);
        return cpu.get_memory()->read(0x9010) == 0x77 && r.hl == 0x9001 && r.de == 0x9011 && r.bc == 0x0001 && (r.f & FLAG_PV);
    });
    add_test("EDA8 LDD", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000; init.de = 0x9010; init.bc = 0x0001;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x88;
        auto r = run(cpu, 0, {0xED, 0xA8}, init);
        return cpu.get_memory()->read(0x9010) == 0x88 && r.hl == 0x8FFF && r.de == 0x900F && r.bc == 0x0000 && !(r.f & FLAG_PV);
    });
    add_test("EDB0 LDIR (repeats until BC==0)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000; init.de = 0x9100; init.bc = 0x0003; init.pc = 0;
        cpu.set_registers(init);
        byte* mem = cpu.get_memory()->data();
        mem[0] = 0xED; mem[1] = 0xB0;
        mem[0x9000] = 0x01; mem[0x9001] = 0x02; mem[0x9002] = 0x03;
        for (int i = 0; i < 3; ++i) cpu.step();
        auto r = cpu.get_registers();
        return mem[0x9100] == 0x01 && mem[0x9101] == 0x02 && mem[0x9102] == 0x03 && r.bc == 0 && r.pc == 2;
    });
    add_test("EDB8 LDDR (repeats until BC==0)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9002; init.de = 0x9102; init.bc = 0x0003; init.pc = 0;
        cpu.set_registers(init);
        byte* mem = cpu.get_memory()->data();
        mem[0] = 0xED; mem[1] = 0xB8;
        mem[0x9000] = 0x01; mem[0x9001] = 0x02; mem[0x9002] = 0x03;
        for (int i = 0; i < 3; ++i) cpu.step();
        auto r = cpu.get_registers();
        return mem[0x9100] == 0x01 && mem[0x9101] == 0x02 && mem[0x9102] == 0x03 && r.bc == 0 && r.pc == 2;
    });
    add_test("EDA1 CPI (match)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x42; init.hl = 0x9000; init.bc = 0x0002;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x42;
        auto r = run(cpu, 0, {0xED, 0xA1}, init);
        return (r.f & FLAG_Z) && r.hl == 0x9001 && r.bc == 0x0001;
    });
    add_test("EDA9 CPD (no match)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x42; init.hl = 0x9000; init.bc = 0x0002;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x10;
        auto r = run(cpu, 0, {0xED, 0xA9}, init);
        return !(r.f & FLAG_Z) && r.hl == 0x8FFF && r.bc == 0x0001;
    });
    add_test("EDB1 CPIR (finds match and stops)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x03; init.hl = 0x9000; init.bc = 0x0003; init.pc = 0;
        cpu.set_registers(init);
        byte* mem = cpu.get_memory()->data();
        mem[0] = 0xED; mem[1] = 0xB1;
        mem[0x9000] = 0x01; mem[0x9001] = 0x02; mem[0x9002] = 0x03;
        for (int i = 0; i < 3; ++i) cpu.step();
        auto r = cpu.get_registers();
        return (r.f & FLAG_Z) && r.hl == 0x9003 && r.bc == 0x0000 && r.pc == 2;
    });
    add_test("EDB9 CPDR (finds match and stops)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.a = 0x01; init.hl = 0x9002; init.bc = 0x0003; init.pc = 0;
        cpu.set_registers(init);
        byte* mem = cpu.get_memory()->data();
        mem[0] = 0xED; mem[1] = 0xB9;
        mem[0x9000] = 0x01; mem[0x9001] = 0x02; mem[0x9002] = 0x03;
        for (int i = 0; i < 3; ++i) cpu.step();
        auto r = cpu.get_registers();
        return (r.f & FLAG_Z) && r.hl == 0x8FFF && r.bc == 0x0000 && r.pc == 2;
    });
    add_test("EDA2 INI", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000; init.bc = make_word(0x00, 0x02);
        auto r = run(cpu, 0, {0xED, 0xA2}, init);
        return cpu.get_memory()->read(0x9000) == 0xFF && r.hl == 0x9001 && hi_byte(r.bc) == 0x01;
    });
    add_test("EDAA IND", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000; init.bc = make_word(0x00, 0x01);
        auto r = run(cpu, 0, {0xED, 0xAA}, init);
        return cpu.get_memory()->read(0x9000) == 0xFF && r.hl == 0x8FFF && hi_byte(r.bc) == 0x00 && (r.f & FLAG_Z);
    });
    add_test("EDB2 INIR (repeats until B==0)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000; init.bc = make_word(0x00, 0x02); init.pc = 0;
        cpu.set_registers(init);
        byte* mem = cpu.get_memory()->data();
        mem[0] = 0xED; mem[1] = 0xB2;
        for (int i = 0; i < 2; ++i) cpu.step();
        auto r = cpu.get_registers();
        return r.hl == 0x9002 && hi_byte(r.bc) == 0x00 && r.pc == 2;
    });
    add_test("EDBA INDR (repeats until B==0)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000; init.bc = make_word(0x00, 0x02); init.pc = 0;
        cpu.set_registers(init);
        byte* mem = cpu.get_memory()->data();
        mem[0] = 0xED; mem[1] = 0xBA;
        for (int i = 0; i < 2; ++i) cpu.step();
        auto r = cpu.get_registers();
        return r.hl == 0x8FFE && hi_byte(r.bc) == 0x00 && r.pc == 2;
    });
    add_test("EDA3 OUTI", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000; init.bc = make_word(0x00, 0x02);
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x55;
        auto r = run(cpu, 0, {0xED, 0xA3}, init);
        return r.hl == 0x9001 && hi_byte(r.bc) == 0x01;
    });
    add_test("EDAB OUTD", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000; init.bc = make_word(0x00, 0x01);
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x66;
        auto r = run(cpu, 0, {0xED, 0xAB}, init);
        return r.hl == 0x8FFF && hi_byte(r.bc) == 0x00 && (r.f & FLAG_Z);
    });
    add_test("EDB3 OTIR (repeats until B==0)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9000; init.bc = make_word(0x00, 0x02); init.pc = 0;
        cpu.set_registers(init);
        byte* mem = cpu.get_memory()->data();
        mem[0] = 0xED; mem[1] = 0xB3;
        mem[0x9000] = 0x01; mem[0x9001] = 0x02;
        for (int i = 0; i < 2; ++i) cpu.step();
        auto r = cpu.get_registers();
        return r.hl == 0x9002 && hi_byte(r.bc) == 0x00 && r.pc == 2;
    });
    add_test("EDBB OTDR (repeats until B==0)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.hl = 0x9002; init.bc = make_word(0x00, 0x02); init.pc = 0;
        cpu.set_registers(init);
        byte* mem = cpu.get_memory()->data();
        mem[0] = 0xED; mem[1] = 0xBB;
        mem[0x9001] = 0x01; mem[0x9002] = 0x02;
        for (int i = 0; i < 2; ++i) cpu.step();
        auto r = cpu.get_registers();
        return r.hl == 0x9000 && hi_byte(r.bc) == 0x00 && r.pc == 2;
    });

    // ---- Indexed pages: DD/FD -----------------------------------------
    add_test("DD21 LD IX,nn", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0xDD, 0x21, 0x34, 0x12}, {});
        return r.ix == 0x1234;
    });
    add_test("FD21 LD IY,nn", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0xFD, 0x21, 0x78, 0x56}, {});
        return r.iy == 0x5678;
    });
    add_test("DD22 LD (nn),IX", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x1234;
        auto r = run(cpu, 0, {0xDD, 0x22, 0x00, 0x90}, init);
        return cpu.get_memory()->read(0x9000) == 0x34 && cpu.get_memory()->read(0x9001) == 0x12;
    });
    add_test("DD23 INC IX (wraps)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0xFFFF;
        auto r = run(cpu, 0, {0xDD, 0x23}, init);
        return r.ix == 0x0000;
    });
    add_test("DD24 INC IXH (undocumented)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x00FF;
        auto r = run(cpu, 0, {0xDD, 0x24}, init);
        return hi_byte(r.ix) == 0x01;
    });
    add_test("DD2C INC IXL (undocumented)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x000F;
        auto r = run(cpu, 0, {0xDD, 0x2C}, init);
        return lo_byte(r.ix) == 0x10;
    });
    add_test("DD26 LD IXH,n (undocumented)", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0xDD, 0x26, 0x99}, {});
        return hi_byte(r.ix) == 0x99;
    });
    add_test("DD2E LD IXL,n (undocumented)", [] {
        Cpu cpu; cpu.reset();
        auto r = run(cpu, 0, {0xDD, 0x2E, 0x77}, {});
        return lo_byte(r.ix) == 0x77;
    });
    add_test("DD2A LD IX,(nn)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{};
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x34;
        cpu.get_memory()->data()[0x9001] = 0x12;
        auto r = run(cpu, 0, {0xDD, 0x2A, 0x00, 0x90}, init);
        return r.ix == 0x1234;
    });
    add_test("DD2B DEC IX (wraps)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x0000;
        auto r = run(cpu, 0, {0xDD, 0x2B}, init);
        return r.ix == 0xFFFF;
    });
    add_test("DD09 ADD IX,BC", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x0001; init.bc = 0x0001;
        auto r = run(cpu, 0, {0xDD, 0x09}, init);
        return r.ix == 0x0002;
    });
    add_test("DD29 ADD IX,IX (doubling)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x1000;
        auto r = run(cpu, 0, {0xDD, 0x29}, init);
        return r.ix == 0x2000;
    });
    add_test("DD34 INC (IX+d)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9005] = 0x0F;
        auto r = run(cpu, 0, {0xDD, 0x34, 0x05}, init);
        return cpu.get_memory()->read(0x9005) == 0x10;
    });
    add_test("DD35 DEC (IX+d) with negative displacement", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9010;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x900E] = 0x01;
        auto r = run(cpu, 0, {0xDD, 0x35, 0xFE}, init); // 0xFE = -2 signed
        return cpu.get_memory()->read(0x900E) == 0x00;
    });
    add_test("DD36 LD (IX+d),n", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9000;
        auto r = run(cpu, 0, {0xDD, 0x36, 0x03, 0x99}, init);
        return cpu.get_memory()->read(0x9003) == 0x99;
    });
    add_test("DD46 LD B,(IX+d)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9002] = 0x5A;
        auto r = run(cpu, 0, {0xDD, 0x46, 0x02}, init);
        return hi_byte(r.bc) == 0x5A;
    });
    add_test("DD70 LD (IX+d),B", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9000; init.bc = make_word(0x00, 0x66);
        auto r = run(cpu, 0, {0xDD, 0x70, 0x02}, init);
        return cpu.get_memory()->read(0x9002) == 0x66;
    });
    add_test("DD86 ADD A,(IX+d)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9000; init.a = 0x01;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9004] = 0x02;
        auto r = run(cpu, 0, {0xDD, 0x86, 0x04}, init);
        return r.a == 0x03;
    });
    // Maskable interrupt acknowledge. Without this the ZX ROM never scans
    // the keyboard, since KEY-SCAN is only reached from the 50Hz handler.
    add_test("INT ignored while IFF1 clear", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.pc = 0x1234; init.sp = 0x8000;
        init.iff1 = false; init.im = 1;
        cpu.set_registers(init);
        int cycles = cpu.interrupt();
        auto r = cpu.get_registers();
        return cycles == 0 && r.pc == 0x1234 && r.sp == 0x8000;
    });
    add_test("INT mode 1 vectors to 0x0038 and pushes PC", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.pc = 0x1234; init.sp = 0x8000;
        init.iff1 = true; init.iff2 = true; init.im = 1;
        cpu.set_registers(init);
        int cycles = cpu.interrupt();
        auto r = cpu.get_registers();
        return cycles == 13 && r.pc == 0x0038 && r.sp == 0x7FFE
            && !r.iff1 && !r.iff2
            && cpu.get_memory()->read(0x7FFE) == 0x34
            && cpu.get_memory()->read(0x7FFF) == 0x12;
    });
    add_test("INT mode 2 vectors through (I*256+0xFF)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.pc = 0x1234; init.sp = 0x8000;
        init.iff1 = true; init.im = 2; init.i = 0x90;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x90FF] = 0xCD; // handler low byte
        cpu.get_memory()->data()[0x9100] = 0xAB; // handler high byte
        int cycles = cpu.interrupt();
        auto r = cpu.get_registers();
        return cycles == 19 && r.pc == 0xABCD && r.sp == 0x7FFE;
    });
    add_test("INT releases HALT", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0x8000; init.iff1 = true; init.im = 1;
        auto r = run(cpu, 0x1000, {0x76}, init); // HALT
        if (!r.halted || r.pc != 0x1001) return false;
        cpu.interrupt();
        r = cpu.get_registers();
        // The pushed address is the instruction *after* HALT, so RETI resumes
        // there rather than re-halting.
        return !r.halted && r.pc == 0x0038
            && cpu.get_memory()->read(0x7FFE) == 0x01
            && cpu.get_memory()->read(0x7FFF) == 0x10;
    });

    // H/L must stay *plain* H/L when the instruction uses (IX+d)/(IY+d)
    // addressing -- only displacement-free DD/FD opcodes address IXH/IXL.
    // Getting this wrong corrupted IY on `LD L,(IY+d)` and broke ROM boot.
    add_test("DD66 LD H,(IX+d) targets plain H, not IXH", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9000; init.hl = 0x1234;
        cpu.get_memory()->data()[0x9002] = 0x5A;
        auto r = run(cpu, 0, {0xDD, 0x66, 0x02}, init);
        return r.hl == 0x5A34 && r.ix == 0x9000;
    });
    add_test("FD6E LD L,(IY+d) targets plain L, not IYL", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.iy = 0x9000; init.hl = 0x1234;
        cpu.get_memory()->data()[0x9002] = 0x5A;
        auto r = run(cpu, 0, {0xFD, 0x6E, 0x02}, init);
        return r.hl == 0x125A && r.iy == 0x9000;
    });
    add_test("DD74 LD (IX+d),H stores plain H, not IXH", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9000; init.hl = 0xAB00;
        auto r = run(cpu, 0, {0xDD, 0x74, 0x02}, init);
        return cpu.get_memory()->read(0x9002) == 0xAB;
    });
    add_test("DD7C LD A,IXH (no displacement -> index half)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0xAB00; init.hl = 0x1234;
        auto r = run(cpu, 0, {0xDD, 0x7C}, init);
        return r.a == 0xAB;
    });
    add_test("DD44 LD B,IXH (no displacement -> index half)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0xCD00; init.hl = 0x1234;
        auto r = run(cpu, 0, {0xDD, 0x44}, init);
        return hi_byte(r.bc) == 0xCD;
    });

    add_test("DD04 INC B (unaffected by DD prefix)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.bc = make_word(0x00, 0x05);
        auto r = run(cpu, 0, {0xDD, 0x04}, init);
        return hi_byte(r.bc) == 0x06;
    });
    add_test("DDE1 POP IX", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.sp = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x34;
        cpu.get_memory()->data()[0x9001] = 0x12;
        auto r = run(cpu, 0, {0xDD, 0xE1}, init);
        return r.ix == 0x1234;
    });
    add_test("DDE5 PUSH IX", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0xABCD; init.sp = 0x9000;
        auto r = run(cpu, 0, {0xDD, 0xE5}, init);
        return cpu.get_memory()->read(0x8FFE) == 0xCD && cpu.get_memory()->read(0x8FFF) == 0xAB;
    });
    add_test("DDE9 JP (IX)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x1234;
        auto r = run(cpu, 0, {0xDD, 0xE9}, init);
        return r.pc == 0x1234;
    });
    add_test("DDF9 LD SP,IX", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9000;
        auto r = run(cpu, 0, {0xDD, 0xF9}, init);
        return r.sp == 0x9000;
    });
    add_test("DDE3 EX (SP),IX", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x1234; init.sp = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9000] = 0x78;
        cpu.get_memory()->data()[0x9001] = 0x56;
        auto r = run(cpu, 0, {0xDD, 0xE3}, init);
        return r.ix == 0x5678 && cpu.get_memory()->read(0x9000) == 0x34;
    });

    // ---- Indexed-CB pages: DD CB / FD CB -------------------------------
    add_test("DDCB.. BIT 3,(IX+d)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9000; init.f = FLAG_C;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9002] = 0x08; // bit 3 set
        auto r = run(cpu, 0, {0xDD, 0xCB, 0x02, 0x5E}, init); // BIT 3,(IX+d)
        return !(r.f & FLAG_Z) && (r.f & FLAG_C);
    });
    add_test("DDCB.. RES 2,(IX+d)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9002] = 0xFF;
        auto r = run(cpu, 0, {0xDD, 0xCB, 0x02, 0x96}, init); // RES 2,(IX+d)
        return cpu.get_memory()->read(0x9002) == 0xFB;
    });
    add_test("DDCB.. SET 5,(IX+d)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9002] = 0x00;
        auto r = run(cpu, 0, {0xDD, 0xCB, 0x02, 0xEE}, init); // SET 5,(IX+d)
        return cpu.get_memory()->read(0x9002) == 0x20;
    });
    add_test("DDCB.. RLC (IX+d)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9002] = 0x81;
        auto r = run(cpu, 0, {0xDD, 0xCB, 0x02, 0x06}, init); // RLC (IX+d)
        return cpu.get_memory()->read(0x9002) == 0x03 && (r.f & FLAG_C);
    });
    add_test("DDCB.. RLC (IX+d) also stores into B (undocumented)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.ix = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9002] = 0x81;
        auto r = run(cpu, 0, {0xDD, 0xCB, 0x02, 0x00}, init); // RLC (IX+d),B
        return cpu.get_memory()->read(0x9002) == 0x03 && hi_byte(r.bc) == 0x03;
    });
    add_test("FDCB.. SRL (IY+d)", [] {
        Cpu cpu; cpu.reset();
        RegisterState init{}; init.iy = 0x9000;
        cpu.set_registers(init);
        cpu.get_memory()->data()[0x9002] = 0x03;
        auto r = run(cpu, 0, {0xFD, 0xCB, 0x02, 0x3E}, init); // SRL (IY+d)
        return cpu.get_memory()->read(0x9002) == 0x01 && (r.f & FLAG_C);
    });

    // ---- CB page: BIT/RES/SET (192 combinations) ---------------------------
    add_bit_res_set_tests();
}

} // namespace

int main() {
    register_tests();
    register_tape_tests();
    register_trap_tests();
    register_memory_tests();
    register_tape_trap_tests();
    register_rom_protect_tests();
    register_timing_tests();
    register_beeper_tests();

    int failures = 0;
    for (auto& t : tests()) {
        bool passed = false;
        try {
            passed = t.fn();
        } catch (...) {
            passed = false;
        }

        if (passed) {
            std::printf("%-32s \033[32mPassed\033[0m\n", t.name.c_str());
        } else {
            std::printf("%-32s \033[31mFailed\033[0m\n", t.name.c_str());
            ++failures;
        }
    }

    std::printf("\n%zu test(s), %d failed.\n", tests().size(), failures);
    return failures == 0 ? 0 : 1;
}
