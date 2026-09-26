#include "z80/tape_trap.h"

namespace z80 {

namespace {

/// Bit 0 of F. LD-BYTES takes carry as "load, don't verify" on entry and
/// returns it as "succeeded".
constexpr ::byte FLAG_CARRY = 0x01;

/// Keyboard half-rows, read active-low through any port with bit 0 clear.
constexpr word PORT_ROW_CAPS  = 0xFEFE;  // row 0: CAPS SHIFT, Z, X, C, V
constexpr word PORT_ROW_SPACE = 0x7FFE;  // row 7: SPACE, SYM SHIFT, M, N, B

} // namespace

void TapeTrap::install()
{
    if (installed_) return;
    cpu_->set_trap(LD_BYTES_ENTRY, [this](Cpu* cpu) { return on_ld_bytes(cpu); });
    cpu_->set_trap(SA_BYTES_ENTRY, [this](Cpu* cpu) { return on_sa_bytes(cpu); });
    installed_ = true;
}

void TapeTrap::uninstall()
{
    if (!installed_) return;
    cpu_->clear_trap(LD_BYTES_ENTRY);
    cpu_->clear_trap(SA_BYTES_ENTRY);
    installed_ = false;
}

bool TapeTrap::break_pressed(Cpu* cpu)
{
    const bool caps  = (cpu->port_in(PORT_ROW_CAPS)  & 0x01) == 0;
    const bool space = (cpu->port_in(PORT_ROW_SPACE) & 0x01) == 0;
    return caps && space;
}

bool TapeTrap::on_ld_bytes(Cpu* cpu)
{
    // With no tape in the deck, decline and let the genuine ROM routine run.
    // It will wait for tape edges that never come and report its own error,
    // which is exactly what the machine does with no tape in the recorder.
    if (!deck_->inserted()) return false;

    // Likewise for BREAK: declining hands control back to the ROM, whose own
    // loop knows how to abort cleanly.
    if (break_pressed(cpu)) return false;

    RegisterState r = cpu->get_registers();

    // On entry: A = expected flag byte, IX = destination, DE = byte count,
    // carry set = LOAD, carry clear = VERIFY.
    const ::byte expected_flag = r.a;
    const bool verify = (r.f & FLAG_CARRY) == 0;

    const TapeDeck::LoadOutcome outcome =
        deck_->load_block(expected_flag, *cpu->get_memory(), r.ix, r.de, verify);

    load_calls_++;
    last_load_result_ = outcome.result;

    if (outcome.result == TapeDeck::LoadResult::Ok) {
        r.f = static_cast<::byte>(r.f | FLAG_CARRY);
    } else {
        r.f = static_cast<::byte>(r.f & ~FLAG_CARRY);
    }

    // The real routine leaves IX past the last byte stored and DE counted
    // down to zero; on a failure both reflect however far it got.
    r.ix = static_cast<word>(r.ix + outcome.transferred);
    r.de = static_cast<word>(r.de - outcome.transferred);

    cpu->set_registers(r);

    // set_registers restored SP, so the stack top is still the return address
    // pushed by the CALL that reached this entry point. Popping it here is
    // what makes the trap behave as an immediate RET from LD-BYTES.
    cpu->set_pc(cpu->pop_word());
    return true;
}

bool TapeTrap::on_sa_bytes(Cpu* cpu)
{
    // With nowhere to write, decline and let the real routine emit its pilot
    // tone into a recorder that is not there -- which is what the machine
    // does anyway, and it keeps the timing and border stripes authentic.
    if (deck_->path().empty()) return false;
    if (break_pressed(cpu)) return false;

    RegisterState r = cpu->get_registers();

    // On entry: A = flag byte (0x00 header, 0xFF data), IX = start of the data,
    // DE = how many bytes. SA-BYTES writes flag + DE bytes + checksum.
    const ::byte flag = r.a;

    last_save_result_ = deck_->save_block(flag, *cpu->get_memory(), r.ix, r.de);
    save_calls_++;

    // SA-BYTES has no success convention: a real recorder cannot answer back,
    // so the routine just returns. Leave the flags alone -- SA/LD-RET
    // preserves AF across its own work -- and only advance the pointers.
    r.ix = static_cast<word>(r.ix + r.de);
    r.de = 0;
    cpu->set_registers(r);

    cpu->set_pc(cpu->pop_word());
    return true;
}

} // namespace z80
