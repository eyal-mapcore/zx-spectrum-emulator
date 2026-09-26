#pragma once

#include "globals.h"
#include <bitset>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <z80/z80_mem.h>
#include <chrono>
#include <functional>

#define FRMAES_PER_SECOND 50 // 50Hz frame rate for ZX Spectrum 
#define CYCLES_PER_FRAME 69888 // Number of T-states per frame for ZX Spectrum

namespace z80 {

// Forward declaration of the Memory class
enum class Page : byte { Base, CB, ED, Indexed, IndexedCB, IgnoredPrefix };

/// @brief ZX Instruction layout
typedef struct _Layout {
    uint8_t length    = 1;   // total bytes, 1..4
    uint8_t prefixLen = 0;   // 0, 1 (CB/ED/DD/FD) or 2 (DD CB)
    int8_t  opcodeOff = 0;   // offset of the opcode byte
    int8_t  dispOff   = -1;  // offset of the signed displacement d
    int8_t  immOff    = -1;  // offset of the immediate
    uint8_t immLen    = 0;   // 0, 1 or 2
    uint8_t opcode    = 0;   // the opcode byte itself
    uint8_t index     = 0;   // 0xDD, 0xFD, or 0 when not index-prefixed
    Page    page      = Page::Base;
} Layout;

/// @brief Flat snapshot of the CPU's registers, for testing/inspection.
struct RegisterState {
    byte a = 0;
    byte f = 0;   // raw flags byte: bit0=C,1=N,2=P/V,4=H,6=Z,7=S
    word bc = 0;
    word de = 0;
    word hl = 0;
    word ix = 0;
    word iy = 0;
    word pc = 0;
    word sp = 0;
    byte i = 0;
    byte r = 0;
    byte im = 0;
    bool iff1 = false;
    bool iff2 = false;
    bool halted = false;

    // Shadow ("alternate") register bank.
    byte a_alt = 0;
    byte f_alt = 0;
    word bc_alt = 0;
    word de_alt = 0;
    word hl_alt = 0;
};

class Cpu {
public:
    Cpu();

    void reset();
    int step();
    void boot();
    Memory* get_memory() { return pMemory_; }

    // Snapshot/restore the registers, for tests and debugging.
    RegisterState get_registers() const;
    void set_registers(const RegisterState& regs);

    // Command layout information for the current instruction
    Layout decode(const byte* p);

    // Read the immediate as an unsigned value (little-endian) if present.
    uint16_t immediate(const byte* p, const Layout& L);

    int8_t displacement(const byte* p, const Layout& L);

    uint16_t from_reg(word value);
    bool try_decode(const byte *p, Layout &L);

    // Scans backward from pCurrent for an instruction whose decoded length
    // lands exactly on pCurrent. On success, fills L with that instruction's
    // Layout and returns its start address; returns nullptr if none is found
    // within kMaxInstructionLength bytes.
    byte* find_prev_instruction(Layout &L, byte *pCurrent);

    bool executeCommand(Layout & layout, byte *pCmd);

    // Called once per frame (~50Hz), right when step() detects that a
    // frame's worth of T-states has been simulated -- the hook a caller
    // (e.g. the renderer, or eventually the interrupt controller) uses to
    // react to frame boundaries without subclassing Cpu. Receives the Cpu
    // that raised it, so one callback can serve several instances.
    using FrameCallback = std::function<void(Cpu*)>;
    void set_frame_callback(FrameCallback cb) { frame_callback_ = std::move(cb); }

    // Raises the maskable interrupt (the ULA pulses /INT once per frame).
    // The request is ignored while interrupts are disabled (IFF1 clear),
    // which is how the ROM protects its critical sections. On acceptance the
    // CPU leaves HALT, disables further interrupts, pushes the return address
    // and vectors according to the current interrupt mode. Returns the number
    // of T-states the acknowledge cycle consumed, or 0 if it was ignored, so
    // the caller can keep its cycle budget honest.
    int interrupt();

    // Minimal I/O seam: no ULA/keyboard device is wired in yet, so IN/OUT
    // opcodes read/write through these. Two ways to attach a device: set a
    // callback (works on a plain Cpu, no subclassing needed) or override the
    // virtual method in a subclass; the callback takes precedence if set.
    using PortInCallback = std::function<byte(word)>;
    using PortOutCallback = std::function<void(word, byte)>;
    void set_port_in_callback(PortInCallback cb) { port_in_callback_ = std::move(cb); }
    void set_port_out_callback(PortOutCallback cb) { port_out_callback_ = std::move(cb); }

    virtual byte port_in(word port) {
        return port_in_callback_ ? port_in_callback_(port) : 0xFF;
    }
    virtual void port_out(word port, byte value) {
        if (port_out_callback_) port_out_callback_(port, value);
    }

