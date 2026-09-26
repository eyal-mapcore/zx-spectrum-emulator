cmake_minimum_required(VERSION 3.20)
project(zx_spectrum_emulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL2 REQUIRED)

# --- z80-emulator shared library (libz80-emulator.so) ---------------------
add_library(z80-emulator SHARED
    src/z80/z80_cpu.cpp
    src/z80/z80_mem.cpp
    src/z80/tape.cpp
    src/z80/tape_trap.cpp
    src/z80/beeper.cpp
)

target_include_directories(z80-emulator
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(z80-emulator PUBLIC cxx_std_20)

set_target_properties(z80-emulator PROPERTIES
    OUTPUT_NAME "z80-emulator"
    VERSION 1.0.0
    SOVERSION 1
)

# --- zx-spectrum-emulator executable ---------------------------------------
add_executable(zx-spectrum-emulator
    src/zx_spectrum_emulator/main.cpp
)

target_link_libraries(zx-spectrum-emulator
    PRIVATE
        z80-emulator
        $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,${SDL2_LIBRARIES}>
)

target_include_directories(zx-spectrum-emulator PRIVATE ${SDL2_INCLUDE_DIRS})
target_compile_features(zx-spectrum-emulator PRIVATE cxx_std_20)

# --- z80-emulator-test executable -------------------------------------------
add_executable(z80-emulator-test
    tests/z80_emulator_test/main.cpp
    tests/z80_emulator_test/tape_tests.cpp
    tests/z80_emulator_test/trap_tests.cpp
    tests/z80_emulator_test/memory_tests.cpp
    tests/z80_emulator_test/tape_trap_tests.cpp
    tests/z80_emulator_test/rom_protect_tests.cpp
    tests/z80_emulator_test/timing_tests.cpp
    tests/z80_emulator_test/beeper_tests.cpp
)

target_link_libraries(z80-emulator-test
    PRIVATE
        z80-emulator
        $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,${SDL2_LIBRARIES}>
)

target_include_directories(z80-emulator-test PRIVATE ${SDL2_INCLUDE_DIRS})
target_compile_features(z80-emulator-test PRIVATE cxx_std_20)

enable_testing()
add_test(NAME z80-emulator-test COMMAND z80-emulator-test)

#---- bas2tap executable ------------------------------------------------------

add_executable(bas2tap
    src/bas2tap/bas2tap.cpp
)

target_include_directories(bas2tap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(bas2tap PRIVATE cxx_std_20)

set_target_properties(bas2tap PROPERTIES
    OUTPUT_NAME "bas2tap"
    VERSION 1.0.0
    SOVERSION 1
)

#---- tap2bas executable ------------------------------------------------------
# The inverse of bas2tap. Links z80-emulator for .tap parsing rather than
# re-implementing it: TapFile already handles framing, checksums and headers.

add_executable(tap2bas
    src/tap2bas/tap2bas.cpp
)

target_link_libraries(tap2bas PRIVATE z80-emulator)
target_include_directories(tap2bas PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(tap2bas PRIVATE cxx_std_20)

set_target_properties(tap2bas PROPERTIES
    OUTPUT_NAME "tap2bas"
    VERSION 1.0.0
    SOVERSION 1
)
