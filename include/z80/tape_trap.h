#pragma once

#include "globals.h"
#include "tape.h"
#include "z80_cpu.h"

namespace z80 {

/// Binds a TapeDeck to a Cpu by trapping the ROM's tape entry points, so that
/// LOAD and SAVE move bytes directly instead of the ROM's edge-detection loop
/// measuring pulses that no real tape is producing.
///
/// This is the fast, high-level form of tape emulation. It works for anything
/// that loads through the ROM loader, and by construction cannot work for the
/// turbo and custom loaders that many commercial games use -- those never call
/// these routines. Supporting them needs a signal-level backend behind
/// TapeDeck's load_block()/save_block().
class TapeTrap {
public:
    /// ROM entry points, both verified against the 48K image:
    ///   0x0556  LD-BYTES  14 08 15 F3 3E 0F D3 FE
    ///   0x04C2  SA-BYTES  21 3F 05 E5
    /// Both push their own return address only *after* entry, so a trap on the
    /// first instruction still sees the caller's return address on the stack.
    static const word LD_BYTES_ENTRY = 0x0556;
    static const word SA_BYTES_ENTRY = 0x04C2;

    TapeTrap(Cpu& cpu, TapeDeck& deck) : cpu_(&cpu), deck_(&deck) {}
    ~TapeTrap() { uninstall(); }

    TapeTrap(const TapeTrap&) = delete;
    TapeTrap& operator=(const TapeTrap&) = delete;

    void install();
    void uninstall();
    bool installed() const { return installed_; }

    /// Diagnostics, and how the tests observe what the hook decided.
    /// last_save_result() is the only place a write failure surfaces: SA-BYTES
    /// cannot report one to the emulated machine, so the host must check here.
    size_t load_calls() const { return load_calls_; }
    size_t save_calls() const { return save_calls_; }
    TapeDeck::LoadResult last_load_result() const { return last_load_result_; }
    TapeDeck::SaveResult last_save_result() const { return last_save_result_; }

private:
    bool on_ld_bytes(Cpu* cpu);
    bool on_sa_bytes(Cpu* cpu);

    /// BREAK is CAPS SHIFT + SPACE. The trap bypasses LD-BYTES entirely, so
    /// the ROM's own BREAK check never runs -- without this, a LOAD that finds
    /// nothing on the tape spins with no way for the user to escape.
    static bool break_pressed(Cpu* cpu);

    Cpu* cpu_ = nullptr;
    TapeDeck* deck_ = nullptr;
    bool installed_ = false;
    size_t load_calls_ = 0;
    size_t save_calls_ = 0;
    TapeDeck::LoadResult last_load_result_ = TapeDeck::LoadResult::NoTape;
    TapeDeck::SaveResult last_save_result_ = TapeDeck::SaveResult::NoPath;
};

} // namespace z80
