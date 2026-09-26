#include "z80/z80_mem.h"
#include <fstream>
#include <iostream>
#include <memory.h>

using namespace std;

namespace z80 
{
    Memory::Memory(size_t size) :
        size_(size)
    {
        // Zero the whole array, guard bytes included, rather than just size_
        // bytes -- a short Memory would otherwise leave the tail undefined.
        memset(memory_, 0, sizeof(memory_));
    }
    bool Memory::load_rom(std::string& path, word address, size_t size)
    {
        if ((size_t)address + size > size_)
            // Address and size exceed memory bounds
            return false;
        // Implementation for loading ROM file
        ifstream rom_file(path, std::ios::binary);
        if (!rom_file.is_open())
            // Failed to open ROM file
            return false;
        rom_file.read(reinterpret_cast<char *>(memory_) + address, size);
        rom_file.close();
        return true;
    }
}