    // ---- PC traps ---------------------------------------------------------
    // A trap replaces execution of the instruction at one address. The
    // callback runs with pc_ still pointing at that address and must leave the
    // registers as the code it stands in for would have -- for a routine that
    // was CALLed, that means ending with `cpu->set_pc(cpu->pop_word())` so the
    // trap behaves as an immediate RET.
    //
    // Returning false *declines* this occurrence and lets the real instruction
    // execute, so a hook can fall back to the genuine ROM code (no tape
    // inserted, BREAK pressed, and so on).
    //
    // This is how ROM entry points are intercepted -- the tape routines
    // LD-BYTES (0x0556) and SA-BYTES (0x04C2) -- without patching the ROM.
    using TrapCallback = std::function<bool(Cpu*)>;

    // T-states charged for a trap that does not specify its own cost. A trap
    // returns instantly where the real routine took millions of T-states, so
    // the ROM's FRAMES counter runs slow across an intercepted operation;
    // charging a nominal cost at least keeps time moving forward.
    static const int kDefaultTrapStates = 100;

    void set_trap(word address, TrapCallback cb, int states = kDefaultTrapStates);
    void clear_trap(word address);
    void clear_all_traps();
    bool has_trap(word address) const { return trap_bitmap_[address]; }

    // Stack primitives and direct PC access, public so that a trap callback
    // can stand in for a routine that was CALLed.
    void push_word(word value);
    word pop_word();
    word pc() const { return pc_; }
    void set_pc(word address) { pc_ = address; }

    /// Real-time pacing, on by default: step() sleeps at each frame boundary
    /// so the machine runs at a Spectrum's speed. Turn it off to run flat out.
    /// Tests that drive the ROM care about emulated time, not wall-clock time,
    /// and pacing makes them hundreds of times slower for no benefit.
    void set_realtime(bool on) { realtime_ = on; }
    bool realtime() const { return realtime_; }

    /// T-states executed since this Cpu was constructed.
    ///
    /// Free-running, and deliberately *not* cleared by reset(): its whole
    /// purpose is to be a monotonic timestamp for devices that care when
    /// something happened -- the beeper, and eventually a signal-level tape.
    /// A consumer holding a last-seen value must never see it go backwards.
    /// (cycle_count_ is the separate, resettable frame budget.)
    ///
    /// Resolution is one instruction: a device notified from inside an
    /// instruction -- a port write, say -- sees the count as it stood at that
    /// instruction's *start*, because the cost is added once the instruction
    /// completes. At 3.5MHz that is at most ~23 T-states of skew, comfortably
    /// inside one 44.1kHz sample (~79 T-states).
    uint64_t total_cycles() const { return total_cycles_; }

protected:
    // Per-page dispatch, called from executeCommand based on layout.page.
    // Base and CB stop sharing anything meaningful past opcode ~0x3F (e.g.
    // Base 0x40-0x7F is LD r,r' while CB 0x40-0x7F is BIT b,r), so each page
    // gets its own switch rather than one switch keyed on the opcode byte.
    bool executeBase(Layout &layout, byte *pCmd);
    bool executeCB(Layout &layout, byte *pCmd);
    bool executeED(Layout &layout, byte *pCmd);
    bool executeIndexed(Layout &layout, byte *pCmd);
    bool executeIndexedCB(Layout &layout, byte *pCmd);

    // Resolves "the r operand" for opcode bit-patterns where the low 3 bits
    // select {B,C,D,E,H,L,(HL),A} (or, for Page::Indexed/IndexedCB layouts,
    // {B,C,D,E,IXH/IYH,IXL/IYL,(IX+d)/(IY+d),A}). Returns a pointer valid for
    // the duration of the current instruction.
    byte* resolve_r(const Layout &L, const byte *p, uint8_t code);

    // Effective address for (IX+d)/(IY+d) given an Indexed/IndexedCB layout.
    word resolve_indexed_addr(const Layout &L, const byte *p);

    enum class AluOp { ADD, ADC, SUB, SBC, AND, XOR, OR, CP };
    bool execute_alu(AluOp op, byte operand);

    bool execute_bit(byte value, uint8_t bit);
    void execute_res(byte &reg, uint8_t bit);
    void execute_set(byte &reg, uint8_t bit);

    bool execute_rl(byte &reg);
    bool execute_rr(byte &reg);

    bool execute_rlc(byte &reg);
    bool execute_rrc(byte &reg);

    bool execute_sla(byte &reg);
    bool execute_sra(byte &reg);
    bool execute_sll(byte &reg); // undocumented: shift left, bit 0 forced to 1
    bool execute_srl(byte &reg);

    bool execute_inc(byte &reg);
    bool execute_dec(byte &reg);

    bool execute_inc(word &reg);
    bool execute_dec(word &reg);

    bool execute_add_r2_to_r1(word& r1, word& r2);
    bool execute_adc_sbc_hl(word& r2, bool isAdc);
    bool execute_daa();

    // Frame-boundary housekeeping: real-time pacing, frame callback, /INT.
    void end_of_frame();

    bool check_condition(uint8_t cc); // NZ,Z,NC,C,PO,PE,P,M (0..7)

