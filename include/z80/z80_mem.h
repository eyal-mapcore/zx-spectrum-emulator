#pragma once

#include <cstdint>
#include <string>
#include <atomic>

#include "globals.h"

// Keyboard map
//std::atomic<uint8_t> key_matrix[8] = { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F };

namespace z80 {
    class Memory {
    public:
        Memory(size_t size = MEMORY_SIZE);
        ~Memory() = default;

        bool load_rom(std::string& path, word address, size_t size);
        ::byte read(word address) const { return memory_[address]; }

        /// Raw access, for the host rather than the emulated CPU: rendering,
        /// the ROM loader, test fixtures. It bypasses write protection, so
        /// anything standing in for the Z80 should use write()/writable().
        ::byte *data() { return &memory_[0];}

        /// Stores from the emulated CPU. Writes below rom_end() are discarded,
        /// because on a 48K Spectrum 0x0000-0x3FFF is ROM and the hardware
        /// simply ignores stores there.
        ///
        /// This is not a nicety the ROM merely tolerates -- it depends on it.
        /// SKIP-CONS at 0x33FB does `LD DE,$0000` and then calls STK-CONST, so
        /// that stacking a 5-byte floating-point constant into address 0
        /// throws it away. Every numeric literal in a BASIC line goes through
        /// that path, so without this the reset vector is overwritten the
        /// first time anyone types a number.
        void write(word address, ::byte value) {
            if (address >= rom_end_) memory_[address] = value;
        }

        /// A pointer to store through, for read-modify-write instructions
        /// (INC (HL), SET b,(IX+d)) and for operand resolution that does not
        /// yet know whether it will read or write.
        ///
        /// In RAM this is the cell itself. In ROM it is a scratch byte
        /// preloaded with the ROM contents, so the read half sees the true
        /// value and the store half is discarded -- exactly what the hardware
        /// does. Only one such pointer is live at a time; no Z80 instruction
        /// resolves two memory operands.
        ::byte* writable(word address) {
            if (address >= rom_end_) return &memory_[address];
            rom_sink_ = memory_[address];
            return &rom_sink_;
        }

        /// First writable address. Defaults to ROM_SIZE; settable so a test or
        /// a non-Spectrum machine can make the whole space RAM.
        size_t rom_end() const { return rom_end_; }
        void set_rom_end(size_t address) { rom_end_ = address; }

    private:
        ::byte memory_[MEMORY_SIZE + MEMORY_DECODE_GUARD];
        // size_t, not word: MEMORY_SIZE is 0x10000 and would truncate to 0.
        size_t size_ = 0;
        size_t rom_end_ = ROM_SIZE;
        /// Where discarded ROM stores land. See writable().
        ::byte rom_sink_ = 0;
  };

};