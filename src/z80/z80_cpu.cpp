#include "z80/z80_cpu.h"
#include <algorithm>
#include <cassert>
#include <thread>

namespace z80 {


// Number of immediate bytes (n or nn, little-endian) carried by each
// unprefixed opcode. Prefix bytes themselves are entered as 0.
constexpr byte kBaseImm[256] = {
//   0  1  2  3  4  5  6  7    8  9  A  B  C  D  E  F
     0, 2, 0, 0, 0, 0, 1, 0,   0, 0, 0, 0, 0, 0, 1, 0,  // 00
     1, 2, 0, 0, 0, 0, 1, 0,   1, 0, 0, 0, 0, 0, 1, 0,  // 10
     1, 2, 2, 0, 0, 0, 1, 0,   1, 0, 2, 0, 0, 0, 1, 0,  // 20
     1, 2, 2, 0, 0, 0, 1, 0,   1, 0, 2, 0, 0, 0, 1, 0,  // 30
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // 40
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // 50
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // 60
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // 70
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // 80
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // 90
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // A0
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // B0
     0, 0, 2, 2, 2, 0, 1, 0,   0, 0, 2, 0, 2, 2, 1, 0,  // C0  (CB at 0xCB)
     0, 0, 2, 1, 2, 0, 1, 0,   0, 0, 2, 1, 2, 0, 1, 0,  // D0  (D3/DB take n)
     0, 0, 2, 0, 2, 0, 1, 0,   0, 0, 2, 0, 2, 0, 1, 0,  // E0  (ED at 0xED)
     0, 0, 2, 0, 2, 0, 1, 0,   0, 0, 2, 0, 2, 0, 1, 0,  // F0
};

// Unprefixed opcodes that address (HL), and therefore take a displacement
// byte when preceded by DD or FD.  Note 0x76 is HALT, not LD (HL),(HL), so
// DD 76 stays 2 bytes with no displacement.
constexpr byte kIndexed[256] = {
//   0  1  2  3  4  5  6  7    8  9  A  B  C  D  E  F
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // 00
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // 10
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // 20
     0, 0, 0, 0, 1, 1, 1, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // 30  INC/DEC/LD (HL)
     0, 0, 0, 0, 0, 0, 1, 0,   0, 0, 0, 0, 0, 0, 1, 0,  // 40
     0, 0, 0, 0, 0, 0, 1, 0,   0, 0, 0, 0, 0, 0, 1, 0,  // 50
     0, 0, 0, 0, 0, 0, 1, 0,   0, 0, 0, 0, 0, 0, 1, 0,  // 60
     1, 1, 1, 1, 1, 1, 0, 1,   0, 0, 0, 0, 0, 0, 1, 0,  // 70  LD (HL),r / HALT
     0, 0, 0, 0, 0, 0, 1, 0,   0, 0, 0, 0, 0, 0, 1, 0,  // 80
     0, 0, 0, 0, 0, 0, 1, 0,   0, 0, 0, 0, 0, 0, 1, 0,  // 90
     0, 0, 0, 0, 0, 0, 1, 0,   0, 0, 0, 0, 0, 0, 1, 0,  // A0
     0, 0, 0, 0, 0, 0, 1, 0,   0, 0, 0, 0, 0, 0, 1, 0,  // B0
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // C0
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // D0
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // E0
     0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,  // F0
};

// Base-page T-states. Rows 0x40-0xBF (LD r,r' and the 8-bit ALU block) are
// computed by formula in Cpu::states() instead (4/7 depending on whether the
// (HL) slot is involved), so they're left at 0 here. Conditional branches
// (JR cc, DJNZ, RET cc, CALL cc) list their minimum ("not taken") cost;
// Cpu::states() adds the extra cycles when the branch is actually taken.
// 0xCB/0xDD/0xED/0xFD slots are dead (those bytes are intercepted by decode()
// into a different page before reaching a Base-page opcode lookup).
constexpr byte kBaseStates[256] = {
//    0   1   2   3   4   5   6   7     8   9   A   B   C   D   E   F
      4, 10,  7,  6,  4,  4,  7,  4,    4, 11,  7,  6,  4,  4,  7,  4,  // 00
      8, 10,  7,  6,  4,  4,  7,  4,   12, 11,  7,  6,  4,  4,  7,  4,  // 10  (0x10 djnz min=8, 0x18 jr e=12 fixed)
      7, 10, 16,  6,  4,  4,  7,  4,    7, 11, 16,  6,  4,  4,  7,  4,  // 20  (jr nz/z min=7)
      7, 10, 13,  6, 11, 11, 10,  4,    7, 11, 13,  6,  4,  4,  7,  4,  // 30  (jr nc/c min=7)
      0,  0,  0,  0,  0,  0,  0,  0,    0,  0,  0,  0,  0,  0,  0,  0,  // 40
      0,  0,  0,  0,  0,  0,  0,  0,    0,  0,  0,  0,  0,  0,  0,  0,  // 50
      0,  0,  0,  0,  0,  0,  0,  0,    0,  0,  0,  0,  0,  0,  0,  0,  // 60
      0,  0,  0,  0,  0,  0,  0,  0,    0,  0,  0,  0,  0,  0,  0,  0,  // 70
      0,  0,  0,  0,  0,  0,  0,  0,    0,  0,  0,  0,  0,  0,  0,  0,  // 80
      0,  0,  0,  0,  0,  0,  0,  0,    0,  0,  0,  0,  0,  0,  0,  0,  // 90
      0,  0,  0,  0,  0,  0,  0,  0,    0,  0,  0,  0,  0,  0,  0,  0,  // A0
      0,  0,  0,  0,  0,  0,  0,  0,    0,  0,  0,  0,  0,  0,  0,  0,  // B0
      5, 10, 10, 10, 10, 11,  7, 11,    5, 10, 10,  0, 10, 17,  7, 11,  // C0  (ret/call cc min=5/10)
      5, 10, 10, 11, 10, 11,  7, 11,    5,  4, 10, 11, 10,  0,  7, 11,  // D0
      5, 10, 10, 19, 10, 11,  7, 11,    5,  4, 10,  4, 10,  0,  7, 11,  // E0
      5, 10, 10,  4, 10, 11,  7, 11,    5,  6, 10,  4, 10,  0,  7, 11,  // F0
};

Cpu::Cpu() {
    boot();
//    reset();
}

std::chrono::duration<double, std::milli> Cpu::time_to_sleep(
    int cycle_count,
    std::chrono::time_point<std::chrono::high_resolution_clock> current_time,
    std::chrono::time_point<std::chrono::high_resolution_clock> cycle_time)
{
    // How long cycle_count T-states should take on real hardware, using the
    // clock rate already implied by CYCLES_PER_FRAME/FRMAES_PER_SECOND
    // rather than a separately-defined Hz constant that could subtly disagree.
    std::chrono::duration<double, std::milli> ideal_elapsed(
        (1000.0 / FRMAES_PER_SECOND) * (static_cast<double>(cycle_count) / CYCLES_PER_FRAME));

    std::chrono::duration<double, std::milli> actual_elapsed = current_time - cycle_time;

    auto sleep_time = ideal_elapsed - actual_elapsed;
    if (sleep_time.count() < 0)
    {
        sleep_time = std::chrono::duration<double, std::milli>(0);
    }
    return sleep_time;
}

void Cpu::reset() {
    pc_ = 0;
    sp_ = 0xFFFF;
    pc_ = ROM_START_ADDRESS;
    cycle_time_ = std::chrono::high_resolution_clock::now();
    cycle_duration_ = std::chrono::milliseconds(0);
    cycle_count_ = 0;
    halted_ = false;
}

int Cpu::interrupt()
{
    // A maskable interrupt is only accepted when IFF1 is set. (EI famously
    // only takes effect *after* the following instruction, so that the
    // "EI / RET" tail of an ISR can't be re-entered; that one-instruction
    // delay is not modelled here.)
    if (!iff1) return 0;

    // /INT releases HALT: the CPU resumes at the instruction after it. pc_
    // was already advanced past the HALT opcode when it executed, so the
    // value pushed below is already the correct return address.
    halted_ = false;

    // Both flip-flops clear on acceptance, so the ISR runs uninterrupted
    // until it does EI/RETI. IFF2 keeps the old IFF1 for RETN's benefit.
    iff1 = iff2 = false;

    push_word(pc_);

    switch (im) {
    case 0:
        // Mode 0 executes whatever the device puts on the data bus. Nothing
        // drives it on a Spectrum, so the floating bus reads 0xFF = RST 38 --
        // the same destination as mode 1.
    case 1:
        pc_ = 0x0038;
        return 13;
    case 2: {
        // I supplies the high byte, the device the low byte; again nothing
        // drives the bus, so 0xFF it is. The word at that address is the
        // handler's address.
        word vector = (word)((i_ << 8) | 0xFF);
        pc_ = (word)(pMemory_->read(vector) | (pMemory_->read((word)(vector + 1)) << 8));
        return 19;
    }
    }
    return 13;
}

// Runs the once-per-frame housekeeping: pace to real time, let the host draw,
// then pulse /INT the way the ULA does at the start of the vertical blank.
void Cpu::end_of_frame()
{
    if (realtime_) {
        auto current_time = std::chrono::high_resolution_clock::now();
        std::this_thread::sleep_for(time_to_sleep(cycle_count_, current_time, cycle_time_));
    }
    if (frame_callback_) frame_callback_(this);

    // Carry the overshoot into the next frame instead of dropping it, so the
    // T-state budget doesn't drift slow by up to one instruction per frame.
    cycle_count_ -= CYCLES_PER_FRAME;
    advance_cycles(interrupt());
    cycle_time_ = std::chrono::high_resolution_clock::now();
}

void Cpu::set_trap(word address, TrapCallback cb, int states)
{
    traps_[address] = Trap{ std::move(cb), states };
    trap_bitmap_.set(address);
}

void Cpu::clear_trap(word address)
{
    traps_.erase(address);
    trap_bitmap_.reset(address);
}

void Cpu::clear_all_traps()
{
    traps_.clear();
    trap_bitmap_.reset();
}

int Cpu::step() {
    if (halted_)
    {
        // HALT is internally a stream of NOPs until an interrupt arrives.
        advance_cycles(4);
        if (cycle_count_ >= CYCLES_PER_FRAME) end_of_frame();
        return pc_;
    }

    // A trap may stand in for the instruction at pc_ -- how ROM entry points
    // are intercepted. Checked before decode so that a trap works even at an
    // address where decoding would run past the end of memory. A callback
    // returning false declines, and the real instruction runs below.
    if (trap_bitmap_[pc_])
    {
        auto trap = traps_.find(pc_);
        if (trap != traps_.end() && trap->second.callback(this))
        {
            advance_cycles(trap->second.states);
            if (cycle_count_ >= CYCLES_PER_FRAME) end_of_frame();
            return pc_;
        }
    }

    auto L = decode(pMemory_->data() + pc_);
    // An instruction ending exactly on the last byte is legal, so this is a
    // strict '>': the top byte of the address space must be executable.
    if ((int)pc_ + (int)L.length > (int)MEMORY_SIZE)
    {
        halted_ = true; // Memory overflow, halt the CPU
        return pc_; // Memory overflow
    }

    // states() predicts variable timings -- DJNZ, conditional branches, the
    // block-repeat forms -- from the registers and flags as they stand
    // *before* the instruction runs, so it must be read here and not after
    // executeCommand() has moved them. DJNZ is the clearest case: its cost
    // depends on B, which it decrements.
    const int cost = states(L);

    bool result = executeCommand(L, pMemory_->data() + pc_);

    if (result) pc_ += (word)L.length;
    advance_cycles(cost);
    if (cycle_count_ >= CYCLES_PER_FRAME) end_of_frame();
    return pc_;
}

void Cpu::boot()
{
    // Initialize CPU registers and memory
    im = 1;
    iff1 = iff2 = true;
    i_ = 0x35;

    // Initialize memory
    pMemory_ = new Memory(MEMORY_SIZE);
}

RegisterState Cpu::get_registers() const
{
    RegisterState regs;
    regs.a  = a_;
    regs.f  = f_.f;
    regs.bc = bc_.bc;
    regs.de = de_.de;
    regs.hl = hl_.hl;
    regs.ix = ix_.ix;
    regs.iy = iy_.iy;
    regs.pc = pc_;
    regs.sp = sp_;
    regs.i  = i_;
    regs.r  = r_;
    regs.im = im;
    regs.iff1 = iff1;
    regs.iff2 = iff2;
    regs.halted = halted_;
    regs.a_alt = _a_;
    regs.f_alt = _f_;
    regs.bc_alt = _bc_.bc;
    regs.de_alt = _de_.de;
    regs.hl_alt = _hl_.hl;
    return regs;
}

void Cpu::set_registers(const RegisterState& regs)
{
    a_ = regs.a;
    f_.f = regs.f;
    bc_.bc = regs.bc;
    de_.de = regs.de;
    hl_.hl = regs.hl;
    ix_.ix = regs.ix;
    iy_.iy = regs.iy;
    pc_ = regs.pc;
    sp_ = regs.sp;
    i_ = regs.i;
    r_ = regs.r;
    im = regs.im;
    iff1 = regs.iff1;
    iff2 = regs.iff2;
    halted_ = regs.halted;
    _a_ = regs.a_alt;
    _f_ = regs.f_alt;
    _bc_.bc = regs.bc_alt;
    _de_.de = regs.de_alt;
    _hl_.hl = regs.hl_alt;
}

Layout Cpu::decode(const byte *p)
{
    Layout L;
    const uint8_t first = p[0];

    if (first == PFX_DD || first == PFX_FD) {
        const uint8_t next = p[1];

        // A run of index prefixes: only the last one takes effect. Earlier
        // ones burn 4 T-states and are otherwise inert, so report them as
        // 1-byte pseudo-instructions and let the caller decode from p+1.
        // DD/FD followed by ED behaves the same way.
        if (next == PFX_DD || next == PFX_FD || next == PFX_ED) {
            L.page   = Page::IgnoredPrefix;
            L.opcode = first;
            L.length = 1;
            return L;
        }

        L.index     = first;
        L.prefixLen = 1;

        if (next == PFX_CB) {              // DD CB d op  - always 4 bytes,
            L.page      = Page::IndexedCB; // and d comes BEFORE the opcode
            L.prefixLen = 2;
            L.dispOff   = 2;
            L.opcodeOff = 3;
            L.opcode    = p[3];
            L.length    = 4;
            return L;
        }

        L.page      = Page::Indexed;
        L.opcodeOff = 1;
        L.opcode    = next;
        uint8_t len = 2;
        if (kIndexed[next]) {
            L.dispOff = static_cast<int8_t>(len);
            ++len;
        }
        if (kBaseImm[next]) {
            L.immOff = static_cast<int8_t>(len);
            L.immLen = kBaseImm[next];
            len += L.immLen;
        }
        L.length = len;
        return L;
    }

    if (first == PFX_CB) {                 // CB op - the one uniform page
        L.page      = Page::CB;
        L.prefixLen = 1;
        L.opcodeOff = 1;
        L.opcode    = p[1];
        L.length    = 2;
        return L;
    }

    if (first == PFX_ED) {                 // ED op [nn]
        L.page      = Page::ED;
        L.prefixLen = 1;
        L.opcodeOff = 1;
        L.opcode    = p[1];
        L.length    = 2;
        if ((p[1] & 0xC7) == 0x43) {       // LD (nn),rp and LD rp,(nn)
            L.immOff = 2;
            L.immLen = 2;
            L.length = 4;
        }
        return L;
    }

    L.page   = Page::Base;                 // op [n|nn]
    L.opcode = first;
    L.immLen = kBaseImm[first];
    if (L.immLen) L.immOff = 1;
    L.length = static_cast<uint8_t>(1 + L.immLen);
    return L;
}

uint16_t Cpu::immediate(const byte *p, const Layout &L)
{
    if (L.immLen == 1) return p[L.immOff];
    if (L.immLen == 2) return static_cast<uint16_t>(p[L.immOff] | (p[L.immOff + 1] << 8));
    return 0;
}

int8_t Cpu::displacement(const byte *p, const Layout &L)
{
    return L.dispOff < 0 ? 0 : static_cast<int8_t>(p[L.dispOff]);
}

uint16_t Cpu::from_reg(word value)
{
    union {
        struct {
            byte msb;
            byte lsb;
        };
        word val;
    } reg;

    reg.val = value;
    return static_cast<uint16_t>(reg.msb | (reg.lsb << 8));
}

bool Cpu::try_decode(const byte *p, Layout &L)
{
    L = decode(p);

    // The whole instruction must fit within memory.
    ptrdiff_t addr = p - pMemory_->data();
    if (addr < 0 || addr + L.length > static_cast<ptrdiff_t>(MEMORY_SIZE))
    {
        return false;
    }

    if (L.length < 1 || L.length > kMaxInstructionLength)
    {
        return false;
    }

    // Check that opcodeOff/dispOff/immOff/immLen/prefixLen/index all agree
    // with each other and with `length`, per the shape decode() should have
    // produced for this page.
    switch (L.page)
    {
    case Page::Base:
        if (L.prefixLen != 0 || L.opcodeOff != 0 || L.dispOff != -1 || L.index != 0)
            return false;
        if (L.immLen > 2)
            return false;
        if ((L.immLen == 0 && L.immOff != -1) || (L.immLen > 0 && L.immOff != 1))
            return false;
        if (L.length != 1 + L.immLen)
            return false;
        break;

    case Page::CB:
        if (L.prefixLen != 1 || L.opcodeOff != 1 || L.dispOff != -1 ||
            L.immOff != -1 || L.immLen != 0 || L.index != 0)
            return false;
        if (L.length != 2)
            return false;
        break;

    case Page::ED:
        if (L.prefixLen != 1 || L.opcodeOff != 1 || L.dispOff != -1 || L.index != 0)
            return false;
        if (L.immLen == 0)
        {
            if (L.immOff != -1 || L.length != 2)
                return false;
        }
        else if (L.immLen == 2)
        {
            if (L.immOff != 2 || L.length != 4)
                return false;
        }
        else
        {
            return false;
        }
        break;

    case Page::Indexed:
    {
        if (L.prefixLen != 1 || L.opcodeOff != 1)
            return false;
        if (L.index != PFX_DD && L.index != PFX_FD)
            return false;
        if (L.immLen > 2)
            return false;
        if (L.dispOff >= 0 && L.dispOff != 2)
            return false;
        int expectedImmOff = (L.dispOff >= 0) ? 3 : 2;
        if ((L.immLen == 0 && L.immOff != -1) || (L.immLen > 0 && L.immOff != expectedImmOff))
            return false;
        int expectedLen = 2 + (L.dispOff >= 0 ? 1 : 0) + L.immLen;
        if (L.length != expectedLen)
            return false;
        break;
    }

    case Page::IndexedCB:
        if (L.length != 4 || L.prefixLen != 2 || L.dispOff != 2 ||
            L.opcodeOff != 3 || L.immOff != -1 || L.immLen != 0)
            return false;
        if (L.index != PFX_DD && L.index != PFX_FD)
            return false;
        break;

    case Page::IgnoredPrefix:
        if (L.length != 1 || L.prefixLen != 0 || L.opcodeOff != 0 ||
            L.dispOff != -1 || L.immOff != -1 || L.immLen != 0)
            return false;
        if (L.opcode != PFX_DD && L.opcode != PFX_FD)
            return false;
        break;

    default:
        return false;
    }

    return true;
}

byte* Cpu::find_prev_instruction(Layout &L, byte *pCurrent)
{
    ptrdiff_t currentAddr = pCurrent - pMemory_->data();

    // A variable-length instruction set has no explicit boundaries, so the
    // only reliable check is: does decoding from this candidate start
    // produce an instruction whose length lands exactly on pCurrent?
    for (ptrdiff_t back = 1;
         back <= static_cast<ptrdiff_t>(kMaxInstructionLength) && currentAddr - back >= 0;
         ++back)
    {
        byte *candidate = pCurrent - back;
        Layout candidateLayout;
        if (try_decode(candidate, candidateLayout) && candidateLayout.length == back)
        {
            L = candidateLayout;
            return candidate;
        }
    }

    return nullptr;
}

/// T-states for a Base-page opcode, including the conditional forms whose
/// cost depends on whether the branch is taken. Split out so that the indexed
/// pages can reuse it: a DD/FD prefix in front of an opcode that has nothing
/// to do with HL is simply an ignored 4-T-state fetch, after which the
/// instruction runs -- and times -- exactly as it would on its own.
uint8_t Cpu::base_states(byte op)
{
    if (op != 0x76 && op >= 0x40 && op <= 0xBF)
    {
        // LD r,r' (0x40-0x7F) / 8-bit ALU (0x80-0xBF): 4 T-states for a
        // plain register operand, 7 when either side is (HL) (code 6).
        bool touchesHL = ((op & 0x07) == 6) || (op < 0x80 && ((op >> 3) & 0x07) == 6);
        return touchesHL ? 7 : 4;
    }

    switch (op)
    {
    case 0x10: return (bc_.b != 1) ? 13 : 8;                          // djnz e
    case 0x20: return !f_.zero  ? 12 : 7;                             // jr nz,e
    case 0x28: return  f_.zero  ? 12 : 7;                             // jr z,e
    case 0x30: return !f_.carry ? 12 : 7;                             // jr nc,e
    case 0x38: return  f_.carry ? 12 : 7;                             // jr c,e
    case 0xC0: return check_condition(0) ? 11 : 5;                    // ret nz
    case 0xC8: return check_condition(1) ? 11 : 5;                    // ret z
    case 0xD0: return check_condition(2) ? 11 : 5;                    // ret nc
    case 0xD8: return check_condition(3) ? 11 : 5;                    // ret c
    case 0xE0: return check_condition(4) ? 11 : 5;                    // ret po
    case 0xE8: return check_condition(5) ? 11 : 5;                    // ret pe
    case 0xF0: return check_condition(6) ? 11 : 5;                    // ret p
    case 0xF8: return check_condition(7) ? 11 : 5;                    // ret m
    case 0xC4: return check_condition(0) ? 17 : 10;                   // call nz,nn
    case 0xCC: return check_condition(1) ? 17 : 10;                   // call z,nn
    case 0xD4: return check_condition(2) ? 17 : 10;                   // call nc,nn
    case 0xDC: return check_condition(3) ? 17 : 10;                   // call c,nn
    case 0xE4: return check_condition(4) ? 17 : 10;                   // call po,nn
    case 0xEC: return check_condition(5) ? 17 : 10;                   // call pe,nn
    case 0xF4: return check_condition(6) ? 17 : 10;                   // call p,nn
    case 0xFC: return check_condition(7) ? 17 : 10;                   // call m,nn
    default:   return kBaseStates[op];
    }
}

uint8_t Cpu::states(Layout &layout)
{
    byte op = layout.opcode;

    switch (layout.page)
    {
    case Page::Base:
        return base_states(op);

    case Page::CB:
    {
        bool isHL = (op & 0x07) == 6;
        if (op >= 0x40 && op <= 0x7F) return isHL ? 12 : 8;               // bit b,r
        return isHL ? 15 : 8;                                             // rotate/shift/res/set
    }

    case Page::ED:
    {
        // Every genuinely-defined ED opcode is enumerated below; anything
        // else is undefined on real hardware and takes 8 T-states, same as
        // executeED()'s NOP-equivalent handling of those opcodes.
        if (op >= 0x40 && op <= 0x7F)
        {
            switch (op & 0x07)
            {
            case 0: return 12; // in r,(c)
            case 1: return 12; // out (c),r
            case 2: return 15; // sbc/adc hl,rr
            case 3: return 20; // ld (nn),rr / ld rr,(nn)
            case 4: return 8;  // neg
            case 5: return 14; // retn/reti
            case 6: return 8;  // im 0/1/2
            default:
            {
                uint8_t y = (op >> 3) & 0x07;
                return (y == 4 || y == 5) ? 18 : 8; // rrd/rld vs ld i,a/ld r,a/ld a,i/ld a,r/undoc
            }
            }
        }

        switch (op)
        {
        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
            return 16; // ldi/cpi/ini/outi/ldd/cpd/ind/outd
        case 0xB0: case 0xB8: // ldir/lddr
            return (bc_.bc != 1) ? 21 : 16;
        case 0xB1: case 0xB9: // cpir/cpdr
        {
            byte value = pMemory_->data()[from_reg(hl_.hl)];
            bool wouldMatch = (a_ == value);
            return (bc_.bc != 1 && !wouldMatch) ? 21 : 16;
        }
        case 0xB2: case 0xBA: case 0xB3: case 0xBB: // inir/indr/otir/otdr
            return (bc_.b != 1) ? 21 : 16;
        default:
            return 8; // undefined ED opcode
        }
    }

    case Page::Indexed:
    {
        switch (op)
        {
        case 0x21: return 14; // ld ix/iy,nn
        case 0x22: return 20; // ld (nn),ix/iy
        case 0x23: return 10; // inc ix/iy
        case 0x2A: return 20; // ld ix/iy,(nn)
        case 0x2B: return 10; // dec ix/iy
        case 0x09: case 0x19: case 0x29: case 0x39: return 15; // add ix/iy,rr
        case 0x24: case 0x25: case 0x2C: case 0x2D: return 8;  // inc/dec ixh/ixl/iyh/iyl (undoc)
        case 0x26: case 0x2E: return 11;                       // ld ixh/ixl/iyh/iyl,n (undoc)
        case 0x34: case 0x35: return 23;                       // inc/dec (ix/iy+d)
        case 0x36: return 19;                                  // ld (ix/iy+d),n
        case 0xE1: return 14;                                  // pop ix/iy
        case 0xE3: return 23;                                  // ex (sp),ix/iy
        case 0xE5: return 15;                                  // push ix/iy
        case 0xE9: return 8;                                   // jp (ix/iy)
        case 0xF9: return 10;                                  // ld sp,ix/iy
        default:
            if (op != 0x76 && op >= 0x40 && op <= 0xBF)
            {
                // LD r,(ix/iy+d) / ALU A,(ix/iy+d) vs a plain register form
                // (including the undocumented IXH/IXL/IYH/IYL slots, which
                // never touch memory and so cost the same as any other
                // register-only indexed opcode).
                bool touchesMem = ((op & 0x07) == 6) || (op < 0x80 && ((op >> 3) & 0x07) == 6);
                return touchesMem ? 19 : 8;
            }
            // Doesn't reference HL/(HL) at all: the prefix is ignored, so this
            // is the Base-page instruction plus the 4 T-states the discarded
            // fetch still costs. base_states() rather than the flat table,
            // because conditional forms still branch and still vary.
            return static_cast<uint8_t>(base_states(op) + 4);
        }
    }

    case Page::IndexedCB:
        return (op >= 0x40 && op <= 0x7F) ? 20 : 23; // bit vs rotate/shift/res/set on (ix/iy+d)

    case Page::IgnoredPrefix:
        return 4; // a stacked DD/FD/ED prefix that gets overridden by the next one
    }

    return 0; // unreachable if Page is exhaustively handled above
}

bool Cpu::executeCommand(Layout &layout, byte *pCmd)
{
    switch (layout.page)
    {
    case Page::Base:          return executeBase(layout, pCmd);
    case Page::CB:            return executeCB(layout, pCmd);
    case Page::ED:            return executeED(layout, pCmd);
    case Page::Indexed:       return executeIndexed(layout, pCmd);
    case Page::IndexedCB:     return executeIndexedCB(layout, pCmd);
    case Page::IgnoredPrefix: return true;
    }
    return false; // unreachable if Page is exhaustively handled above
}

bool Cpu::executeBase(Layout &layout, byte *pCmd)
{
    switch (layout.opcode)
    {
    case 0x00: // nop
        return true;

    case 0x01: // ld bc,NN
        bc_.bc = immediate(pCmd, layout);
        return true;

    case 0x02: // ld (bc),a
    {
        word offset = from_reg(bc_.bc);
        pMemory_->write(offset, a_);
        return true;
    }

    case 0x03: // inc bc
        execute_inc(bc_.bc);
        return true;

    case 0x04: // inc b
        execute_inc(bc_.b);
        return true;

    case 0x05: // dec b
        execute_dec(bc_.b);
        return true;

    case 0x06: // ld b,n
        bc_.b = immediate(pCmd, layout);
        return true;

    case 0x07: // rlca
    {
        auto z = f_.zero;
        auto s = f_.minus;
        auto o = f_.parity_overflow;
        execute_rlc(a_);
        f_.zero = z;
        f_.minus = s;
        f_.parity_overflow = o;
        return true;
    }

    case 0x08: // ex af,af'
        std::swap<byte>(a_, _a_);
        std::swap<byte>(f_.f, _f_);
        return true;

    case 0x09: // add hl,bc
        execute_add_r2_to_r1(hl_.hl, bc_.bc);
        return true;

    case 0x0A: // ld a,(bc)
    {
        word offset = from_reg(bc_.bc);
        a_ = pMemory_->data()[offset];
        return true;
    }

    case 0x0B: // dec bc
        execute_dec(bc_.bc);
        return true;

    case 0x0C: // inc c
        execute_inc(bc_.c);
        return true;

    case 0x0D: // dec c
        execute_dec(bc_.c);
        return true;

    case 0x0E: // ld c,n
        bc_.c = immediate(pCmd, layout);
        return true;

    case 0x0F: // rrca
    {
        auto z = f_.zero;
        auto s = f_.minus;
        auto o = f_.parity_overflow;
        execute_rrc(a_);
        f_.zero = z;
        f_.minus = s;
        f_.parity_overflow = o;
        return true;
    }

    case 0x10: // djnz e
    {
        bc_.b--; // unconditional wraparound decrement (0x00 -> 0xFF)
        if (bc_.b != 0)
        {
            int8_t offset = static_cast<int8_t>(immediate(pCmd, layout));
            pc_ = static_cast<word>(pc_ + layout.length + offset);
            return false; // pc_ already set to the jump target
        }
        return true; // B==0: fall through, let step() advance pc_ normally
    }

    case 0x11: // ld de,NN
        de_.de = immediate(pCmd, layout);
        return true;

    case 0x12: // ld (de),a
    {
        word offset = from_reg(de_.de);
        pMemory_->write(offset, a_);
        return true;
    }

    case 0x13: // inc de
        execute_inc(de_.de);
        return true;

    case 0x14: // inc d
        execute_inc(de_.d);
        return true;

    case 0x15: // dec d
        execute_dec(de_.d);
        return true;

    case 0x16: // ld d,n
        de_.d = immediate(pCmd, layout);
        return true;

    case 0x17: // rla
    {
        bool bZero = f_.zero;
        bool bSign = f_.minus;
        bool bParity = f_.parity_overflow;
        execute_rl(a_);
        set_flags(f_.carry, f_.add_subtract, bSign, f_.half_carry, bZero, bParity);
        return true;
    }

    case 0x18: // jr e
    {
        int8_t offset = static_cast<int8_t>(immediate(pCmd, layout));
        pc_ = static_cast<word>(pc_ + layout.length + offset);
        return false; // pc_ already set to the jump target
    }

    case 0x19: // add hl,de
        execute_add_r2_to_r1(hl_.hl, de_.de);
        return true;

    case 0x1A: // ld a,(de)
    {
        word offset = from_reg(de_.de);
        a_ = pMemory_->data()[offset];
        return true;
    }

    case 0x1B: // dec de
        execute_dec(de_.de);
        return true;

    case 0x1C: // inc e
        execute_inc(de_.e);
        return true;

    case 0x1D: // dec e
        execute_dec(de_.e);
        return true;

    case 0x1E: // ld e,n
        de_.e = immediate(pCmd, layout);
        return true;

    case 0x1F: // rra
    {
        bool bZero = f_.zero;
        bool bSign = f_.minus;
        bool bParity = f_.parity_overflow;
        execute_rr(a_);
        set_flags(f_.carry, f_.add_subtract, bSign, f_.half_carry, bZero, bParity);
        return true;
    }

    case 0x20: // jr nz,e
        if (!f_.zero)
        {
            int8_t offset = static_cast<int8_t>(immediate(pCmd, layout));
            pc_ = static_cast<word>(pc_ + layout.length + offset);
            return false;
        }
        return true;

    case 0x21: // ld hl,NN
        hl_.hl = immediate(pCmd, layout);
        return true;

    case 0x22: // ld (nn),hl
    {
        word offset = from_reg(immediate(pCmd, layout));
        pMemory_->write(offset, hl_.l);
        pMemory_->write(static_cast<word>(offset + 1), hl_.h);
        return true;
    }

    case 0x23: // inc hl
        execute_inc(hl_.hl);
        return true;

    case 0x24: // inc h
        execute_inc(hl_.h);
        return true;

    case 0x25: // dec h
        execute_dec(hl_.h);
        return true;

    case 0x26: // ld h,n
        hl_.h = immediate(pCmd, layout);
        return true;

    case 0x27: // daa
        execute_daa();
        return true;

    case 0x28: // jr z,e
        if (f_.zero)
        {
            int8_t offset = static_cast<int8_t>(immediate(pCmd, layout));
            pc_ = static_cast<word>(pc_ + layout.length + offset);
            return false;
        }
        return true;

    case 0x29: // add hl,hl
        execute_add_r2_to_r1(hl_.hl, hl_.hl);
        return true;

    case 0x2A: // ld hl,(nn)
    {
        word addr = from_reg(immediate(pCmd, layout));
        hl_.l = pMemory_->data()[addr];
        hl_.h = pMemory_->data()[addr + 1];
        return true;
    }

    case 0x2B: // dec hl
        execute_dec(hl_.hl);
        return true;

    case 0x2C: // inc l
        execute_inc(hl_.l);
        return true;

    case 0x2D: // dec l
        execute_dec(hl_.l);
        return true;

    case 0x2E: // ld l,n
        hl_.l = immediate(pCmd, layout);
        return true;

    case 0x2F: // cpl
        a_ = static_cast<byte>(~a_);
        f_.half_carry = 1;
        f_.add_subtract = 1;
        return true;

    case 0x30: // jr nc,e
        if (!f_.carry)
        {
            int8_t offset = static_cast<int8_t>(immediate(pCmd, layout));
            pc_ = static_cast<word>(pc_ + layout.length + offset);
            return false;
        }
        return true;

    case 0x31: // ld sp,nn
        sp_ = immediate(pCmd, layout);
        return true;

    case 0x32: // ld (nn),a
    {
        word addr = from_reg(immediate(pCmd, layout));
        pMemory_->write(addr, a_);
        return true;
    }

    case 0x33: // inc sp
        execute_inc(sp_);
        return true;

    case 0x34: // inc (hl)
    {
        word offset = from_reg(hl_.hl);
        execute_inc(*pMemory_->writable(offset));
        return true;
    }

    case 0x35: // dec (hl)
    {
        word offset = from_reg(hl_.hl);
        execute_dec(*pMemory_->writable(offset));
        return true;
    }

    case 0x36: // ld (hl),n
    {
        word offset = from_reg(hl_.hl);
        pMemory_->write(offset, static_cast<byte>(immediate(pCmd, layout)));
        return true;
    }

    case 0x37: // scf
        f_.carry = 1;
        f_.half_carry = 0;
        f_.add_subtract = 0;
        return true;

    case 0x38: // jr c,e
        if (f_.carry)
        {
            int8_t offset = static_cast<int8_t>(immediate(pCmd, layout));
            pc_ = static_cast<word>(pc_ + layout.length + offset);
            return false;
        }
        return true;

    case 0x39: // add hl,sp
        execute_add_r2_to_r1(hl_.hl, sp_);
        return true;

    case 0x3A: // ld a,(nn)
    {
        word addr = from_reg(immediate(pCmd, layout));
        a_ = pMemory_->data()[addr];
        return true;
    }

    case 0x3B: // dec sp
        execute_dec(sp_);
        return true;

    case 0x3C: // inc a
        execute_inc(a_);
        return true;

    case 0x3D: // dec a
        execute_dec(a_);
        return true;

    case 0x3E: // ld a,n
        a_ = immediate(pCmd, layout);
        return true;

    case 0x3F: // ccf
    {
        bool oldCarry = f_.carry;
        f_.half_carry = oldCarry;
        f_.carry = !oldCarry;
        f_.add_subtract = false;
        return true;
    }

    case 0x76: // halt (the "LD (HL),(HL)" slot on real hardware is HALT instead)
        halted_ = true;
        return true;

    case 0xC0: // ret nz
        if (check_condition(0)) { pc_ = pop_word(); return false; }
        return true;
    case 0xC1: // pop bc
        bc_.bc = pop_word();
        return true;
    case 0xC2: // jp nz,nn
        if (check_condition(0)) { pc_ = immediate(pCmd, layout); return false; }
        return true;
    case 0xC3: // jp nn
        pc_ = immediate(pCmd, layout);
        return false;
    case 0xC4: // call nz,nn
        if (check_condition(0))
        {
            push_word(static_cast<word>(pc_ + layout.length));
            pc_ = immediate(pCmd, layout);
            return false;
        }
        return true;
    case 0xC5: // push bc
        push_word(bc_.bc);
        return true;
    case 0xC6: // add a,n
        execute_alu(AluOp::ADD, static_cast<byte>(immediate(pCmd, layout)));
        return true;
    case 0xC7: // rst 00h
        push_word(static_cast<word>(pc_ + layout.length));
        pc_ = 0x00;
        return false;
    case 0xC8: // ret z
        if (check_condition(1)) { pc_ = pop_word(); return false; }
        return true;
    case 0xC9: // ret
        pc_ = pop_word();
        return false;
    case 0xCA: // jp z,nn
        if (check_condition(1)) { pc_ = immediate(pCmd, layout); return false; }
        return true;
    case 0xCC: // call z,nn
        if (check_condition(1))
        {
            push_word(static_cast<word>(pc_ + layout.length));
            pc_ = immediate(pCmd, layout);
            return false;
        }
        return true;
    case 0xCD: // call nn
        push_word(static_cast<word>(pc_ + layout.length));
        pc_ = immediate(pCmd, layout);
        return false;
    case 0xCE: // adc a,n
        execute_alu(AluOp::ADC, static_cast<byte>(immediate(pCmd, layout)));
        return true;
    case 0xCF: // rst 08h
        push_word(static_cast<word>(pc_ + layout.length));
        pc_ = 0x08;
        return false;

    case 0xD0: // ret nc
        if (check_condition(2)) { pc_ = pop_word(); return false; }
        return true;
    case 0xD1: // pop de
        de_.de = pop_word();
        return true;
    case 0xD2: // jp nc,nn
        if (check_condition(2)) { pc_ = immediate(pCmd, layout); return false; }
        return true;
    case 0xD3: // out (n),a
    {
        word port = static_cast<word>((a_ << 8) | immediate(pCmd, layout));
        port_out(port, a_);
        return true;
    }
    case 0xD4: // call nc,nn
        if (check_condition(2))
        {
            push_word(static_cast<word>(pc_ + layout.length));
            pc_ = immediate(pCmd, layout);
            return false;
        }
        return true;
    case 0xD5: // push de
        push_word(de_.de);
        return true;
    case 0xD6: // sub n
        execute_alu(AluOp::SUB, static_cast<byte>(immediate(pCmd, layout)));
        return true;
    case 0xD7: // rst 10h
        push_word(static_cast<word>(pc_ + layout.length));
        pc_ = 0x10;
        return false;
    case 0xD8: // ret c
        if (check_condition(3)) { pc_ = pop_word(); return false; }
        return true;
    case 0xD9: // exx
        std::swap(bc_.bc, _bc_.bc);
        std::swap(de_.de, _de_.de);
        std::swap(hl_.hl, _hl_.hl);
        return true;
    case 0xDA: // jp c,nn
        if (check_condition(3)) { pc_ = immediate(pCmd, layout); return false; }
        return true;
    case 0xDB: // in a,(n)
    {
        word port = static_cast<word>((a_ << 8) | immediate(pCmd, layout));
        a_ = port_in(port);
        return true;
    }
    case 0xDC: // call c,nn
        if (check_condition(3))
        {
            push_word(static_cast<word>(pc_ + layout.length));
            pc_ = immediate(pCmd, layout);
            return false;
        }
        return true;
    case 0xDE: // sbc a,n
        execute_alu(AluOp::SBC, static_cast<byte>(immediate(pCmd, layout)));
        return true;
    case 0xDF: // rst 18h
        push_word(static_cast<word>(pc_ + layout.length));
        pc_ = 0x18;
        return false;

    case 0xE0: // ret po
        if (check_condition(4)) { pc_ = pop_word(); return false; }
        return true;
    case 0xE1: // pop hl
        hl_.hl = pop_word();
        return true;
    case 0xE2: // jp po,nn
        if (check_condition(4)) { pc_ = immediate(pCmd, layout); return false; }
        return true;
    case 0xE3: // ex (sp),hl
    {
        word temp = static_cast<word>(pMemory_->data()[sp_] | (pMemory_->data()[sp_ + 1] << 8));
        pMemory_->write(sp_, hl_.l);
        pMemory_->write(static_cast<word>(sp_ + 1), hl_.h);
        hl_.hl = temp;
        return true;
    }
    case 0xE4: // call po,nn
        if (check_condition(4))
        {
            push_word(static_cast<word>(pc_ + layout.length));
            pc_ = immediate(pCmd, layout);
            return false;
        }
        return true;
    case 0xE5: // push hl
        push_word(hl_.hl);
        return true;
    case 0xE6: // and n
        execute_alu(AluOp::AND, static_cast<byte>(immediate(pCmd, layout)));
        return true;
    case 0xE7: // rst 20h
        push_word(static_cast<word>(pc_ + layout.length));
        pc_ = 0x20;
        return false;
    case 0xE8: // ret pe
        if (check_condition(5)) { pc_ = pop_word(); return false; }
        return true;
    case 0xE9: // jp (hl)
        pc_ = hl_.hl;
        return false;
    case 0xEA: // jp pe,nn
        if (check_condition(5)) { pc_ = immediate(pCmd, layout); return false; }
        return true;
    case 0xEB: // ex de,hl
        std::swap(de_.de, hl_.hl);
        return true;
    case 0xEC: // call pe,nn
        if (check_condition(5))
        {
            push_word(static_cast<word>(pc_ + layout.length));
            pc_ = immediate(pCmd, layout);
            return false;
        }
        return true;
    case 0xEE: // xor n
        execute_alu(AluOp::XOR, static_cast<byte>(immediate(pCmd, layout)));
        return true;
    case 0xEF: // rst 28h
        push_word(static_cast<word>(pc_ + layout.length));
        pc_ = 0x28;
        return false;

    case 0xF0: // ret p
        if (check_condition(6)) { pc_ = pop_word(); return false; }
        return true;
    case 0xF1: // pop af
    {
        word value = pop_word();
        a_ = static_cast<byte>((value >> 8) & 0xFF);
        f_.f = static_cast<byte>(value & 0xFF);
        return true;
    }
    case 0xF2: // jp p,nn
        if (check_condition(6)) { pc_ = immediate(pCmd, layout); return false; }
        return true;
    case 0xF3: // di
        iff1 = iff2 = false;
        return true;
    case 0xF4: // call p,nn
        if (check_condition(6))
        {
            push_word(static_cast<word>(pc_ + layout.length));
            pc_ = immediate(pCmd, layout);
            return false;
        }
        return true;
    case 0xF5: // push af
        push_word(static_cast<word>((a_ << 8) | f_.f));
        return true;
    case 0xF6: // or n
        execute_alu(AluOp::OR, static_cast<byte>(immediate(pCmd, layout)));
        return true;
    case 0xF7: // rst 30h
        push_word(static_cast<word>(pc_ + layout.length));
        pc_ = 0x30;
        return false;
    case 0xF8: // ret m
        if (check_condition(7)) { pc_ = pop_word(); return false; }
        return true;
    case 0xF9: // ld sp,hl
        sp_ = hl_.hl;
        return true;
    case 0xFA: // jp m,nn
        if (check_condition(7)) { pc_ = immediate(pCmd, layout); return false; }
        return true;
    case 0xFB: // ei
        iff1 = iff2 = true;
        return true;
    case 0xFC: // call m,nn
        if (check_condition(7))
        {
            push_word(static_cast<word>(pc_ + layout.length));
            pc_ = immediate(pCmd, layout);
            return false;
        }
        return true;
    case 0xFE: // cp n
        execute_alu(AluOp::CP, static_cast<byte>(immediate(pCmd, layout)));
        return true;
    case 0xFF: // rst 38h
        push_word(static_cast<word>(pc_ + layout.length));
        pc_ = 0x38;
        return false;

    default:
        // LD r,r' (0x40-0x7F, excluding 0x76=HALT above) and the 8-bit
        // ALU-with-A block (0x80-0xBF) both encode {dst/op, src} directly in
        // the opcode byte, so they're handled generically via resolve_r
        // rather than as ~127 individual cases.
        if (layout.opcode >= 0x40 && layout.opcode <= 0x7F)
        {
            uint8_t dst = (layout.opcode >> 3) & 0x07;
            uint8_t src = layout.opcode & 0x07;
            byte value = *resolve_r(layout, pCmd, src);
            *resolve_r(layout, pCmd, dst) = value;
            return true;
        }
        if (layout.opcode >= 0x80 && layout.opcode <= 0xBF)
        {
            AluOp op = static_cast<AluOp>((layout.opcode >> 3) & 0x07);
            byte operand = *resolve_r(layout, pCmd, layout.opcode & 0x07);
            execute_alu(op, operand);
            return true;
        }
#ifndef NDEBUG
        assert(false && "unimplemented Z80 Base-page opcode");
#endif
        return false;
    }
}

bool Cpu::executeCB(Layout &layout, byte *pCmd)
{
    switch (layout.opcode)
    {
    case 0x00: execute_rlc(bc_.b); return true;
    case 0x01: execute_rlc(bc_.c); return true;
    case 0x02: execute_rlc(de_.d); return true;
    case 0x03: execute_rlc(de_.e); return true;
    case 0x04: execute_rlc(hl_.h); return true;
    case 0x05: execute_rlc(hl_.l); return true;
    case 0x06:
    {
        word offset = from_reg(hl_.hl);
        execute_rlc(*pMemory_->writable(offset));
        return true;
    }
    case 0x07: execute_rlc(a_); return true;

    case 0x08: execute_rrc(bc_.b); return true;
    case 0x09: execute_rrc(bc_.c); return true;
    case 0x0A: execute_rrc(de_.d); return true;
    case 0x0B: execute_rrc(de_.e); return true;
    case 0x0C: execute_rrc(hl_.h); return true;
    case 0x0D: execute_rrc(hl_.l); return true;
    case 0x0E:
    {
        word offset = from_reg(hl_.hl);
        execute_rrc(*pMemory_->writable(offset));
        return true;
    }
    case 0x0F: execute_rrc(a_); return true;

    case 0x10: execute_rl(bc_.b); return true;
    case 0x11: execute_rl(bc_.c); return true;
    case 0x12: execute_rl(de_.d); return true;
    case 0x13: execute_rl(de_.e); return true;
    case 0x14: execute_rl(hl_.h); return true;
    case 0x15: execute_rl(hl_.l); return true;
    case 0x16:
    {
        word offset = from_reg(hl_.hl);
        byte val = pMemory_->data()[offset];
        execute_rl(val);
        pMemory_->write(offset, val);
        return true;
    }
    case 0x17: execute_rl(a_); return true;

    case 0x18: execute_rr(bc_.b); return true;
    case 0x19: execute_rr(bc_.c); return true;
    case 0x1A: execute_rr(de_.d); return true;
    case 0x1B: execute_rr(de_.e); return true;
    case 0x1C: execute_rr(hl_.h); return true;
    case 0x1D: execute_rr(hl_.l); return true;
    case 0x1E:
    {
        word offset = from_reg(hl_.hl);
        byte val = pMemory_->data()[offset];
        execute_rr(val);
        pMemory_->write(offset, val);
        return true;
    }
    case 0x1F: execute_rr(a_); return true;

    case 0x20: execute_sla(bc_.b); return true;
    case 0x21: execute_sla(bc_.c); return true;
    case 0x22: execute_sla(de_.d); return true;
    case 0x23: execute_sla(de_.e); return true;
    case 0x24: execute_sla(hl_.h); return true;
    case 0x25: execute_sla(hl_.l); return true;
    case 0x26:
    {
        word offset = from_reg(hl_.hl);
        byte val = pMemory_->data()[offset];
        execute_sla(val);
        pMemory_->write(offset, val);
        return true;
    }
    case 0x27: execute_sla(a_); return true;

    case 0x28: execute_sra(bc_.b); return true;
    case 0x29: execute_sra(bc_.c); return true;
    case 0x2A: execute_sra(de_.d); return true;
    case 0x2B: execute_sra(de_.e); return true;
    case 0x2C: execute_sra(hl_.h); return true;
    case 0x2D: execute_sra(hl_.l); return true;
    case 0x2E:
    {
        word offset = from_reg(hl_.hl);
        byte val = pMemory_->data()[offset];
        execute_sra(val);
        pMemory_->write(offset, val);
        return true;
    }
    case 0x2F: execute_sra(a_); return true;

    case 0x30: execute_sll(bc_.b); return true; // undocumented
    case 0x31: execute_sll(bc_.c); return true;
    case 0x32: execute_sll(de_.d); return true;
    case 0x33: execute_sll(de_.e); return true;
    case 0x34: execute_sll(hl_.h); return true;
    case 0x35: execute_sll(hl_.l); return true;
    case 0x36:
    {
        word offset = from_reg(hl_.hl);
        byte val = pMemory_->data()[offset];
        execute_sll(val);
        pMemory_->write(offset, val);
        return true;
    }
    case 0x37: execute_sll(a_); return true;

    case 0x38: execute_srl(bc_.b); return true;
    case 0x39: execute_srl(bc_.c); return true;
    case 0x3A: execute_srl(de_.d); return true;
    case 0x3B: execute_srl(de_.e); return true;
    case 0x3C: execute_srl(hl_.h); return true;
    case 0x3D: execute_srl(hl_.l); return true;
    case 0x3E:
    {
        word offset = from_reg(hl_.hl);
        byte val = pMemory_->data()[offset];
        execute_srl(val);
        pMemory_->write(offset, val);
        return true;
    }
    case 0x3F: execute_srl(a_); return true;

    default:
        // BIT b,r (0x40-0x7F) / RES b,r (0x80-0xBF) / SET b,r (0xC0-0xFF):
        // register/bit both come directly from the opcode byte, so these
        // 192 combinations are handled generically rather than as 192 cases.
        if (layout.opcode >= 0x40)
        {
            uint8_t bit = (layout.opcode >> 3) & 0x07;
            uint8_t regCode = layout.opcode & 0x07;
            byte *ptr = resolve_r(layout, pCmd, regCode);
            if (layout.opcode < 0x80)
            {
                execute_bit(*ptr, bit);
            }
            else if (layout.opcode < 0xC0)
            {
                execute_res(*ptr, bit);
            }
            else
            {
                execute_set(*ptr, bit);
            }
            return true;
        }
#ifndef NDEBUG
        assert(false && "unimplemented Z80 CB-page opcode");
#endif
        return false;
    }
}

bool Cpu::executeED(Layout &layout, byte *pCmd)
{
    switch (layout.opcode)
    {
    // ---- IN r,(C) / OUT (C),r (0x40-0x7F, opcode&7==0 or 1) --------------
    // 0x70 is the undocumented "IN (C)" (sets flags, stores nowhere);
    // 0x71 is the undocumented "OUT (C),0".
    case 0x40: case 0x48: case 0x50: case 0x58:
    case 0x60: case 0x68: case 0x70: case 0x78:
    {
        byte value = port_in(bc_.bc);
        uint8_t y = (layout.opcode >> 3) & 0x07;
        if (y != 6) *resolve_r(layout, pCmd, y) = value;
        f_.zero = (value == 0);
        f_.minus = (value & 0x80) != 0;
        f_.parity_overflow = !__builtin_parity(value);
        f_.half_carry = false;
        f_.add_subtract = false;
        return true;
    }
    case 0x41: case 0x49: case 0x51: case 0x59:
    case 0x61: case 0x69: case 0x71: case 0x79:
    {
        uint8_t y = (layout.opcode >> 3) & 0x07;
        byte value = (y == 6) ? 0 : *resolve_r(layout, pCmd, y);
        port_out(bc_.bc, value);
        return true;
    }

    // ---- SBC HL,rr / ADC HL,rr (opcode&7==2) ------------------------------
    case 0x42: case 0x52: case 0x62: case 0x72:
    case 0x4A: case 0x5A: case 0x6A: case 0x7A:
    {
        uint8_t pairSel = (layout.opcode >> 4) & 0x03;
        bool isAdc = (layout.opcode & 0x08) != 0;
        word *rr = (pairSel == 0) ? &bc_.bc : (pairSel == 1) ? &de_.de : (pairSel == 2) ? &hl_.hl : &sp_;
        execute_adc_sbc_hl(*rr, isAdc);
        return true;
    }

    // ---- LD (nn),rr / LD rr,(nn) (opcode&7==3) ----------------------------
    case 0x43: case 0x53: case 0x63: case 0x73:
    case 0x4B: case 0x5B: case 0x6B: case 0x7B:
    {
        uint8_t pairSel = (layout.opcode >> 4) & 0x03;
        bool isLoad = (layout.opcode & 0x08) != 0;
        word *rr = (pairSel == 0) ? &bc_.bc : (pairSel == 1) ? &de_.de : (pairSel == 2) ? &hl_.hl : &sp_;
        word addr = from_reg(immediate(pCmd, layout));
        if (isLoad)
        {
            byte lo = pMemory_->data()[addr];
            byte hi = pMemory_->data()[addr + 1];
            *rr = from_reg(static_cast<word>(lo | (hi << 8)));
        }
        else
        {
            word value = from_reg(*rr);
            pMemory_->write(addr, static_cast<byte>(value & 0xFF));
            pMemory_->write(static_cast<word>(addr + 1), static_cast<byte>((value >> 8) & 0xFF));
        }
        return true;
    }

    // ---- NEG (opcode&7==4, every y is a documented/undocumented duplicate) --
    case 0x44: case 0x4C: case 0x54: case 0x5C:
    case 0x64: case 0x6C: case 0x74: case 0x7C:
    {
        byte operand = a_;
        a_ = 0;
        execute_alu(AluOp::SUB, operand);
        return true;
    }

    // ---- RETN / RETI (opcode&7==5) -----------------------------------------
    case 0x4D: // reti (does not touch iff1/iff2, unlike retn)
        pc_ = pop_word();
        return false;
    case 0x45: case 0x55: case 0x5D:
    case 0x65: case 0x6D: case 0x75: case 0x7D: // retn
        iff1 = iff2;
        pc_ = pop_word();
        return false;

    // ---- IM 0/1/2 (opcode&7==6) ----------------------------------------
    case 0x46: case 0x4E: case 0x66: case 0x6E: im = 0; return true;
    case 0x56: case 0x76: im = 1; return true;
    case 0x5E: case 0x7E: im = 2; return true;

    // ---- Misc single-purpose ops (opcode&7==7) ---------------------------
    case 0x47: // ld i,a
        i_ = a_;
        return true;
    case 0x4F: // ld r,a
        r_ = a_;
        return true;
    case 0x57: // ld a,i
        a_ = i_;
        f_.zero = (a_ == 0);
        f_.minus = (a_ & 0x80) != 0;
        f_.parity_overflow = iff2;
        f_.half_carry = false;
        f_.add_subtract = false;
        return true;
    case 0x5F: // ld a,r
        a_ = r_;
        f_.zero = (a_ == 0);
        f_.minus = (a_ & 0x80) != 0;
        f_.parity_overflow = iff2;
        f_.half_carry = false;
        f_.add_subtract = false;
        return true;
    case 0x67: // rrd
    {
        word offset = from_reg(hl_.hl);
        byte mem = pMemory_->data()[offset];
        byte newMem = static_cast<byte>((a_ << 4) | (mem >> 4));
        byte newA = static_cast<byte>((a_ & 0xF0) | (mem & 0x0F));
        pMemory_->write(offset, newMem);
        a_ = newA;
        f_.zero = (a_ == 0);
        f_.minus = (a_ & 0x80) != 0;
        f_.parity_overflow = !__builtin_parity(a_);
        f_.half_carry = false;
        f_.add_subtract = false;
        return true;
    }
    case 0x6F: // rld
    {
        word offset = from_reg(hl_.hl);
        byte mem = pMemory_->data()[offset];
        byte newMem = static_cast<byte>((mem << 4) | (a_ & 0x0F));
        byte newA = static_cast<byte>((a_ & 0xF0) | (mem >> 4));
        pMemory_->write(offset, newMem);
        a_ = newA;
        f_.zero = (a_ == 0);
        f_.minus = (a_ & 0x80) != 0;
        f_.parity_overflow = !__builtin_parity(a_);
        f_.half_carry = false;
        f_.add_subtract = false;
        return true;
    }
    case 0x77: // undocumented: no operation
    case 0x7F: // undocumented: no operation
        return true;

    // ---- Block instructions (0xA0-0xA3/0xA8-0xAB/0xB0-0xB3/0xB8-0xBB) -----
    case 0xA0: execute_ldi_ldd(false); return true; // ldi
    case 0xA8: execute_ldi_ldd(true);  return true; // ldd
    case 0xB0: // ldir
        execute_ldi_ldd(false);
        return bc_.bc == 0; // repeat (pc_ unadvanced) while BC != 0
    case 0xB8: // lddr
        execute_ldi_ldd(true);
        return bc_.bc == 0;

    case 0xA1: execute_cpi_cpd(false); return true; // cpi
    case 0xA9: execute_cpi_cpd(true);  return true; // cpd
    case 0xB1: return !execute_cpi_cpd(false); // cpir
    case 0xB9: return !execute_cpi_cpd(true);  // cpdr

    case 0xA2: execute_ini_ind(false); return true; // ini
    case 0xAA: execute_ini_ind(true);  return true; // ind
    case 0xB2: // inir
        execute_ini_ind(false);
        return bc_.b == 0;
    case 0xBA: // indr
        execute_ini_ind(true);
        return bc_.b == 0;

    case 0xA3: execute_outi_outd(false); return true; // outi
    case 0xAB: execute_outi_outd(true);  return true; // outd
    case 0xB3: // otir
        execute_outi_outd(false);
        return bc_.b == 0;
    case 0xBB: // otdr
        execute_outi_outd(true);
        return bc_.b == 0;

    default:
        // Every genuinely-defined ED opcode has its own case above (the
        // documented 0x40-0x7F block plus the 16 block-transfer/search/IO
        // instructions), so anything reaching here is undefined on real
        // hardware and behaves as an inert NOP.
        return true;
    }
}

bool Cpu::executeIndexed(Layout &layout, byte *pCmd)
{
    bool useIY = (layout.index == PFX_FD);
    word &ixy = useIY ? iy_.iy : ix_.ix;
    byte &ixyh = useIY ? iy_.iyh : ix_.ixh;
    byte &ixyl = useIY ? iy_.iyl : ix_.ixl;

    switch (layout.opcode)
    {
    case 0x09: // add ix/iy,bc
        execute_add_r2_to_r1(ixy, bc_.bc);
        return true;
    case 0x19: // add ix/iy,de
        execute_add_r2_to_r1(ixy, de_.de);
        return true;
    case 0x21: // ld ix/iy,nn
        ixy = immediate(pCmd, layout);
        return true;
    case 0x22: // ld (nn),ix/iy
    {
        word addr = from_reg(immediate(pCmd, layout));
        pMemory_->write(addr, static_cast<byte>(ixy & 0xFF));
        pMemory_->write(static_cast<word>(addr + 1), static_cast<byte>((ixy >> 8) & 0xFF));
        return true;
    }
    case 0x23: // inc ix/iy
        execute_inc(ixy);
        return true;
    case 0x24: // inc ixh/iyh (undocumented)
        execute_inc(ixyh);
        return true;
    case 0x25: // dec ixh/iyh (undocumented)
        execute_dec(ixyh);
        return true;
    case 0x26: // ld ixh/iyh,n (undocumented)
        ixyh = static_cast<byte>(immediate(pCmd, layout));
        return true;
    case 0x29: // add ix/iy,ix/iy
        execute_add_r2_to_r1(ixy, ixy);
        return true;
    case 0x2A: // ld ix/iy,(nn)
    {
        word addr = from_reg(immediate(pCmd, layout));
        byte lo = pMemory_->data()[addr];
        byte hi = pMemory_->data()[addr + 1];
        ixy = static_cast<word>(lo | (hi << 8));
        return true;
    }
    case 0x2B: // dec ix/iy
        execute_dec(ixy);
        return true;
    case 0x2C: // inc ixl/iyl (undocumented)
        execute_inc(ixyl);
        return true;
    case 0x2D: // dec ixl/iyl (undocumented)
        execute_dec(ixyl);
        return true;
    case 0x2E: // ld ixl/iyl,n (undocumented)
        ixyl = static_cast<byte>(immediate(pCmd, layout));
        return true;
    case 0x34: // inc (ix/iy+d)
        execute_inc(*resolve_r(layout, pCmd, 6));
        return true;
    case 0x35: // dec (ix/iy+d)
        execute_dec(*resolve_r(layout, pCmd, 6));
        return true;
    case 0x36: // ld (ix/iy+d),n
        *resolve_r(layout, pCmd, 6) = static_cast<byte>(immediate(pCmd, layout));
        return true;
    case 0x39: // add ix/iy,sp
        execute_add_r2_to_r1(ixy, sp_);
        return true;
    case 0xE1: // pop ix/iy
        ixy = pop_word();
        return true;
    case 0xE3: // ex (sp),ix/iy
    {
        word temp = static_cast<word>(pMemory_->data()[sp_] | (pMemory_->data()[sp_ + 1] << 8));
        pMemory_->write(sp_, static_cast<byte>(ixy & 0xFF));
        pMemory_->write(static_cast<word>(sp_ + 1), static_cast<byte>((ixy >> 8) & 0xFF));
        ixy = temp;
        return true;
    }
    case 0xE5: // push ix/iy
        push_word(ixy);
        return true;
    case 0xE9: // jp (ix/iy)
        pc_ = ixy;
        return false;
    case 0xF9: // ld sp,ix/iy
        sp_ = ixy;
        return true;

    default:
        // Everything else either doesn't touch HL/(HL) at all (identical to
        // the un-prefixed Base opcode, just costing 4 extra T-states on real
        // hardware) or is part of the LD r,r'/ALU block (0x40-0xBF), whose
        // resolve_r()-based operand resolution is already page-aware and
        // redirects H/L/(HL) codes to IXH/IXL/(IX+d) automatically.
        return executeBase(layout, pCmd);
    }
}

bool Cpu::executeIndexedCB(Layout &layout, byte *pCmd)
{
    word addr = resolve_indexed_addr(layout, pCmd);
    byte value = pMemory_->data()[addr];
    uint8_t group = (layout.opcode >> 6) & 0x03;      // 0=rotate/shift, 1=BIT, 2=RES, 3=SET
    uint8_t bitOrOp = (layout.opcode >> 3) & 0x07;     // bit index, or rotate/shift family
    uint8_t regCode = layout.opcode & 0x07;            // undocumented "also store here" target

    if (group == 1) // bit b,(ix/iy+d) -- never stores anywhere
    {
        execute_bit(value, bitOrOp);
        return true;
    }

    byte result = value;
    if (group == 0)
    {
        switch (bitOrOp)
        {
        case 0: execute_rlc(result); break;
        case 1: execute_rrc(result); break;
        case 2: execute_rl(result);  break;
        case 3: execute_rr(result);  break;
        case 4: execute_sla(result); break;
        case 5: execute_sra(result); break;
        case 6: execute_sll(result); break; // undocumented
        case 7: execute_srl(result); break;
        }
    }
    else if (group == 2)
    {
        execute_res(result, bitOrOp);
    }
    else // group == 3
    {
        execute_set(result, bitOrOp);
    }

    pMemory_->write(addr, result);

    if (regCode != 6) // undocumented: also copy the result into a plain register
    {
        // Always the *plain* B,C,D,E,H,L,A -- this quirk never targets
        // IXH/IXL/IYH/IYL, so resolve_r()'s indexed redirect must be
        // bypassed here.
        switch (regCode)
        {
        case 0: bc_.b = result; break;
        case 1: bc_.c = result; break;
        case 2: de_.d = result; break;
        case 3: de_.e = result; break;
        case 4: hl_.h = result; break;
        case 5: hl_.l = result; break;
        case 7: a_ = result; break;
        }
    }

    return true;
}

byte* Cpu::resolve_r(const Layout &L, const byte *p, uint8_t code)
{
    bool indexed = (L.page == Page::Indexed || L.page == Page::IndexedCB);

    // H and L only become IXH/IXL (IYH/IYL) when the instruction does *not*
    // use (IX+d)/(IY+d) addressing. Once there's a displacement byte, the
    // (HL) slot is the indexed operand and the other operand keeps its plain
    // meaning -- e.g. FD 6E d is LD L,(IY+d), which loads plain L and must
    // not write into IYL.
    bool index_halves = indexed && L.dispOff < 0;

    switch (code & 0x07)
    {
    case 0: return &bc_.b;
    case 1: return &bc_.c;
    case 2: return &de_.d;
    case 3: return &de_.e;
    case 4:
        if (index_halves) return (L.index == PFX_FD) ? &iy_.iyh : &ix_.ixh;
        return &hl_.h;
    case 5:
        if (index_halves) return (L.index == PFX_FD) ? &iy_.iyl : &ix_.ixl;
        return &hl_.l;
    case 6:
    {
        word addr = indexed ? resolve_indexed_addr(L, p) : hl_.hl;
        return pMemory_->writable(addr);
    }
    case 7: return &a_;
    }
    return nullptr; // unreachable
}

word Cpu::resolve_indexed_addr(const Layout &L, const byte *p)
{
    int8_t d = displacement(p, L);
    word base = (L.index == PFX_FD) ? iy_.iy : ix_.ix;
    return static_cast<word>(base + d);
}

bool Cpu::execute_alu(AluOp op, byte operand)
{
    int a = a_;
    int b = operand;
    int carryIn = f_.carry ? 1 : 0;
    int result = 0;
    bool bCarry = false, bHalfCarry = false, bOverflow = false, bSubtract = false;

    switch (op)
    {
    case AluOp::ADD:
        result = a + b;
        bCarry = result > 0xFF;
        bHalfCarry = ((a & 0x0F) + (b & 0x0F)) > 0x0F;
        bOverflow = (~(a ^ b) & (a ^ result) & 0x80) != 0;
        bSubtract = false;
        break;
    case AluOp::ADC:
        result = a + b + carryIn;
        bCarry = result > 0xFF;
        bHalfCarry = ((a & 0x0F) + (b & 0x0F) + carryIn) > 0x0F;
        bOverflow = (~(a ^ b) & (a ^ result) & 0x80) != 0;
        bSubtract = false;
        break;
    case AluOp::SUB:
        result = a - b;
        bCarry = result < 0;
        bHalfCarry = ((a & 0x0F) - (b & 0x0F)) < 0;
        bOverflow = ((a ^ b) & (a ^ result) & 0x80) != 0;
        bSubtract = true;
        break;
    case AluOp::SBC:
        result = a - b - carryIn;
        bCarry = result < 0;
        bHalfCarry = ((a & 0x0F) - (b & 0x0F) - carryIn) < 0;
        bOverflow = ((a ^ b) & (a ^ result) & 0x80) != 0;
        bSubtract = true;
        break;
    case AluOp::AND:
        result = a & b;
        bHalfCarry = true;
        bOverflow = !__builtin_parity(static_cast<byte>(result));
        break;
    case AluOp::XOR:
        result = a ^ b;
        bOverflow = !__builtin_parity(static_cast<byte>(result));
        break;
    case AluOp::OR:
        result = a | b;
        bOverflow = !__builtin_parity(static_cast<byte>(result));
        break;
    case AluOp::CP:
        result = a - b;
        bCarry = result < 0;
        bHalfCarry = ((a & 0x0F) - (b & 0x0F)) < 0;
        bOverflow = ((a ^ b) & (a ^ result) & 0x80) != 0;
        bSubtract = true;
        break;
    }

    byte byteResult = static_cast<byte>(result & 0xFF);
    bool bZero = byteResult == 0;
    bool bMinus = (byteResult & 0x80) != 0;

    if (op != AluOp::CP)
    {
        a_ = byteResult;
    }

    set_flags(bCarry, bSubtract, bMinus, bHalfCarry, bZero, bOverflow);
    return true;
}

bool Cpu::execute_bit(byte value, uint8_t bit)
{
    bool isSet = (value & (1 << bit)) != 0;
    f_.zero = !isSet;
    f_.parity_overflow = !isSet; // undocumented: P/V mirrors Z for BIT
    f_.minus = (bit == 7) && isSet; // undocumented: S reflects bit 7 when tested
    f_.half_carry = true;
    f_.add_subtract = false;
    // Carry is not affected by BIT.
    return isSet;
}

void Cpu::execute_res(byte &reg, uint8_t bit)
{
    reg &= static_cast<byte>(~(1 << bit));
}

void Cpu::execute_set(byte &reg, uint8_t bit)
{
    reg |= static_cast<byte>(1 << bit);
}

bool Cpu::execute_rl(byte &reg)
{
    bool bCarry = (reg & 0x80) != 0;
    reg = (reg << 1) | (f_.carry ? 0x01 : 0x00);
    bool bZero = (reg == 0);
    bool bMinus = (reg & 0x80) != 0;
    bool bParity = !__builtin_parity(reg);
    set_flags(bCarry, false, bMinus, false, bZero, bParity);
    return true;
}

bool Cpu::execute_rr(byte &reg)
{
    bool bCarry = (reg & 0x01) != 0;
    reg = (reg >> 1) | (f_.carry ? 0x80 : 0x00);
    bool bZero = (reg == 0);
    bool bMinus = (reg & 0x80) != 0;
    bool bParity = !__builtin_parity(reg);
    set_flags(bCarry, false, bMinus, false, bZero, bParity);
    return true;
}

bool Cpu::execute_rlc(byte &reg)
{
    bool carry = (reg & 0x80) != 0;

    reg = (reg << 1) | (carry ? 0x01: 0x00);

    bool minus = (reg & 0x80) != 0;
    bool zero = (reg == 0);

    set_flags(carry,0,minus,0, zero, !__builtin_parity(reg));

    return true;
}

bool Cpu::execute_rrc(byte &reg)
{
    bool carry = (reg & 0x01) != 0;

    reg = (reg >> 1) | (carry ? 0x80: 0x00);

    bool minus = (reg & 0x80) != 0;
    bool zero = (reg == 0);

    set_flags(carry,0,minus,0, zero, !__builtin_parity(reg));
    return true;
}

bool Cpu::execute_sla(byte &reg)
{
    f_.carry = ((reg & 0x80) != 0);
    reg = reg << 1;
    f_.zero = (reg == 0);
    f_.minus = ((reg & 0x80) != 0);
    f_.parity_overflow = !__builtin_parity(reg);
    f_.half_carry = false;
    f_.add_subtract = false;
    return true;
}

bool Cpu::execute_sra(byte &reg)
{
    f_.carry = ((reg & 0x01) != 0);
    reg = ((reg & 0x80) | (reg >> 1));
    f_.zero = (reg == 0);
    f_.minus = ((reg & 0x80) != 0);
    f_.parity_overflow = !__builtin_parity(reg);
    f_.half_carry = false;
    f_.add_subtract = false;
    return true;
}

bool Cpu::execute_sll(byte &reg)
{
    f_.carry = ((reg & 0x80) != 0);
    reg = (reg << 1) | 0x01; // undocumented: bit 0 forced to 1
    f_.zero = (reg == 0);
    f_.minus = ((reg & 0x80) != 0);
    f_.parity_overflow = !__builtin_parity(reg);
    f_.half_carry = false;
    f_.add_subtract = false;
    return true;
}

bool Cpu::execute_srl(byte &reg)
{
    f_.carry = ((reg & 0x01) != 0);
    reg = reg >> 1; // logical shift: bit 7 always becomes 0
    f_.zero = (reg == 0);
    f_.minus = ((reg & 0x80) != 0);
    f_.parity_overflow = !__builtin_parity(reg);
    f_.half_carry = false;
    f_.add_subtract = false;
    return true;
}

bool Cpu::execute_inc(byte &reg)
{
    bool bZero = false;
    bool bHalf_carry = (reg & 0x0F) == 0x0F;
    bool bMinus = false;
    bool bOverflow = reg == 0x7F;
    if (reg == 0xFF)
    {
        reg = 0;
        bZero = true;
    }
    else
    {
        reg++;
    }
    if ((reg & 0x80) != 0)
    {
        bMinus = true;
    }
    set_flags(f_.carry == 1 ? true : false, false, bMinus, bHalf_carry, bZero, bOverflow);
    return true;
}

bool Cpu::execute_dec(byte &reg)
{
    bool bZero = false;
    bool bHalf_carry = (reg & 0x0F) == 0x00;
    bool bMinus = false;
    bool bOverflow = reg == 0x80;
    if (reg == 0x01)
    {
        reg = 0;
        bZero = true;
    }
    else
    {
        reg--;
    }
    if ((reg & 0x80) != 0)
    {
        bMinus = true;
    }
    set_flags(f_.carry == 1 ? true : false, true, bMinus, bHalf_carry, bZero, bOverflow);
    return true;
}

bool Cpu::execute_inc(word &reg)
{
    auto value = from_reg(reg);
    if (value == 0xFFFF)
    {
        value = 0;
    }
    else
    {
        value++;
    }
    reg = from_reg(value);
    return true;
}

bool Cpu::execute_dec(word &reg)
{
    auto value = from_reg(reg);
    if (value == 0)
    {
        value = 0xFFFF;
    }
    else
    {
        value--;
    }
    reg = from_reg(value);
    return true;
}

bool Cpu::execute_add_r2_to_r1(word &r1, word &r2)
{
    int r1val = from_reg(r1);
    int r2val = from_reg(r2);
    bool bCarry = r1val + r2val > 0xFFFF;

    uint16_t value = 0;
    if (bCarry)
    {
        value = (uint16_t) ((r1val + r2val) - 0x10000);
    }
    else
    {
        value = (uint16_t)r1val + (uint16_t)r2val;
    }

    bool bHalfCarry = ((r1val & 0x0FFF) + (r2val & 0x0FFF)) > 0x0FFF;
    r1 = from_reg(value);
    set_flags(bCarry, false, f_.minus, bHalfCarry, f_.zero, f_.parity_overflow);

    return true;
}

bool Cpu::execute_adc_sbc_hl(word &r2, bool isAdc)
{
    int hlval = from_reg(hl_.hl);
    int r2val = from_reg(r2);
    int carryIn = f_.carry ? 1 : 0;
    int result;
    bool bCarry, bHalfCarry, bOverflow, bSubtract;

    if (isAdc)
    {
        result = hlval + r2val + carryIn;
        bCarry = result > 0xFFFF;
        bHalfCarry = ((hlval & 0x0FFF) + (r2val & 0x0FFF) + carryIn) > 0x0FFF;
        bOverflow = (~(hlval ^ r2val) & (hlval ^ result) & 0x8000) != 0;
        bSubtract = false;
    }
    else
    {
        result = hlval - r2val - carryIn;
        bCarry = result < 0;
        bHalfCarry = ((hlval & 0x0FFF) - (r2val & 0x0FFF) - carryIn) < 0;
        bOverflow = ((hlval ^ r2val) & (hlval ^ result) & 0x8000) != 0;
        bSubtract = true;
    }

    word wordResult = static_cast<word>(result & 0xFFFF);
    bool bZero = wordResult == 0;
    bool bMinus = (wordResult & 0x8000) != 0;

    hl_.hl = from_reg(wordResult);
    set_flags(bCarry, bSubtract, bMinus, bHalfCarry, bZero, bOverflow);
    return true;
}

bool Cpu::execute_daa()
{
    byte a = a_;
    bool carry = f_.carry;
    byte correction = 0;

    if (f_.half_carry || (!f_.add_subtract && (a & 0x0F) > 9))
    {
        correction |= 0x06;
    }
    if (f_.carry || (!f_.add_subtract && a > 0x99))
    {
        correction |= 0x60;
        carry = true;
    }

    byte result;
    bool halfCarryOut;
    if (f_.add_subtract)
    {
        halfCarryOut = f_.half_carry && ((a & 0x0F) < 6);
        result = static_cast<byte>(a - correction);
    }
    else
    {
        halfCarryOut = (a & 0x0F) > 9;
        result = static_cast<byte>(a + correction);
    }

    a_ = result;
    set_flags(carry, f_.add_subtract, (result & 0x80) != 0, halfCarryOut, result == 0, !__builtin_parity(result));
    return true;
}

void Cpu::execute_ldi_ldd(bool decrement)
{
    word hlAddr = from_reg(hl_.hl);
    word deAddr = from_reg(de_.de);
    byte value = pMemory_->data()[hlAddr];
    pMemory_->write(deAddr, value);

    if (decrement)
    {
        execute_dec(hl_.hl);
        execute_dec(de_.de);
    }
    else
    {
        execute_inc(hl_.hl);
        execute_inc(de_.de);
    }
    execute_dec(bc_.bc); // counter only; 16-bit dec never touches flags

    f_.half_carry = false;
    f_.add_subtract = false;
    f_.parity_overflow = (bc_.bc != 0);
    // S, Z, C unaffected.
}

bool Cpu::execute_cpi_cpd(bool decrement)
{
    word hlAddr = from_reg(hl_.hl);
    byte value = pMemory_->data()[hlAddr];
    int result = static_cast<int>(a_) - static_cast<int>(value);
    bool halfCarry = ((a_ & 0x0F) - (value & 0x0F)) < 0;

    if (decrement)
        execute_dec(hl_.hl);
    else
        execute_inc(hl_.hl);
    execute_dec(bc_.bc);

    byte byteResult = static_cast<byte>(result & 0xFF);
    f_.zero = (byteResult == 0);
    f_.minus = (byteResult & 0x80) != 0;
    f_.half_carry = halfCarry;
    f_.add_subtract = true;
    f_.parity_overflow = (bc_.bc != 0);
    // Carry unaffected.

    return bc_.bc != 0 && byteResult != 0; // whether xxIR/xxDR should repeat
}

void Cpu::execute_ini_ind(bool decrement)
{
    byte value = port_in(bc_.bc);
    word hlAddr = from_reg(hl_.hl);
    pMemory_->write(hlAddr, value);

    bc_.b--; // raw decrement: this isn't a full 8-bit DEC, only Z/N are modeled below
    if (decrement)
        execute_dec(hl_.hl);
    else
        execute_inc(hl_.hl);

    f_.zero = (bc_.b == 0);
    f_.add_subtract = true;
}

void Cpu::execute_outi_outd(bool decrement)
{
    word hlAddr = from_reg(hl_.hl);
    byte value = pMemory_->data()[hlAddr];
    port_out(bc_.bc, value);

    if (decrement)
        execute_dec(hl_.hl);
    else
        execute_inc(hl_.hl);
    bc_.b--; // raw decrement: this isn't a full 8-bit DEC, only Z/N are modeled below

    f_.zero = (bc_.b == 0);
    f_.add_subtract = true;
}

void Cpu::push_word(word value)
{
    sp_ -= 2;
    pMemory_->write(sp_, static_cast<byte>(value & 0xFF));
    pMemory_->write(static_cast<word>(sp_ + 1), static_cast<byte>((value >> 8) & 0xFF));
}

word Cpu::pop_word()
{
    word value = static_cast<word>(pMemory_->data()[sp_] | (pMemory_->data()[sp_ + 1] << 8));
    sp_ += 2;
    return value;
}

bool Cpu::check_condition(uint8_t cc)
{
    switch (cc & 0x07)
    {
    case 0: return !f_.zero;            // NZ
    case 1: return f_.zero;             // Z
    case 2: return !f_.carry;           // NC
    case 3: return f_.carry;            // C
    case 4: return !f_.parity_overflow; // PO
    case 5: return f_.parity_overflow;  // PE
    case 6: return !f_.minus;           // P
    case 7: return f_.minus;            // M
    }
    return false; // unreachable
}

void Cpu::set_flags(bool carry, bool add_subtract, bool minus, bool half_carry, bool zero, bool parity_overflow)
{
    f_.carry = carry ? 1 : 0;
    f_.add_subtract = add_subtract ? 1 : 0;
    f_.minus = minus ? 1 : 0;
    f_.half_carry = half_carry ? 1 : 0;
    f_.zero = zero ? 1 : 0;
    f_.parity_overflow = parity_overflow ? 1 : 0;
}


} // namespace z80
