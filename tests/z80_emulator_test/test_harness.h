#pragma once

// Shared test registry. Deliberately NOT in an anonymous namespace: each
// translation unit would then get its own list and only one would be run.

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace testing {

struct Test {
    std::string name;
    std::function<bool()> fn;
};

inline std::vector<Test>& tests() {
    static std::vector<Test> t;
    return t;
}

inline void add_test(const std::string& name, std::function<bool()> fn) {
    tests().push_back({name, std::move(fn)});
}

} // namespace testing

// Registered from their own translation units.
void register_tape_tests();
void register_trap_tests();
void register_memory_tests();
void register_tape_trap_tests();
void register_rom_protect_tests();
void register_timing_tests();
void register_beeper_tests();