    // Shared logic for the ED-page block instruction families. Each performs
    // one step (transfer/compare/in/out plus the HL/DE/BC pointer update);
    // the xxIR/xxDR "repeat" forms just call the same step and, if the
    // repeat condition holds, leave pc_ unadvanced so step() re-executes it.
    void execute_ldi_ldd(bool decrement);
    bool execute_cpi_cpd(bool decrement); // returns true while a repeat is warranted (xxIR/xxDR)
    void execute_ini_ind(bool decrement);
    void execute_outi_outd(bool decrement);

    void set_flags(bool carry, bool add_subtract, bool minus, bool half_carry, bool zero, bool parity_overflow);


private:
    // Number of T-states the current instruction takes. Must be called
    // before executeCommand() mutates registers/flags: variable-timing
    // instructions (conditional JR/RET/JP/CALL, DJNZ, the ED-page xxIR/xxDR
    // block-repeat forms) are resolved by inspecting the *current* flags/
    // registers to predict whether the branch/repeat will be taken, exactly
    // as check_condition()/DJNZ's own decrement logic would.
    uint8_t states(Layout &layout);

    // Base-page timing, including the conditional forms. Shared with the
    // indexed pages, where an ignored DD/FD prefix just adds 4 T-states.
    uint8_t base_states(byte op);

    // How long to sleep so that `cycle_count` T-states' worth of real time
    // (at the clock rate implied by CYCLES_PER_FRAME/FRMAES_PER_SECOND) has
    // elapsed since `cycle_time`, given that `current_time` has already
    // elapsed. Returns zero (never negative) if that much real time has
    // already passed -- i.e. we're behind schedule, not ahead of it.
    std::chrono::duration<double, std::milli> time_to_sleep(
        int cycle_count,
        std::chrono::time_point<std::chrono::high_resolution_clock> current_time,
        std::chrono::time_point<std::chrono::high_resolution_clock> cycle_time);

    const size_t kMaxInstructionLength = 4;

    // Main registers
    byte a_ = 0;
    union {
        struct {
            byte carry:1;            // bit 0 - C
            byte add_subtract:1;     // bit 1 - N
            byte parity_overflow:1;  // bit 2 - P/V
            byte unused3:1;          // bit 3 - X (undocumented)
            byte half_carry:1;       // bit 4 - H
            byte unused5:1;          // bit 5 - Y (undocumented)
            byte zero:1;             // bit 6 - Z
            byte minus:1;            // bit 7 - S (sign)
        };
        byte f = 0;
    } f_;

    union {
        struct {
            byte c;
            byte b;
        };
        word bc = 0;
    } bc_;

    union {
        struct {
            byte e;
            byte d;
        };
        word de = 0;
    } de_;

    union {
        struct {
            byte l;
            byte h;
        };
        word hl = 0;
    } hl_;

    // index registers (byte-addressable for the undocumented IXH/IXL/IYH/IYL opcodes)
    union {
        struct {
            byte ixl;
            byte ixh;
        };
        word ix = 0;
    } ix_;

    union {
        struct {
            byte iyl;
            byte iyh;
        };
        word iy = 0;
    } iy_;

    // Interrupt and refresh registers
    byte i_ = 0;
    byte r_ = 0;

    // Program controller registers
    word pc_ = 0, sp_ = 0;

    bool halted_ = false;

    // Alternate ("shadow") registers
    byte _a_ = 0, _f_ = 0;
    union {
        struct { byte c; byte b; };
        word bc = 0;
    } _bc_;
    union {
        struct { byte e; byte d; };
        word de = 0;
    } _de_;
    union {
        struct { byte l; byte h; };
        word hl = 0;
    } _hl_;

    // Interrupt status
    union {
        struct {
            byte im: 2;   // interrupt mode
            bool iff2: 1; // interrupt flip-flop 2
            bool iff1: 1; // interrupt flip-flop 1
        };
        byte interrupt_status = 0;
    };


    Memory * pMemory_ = nullptr;
    std::chrono::time_point<std::chrono::high_resolution_clock> cycle_time_ = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> cycle_duration_ = std::chrono::milliseconds(0);
    int cycle_count_ = 0; // Total number of cycles executed since last frame

    /// Free-running T-state clock; see total_cycles().
    uint64_t total_cycles_ = 0;

    bool realtime_ = true;

    /// Charges `states` T-states to both the frame budget and the free-running
    /// clock. Every path that consumes time must go through here, or the two
    /// drift apart -- end_of_frame() subtracts a frame from cycle_count_ and
    /// must not touch total_cycles_.
    void advance_cycles(int states) {
        cycle_count_ += states;
        total_cycles_ += static_cast<uint64_t>(states);
    }

    FrameCallback frame_callback_;
    PortInCallback port_in_callback_;
    PortOutCallback port_out_callback_;

    struct Trap {
        TrapCallback callback;
        int states = kDefaultTrapStates;
    };

    // The bitmap guards the map lookup: a hash lookup for every instruction
    // would be a real cost, whereas a single bit test is free.
    std::bitset<MEMORY_SIZE> trap_bitmap_;
    std::unordered_map<word, Trap> traps_;
};

} // namespace z80
