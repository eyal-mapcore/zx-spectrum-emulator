#pragma once

#include <cstdint>
#include <string>
#include <atomic>

#define ROM_FILE_NAME "/usr/share/spectrum-roms/48.rom"

// The virtual tape, relative to the working directory. SAVE creates it if it
// is not there, so the emulator starts fine with no tape present.
#define TAPE_FILE_NAME "tape.tap"

typedef std::uint8_t byte;
typedef std::uint16_t word;

// The Z80's address space is 16 bits, so 0x10000 bytes -- 0xFFFF would be one
// byte short and leave the top address unaddressable.
const size_t MEMORY_SIZE = 0x10000;

// decode() reads an instruction's bytes before step() can know its length and
// bounds-check it, so a fetch at the very top of memory reads up to
// kMaxInstructionLength-1 bytes past the end. These guard bytes absorb that;
// they sit beyond the 16-bit address space and so are unreachable by emulated
// code.
const size_t MEMORY_DECODE_GUARD = 3;
const size_t ROM_SIZE = 0x4000;
const word ROM_START_ADDRESS = 0x0000;

// Screen Memory layout for ZX Spectrum 48K
const size_t SCREEN_MEMORY_START = 0x4000;
const size_t SCREEN_MEMORY_END = 0x57FF;
const size_t SCREEN_MEMORY_SIZE = 0x1800;

// Attribute file: one byte per 8x8 character cell (32 x 24), laid out
// linearly (unlike the interleaved display file above).
//   bit 7    FLASH    bit 6    BRIGHT
//   bits 5-3 PAPER    bits 2-0 INK
const size_t ATTRIBUTE_MEMORY_START = 0x5800;
const size_t ATTRIBUTE_MEMORY_END = 0x5AFF;
const size_t ATTRIBUTE_MEMORY_SIZE = 0x300;

enum : byte { PFX_CB = 0xCB, PFX_ED = 0xED, PFX_DD = 0xDD, PFX_FD = 0xFD };    


