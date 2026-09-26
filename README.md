# ZX Spectrum Emulator

A ZX Spectrum 48K emulator written from scratch in C++20, with SDL2 for video,
keyboard and sound. It boots the original Sinclair ROM, runs BASIC, saves and
loads programs to a virtual cassette, and plays the beeper.

Alongside the emulator are two command-line tools, `bas2tap` and `tap2bas`,
that convert between plain-text BASIC listings and `.tap` tape images.

## Features

- **Complete Z80 instruction set** — all five opcode pages, including the
  undocumented instructions (`IXH`/`IXL`, `SLL`, the indexed-CB register copy).
- **Interrupts** — the 50 Hz maskable interrupt in modes 0, 1 and 2, which is
  what drives the ROM's keyboard scan.
- **Display** — the interleaved screen layout, attributes, `BRIGHT`, `FLASH` and
  the border colour, rendered through a single streaming texture.
- **Keyboard** — the full 8×5 matrix mapped onto a PC keyboard by position.
- **Tape** — `LOAD`, `SAVE` and `VERIFY` from BASIC against a virtual cassette,
  with multi-program tapes and load-by-name.
- **Sound** — the beeper, rendered from T-state-accurate timestamps with an
  anti-aliasing filter.
- **ROM protection** — writes to `0x0000–0x3FFF` are discarded, as on real
  hardware. The ROM relies on this.

## Installation

The emulator needs a C++20 compiler, CMake 3.20 or later, the SDL2 development
files, and the Sinclair 48K ROM image. These instructions were tested on
Ubuntu 22.04, both natively and under WSL.

### Linux (Ubuntu)

**1. Install the tools and libraries.**

The ROM image comes from the `spectrum-roms` package, which Ubuntu keeps in its
*multiverse* component. That isn't always enabled on a fresh install, so turn
it on first — without it `apt` will report that it can't find the package:

```sh
sudo add-apt-repository multiverse
sudo apt update
sudo apt install build-essential cmake libsdl2-dev spectrum-roms
```

This installs the ROM at `/usr/share/spectrum-roms/48.rom`, which is where the
emulator looks for it.

On **Debian**, the other packages have the same names, but there is no
multiverse component, so skip `add-apt-repository`. `spectrum-roms` sits
outside Debian's default `main` section: either enable the section that carries
it, or install the ROM by hand as described under
[Other Linux distributions](#other-linux-distributions).

**2. Build.** From the project directory:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
```

**3. Run.**

```sh
./build/release/zx-spectrum-emulator
```

A window opens showing `© 1982 Sinclair Research Ltd`, and the terminal prints
a line confirming the audio device.

### Windows (WSL)

On Windows the emulator runs inside WSL, the Windows Subsystem for Linux. Its
window and its sound reach the Windows desktop through **WSLg**, the part of
WSL that runs Linux graphical apps. WSLg needs Windows 11, or Windows 10
build 19044 or later with the Microsoft Store version of WSL.

**1. Install WSL and Ubuntu.** Open PowerShell *as Administrator* and run:

```powershell
wsl --install
```

This installs WSL 2 with Ubuntu. Restart when prompted, then open **Ubuntu**
from the Start menu and choose a Linux username and password.

If WSL was already installed, bring it up to date so that WSLg is present:

```powershell
wsl --update
wsl --shutdown
```

**2. Check that WSLg is working.** In the Ubuntu terminal:

```sh
ls /mnt/wslg
```

If that lists files, graphical apps and sound will work. If the directory
doesn't exist, run `wsl --update` again from PowerShell.

**3. Put the project in the Linux filesystem.** Keep it under your Linux home
directory, for example `~/git/zx-spectrum-emulator`, rather than on a Windows
drive under `/mnt/c`. Builds are far slower across that boundary.

**4. Follow the Linux steps above** inside the Ubuntu terminal — enable
multiverse, install the packages, build, and run. The emulator window appears
on the Windows desktop like any other.

**Using VS Code.** Install the **WSL** extension in VS Code on Windows, then
from the Ubuntu terminal, in the project directory, run:

```sh
code .
```

VS Code reopens connected to WSL, and the build tasks and debugger work as
they would on Linux.

### Other Linux distributions

Package names differ, but you need the same four things: a C++20 compiler,
CMake 3.20 or later, the SDL2 development headers, and the 48K ROM. For
example — these commands are untested here:

```sh
sudo dnf install gcc-c++ cmake SDL2-devel     # Fedora
sudo pacman -S base-devel cmake sdl2          # Arch
```

The `spectrum-roms` package is specific to Debian and Ubuntu. Elsewhere, obtain
the 48K ROM image and copy it to `/usr/share/spectrum-roms/48.rom`, or point
`ROM_FILE_NAME` in [include/z80/globals.h](include/z80/globals.h) at wherever
you keep it, then rebuild.

### Checking the install

Build the debug configuration and run the test suite:

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
./build/debug/z80-emulator-test
```

It finishes in about a second and should end with `0 failed`. Several tests
boot the real ROM, so a passing run also confirms the ROM was found.

### Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `Unable to locate package spectrum-roms` | The *multiverse* component isn't enabled. Run `sudo add-apt-repository multiverse` and `sudo apt update`. |
| CMake reports it can't find SDL2 | The development package is missing: `sudo apt install libsdl2-dev`. |
| `ROM not found at '/usr/share/spectrum-roms/48.rom'` | The ROM image is missing. Install `spectrum-roms`, or set `ROM_FILE_NAME` in `include/z80/globals.h` and rebuild. The window still opens, but shows only a border. |
| `SDL_Init failed` or `SDL_CreateWindow failed` under WSL | WSLg isn't available. From PowerShell run `wsl --update`, then `wsl --shutdown`, and reopen Ubuntu. |
| `Audio unavailable ... running silent` | No audio device was found. The emulator still works, just without sound. Under WSL, `wsl --update` usually fixes it. |
| Builds are very slow under WSL | The project is on a Windows drive (`/mnt/c/...`). Move it into your Linux home directory. |

## Building

Once the prerequisites are installed:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
```

For a debug build, use `build/debug` and `-DCMAKE_BUILD_TYPE=Debug`. In VS Code
the same four steps are available as tasks: *CMake: Configure (Debug)*,
*CMake: Configure (Release)*, *Build (Debug)* and *Build (Release)*.

This produces:

| Target | What it is |
|---|---|
| `libz80-emulator.so` | The CPU, memory, tape and beeper, as a shared library |
| `zx-spectrum-emulator` | The emulator itself |
| `z80-emulator-test` | The automated test suite |
| `bas2tap` | Converts a `.bas` listing into a `.tap` file |
| `tap2bas` | Converts a `.tap` file back into `.bas` listings |

## Running the emulator

```sh
./build/release/zx-spectrum-emulator
```

It boots to the familiar `© 1982 Sinclair Research Ltd` screen. Run it from the
directory where you want your tape to live — see [Tape](#tape).

| Key | Action |
|---|---|
| Left Shift | CAPS SHIFT |
| Right Shift | SYMBOL SHIFT |
| Left Shift + Right Shift | Extended mode, for one keypress |
| Left Shift + Space | BREAK |
| F5 | Rewind the tape |
| F6 | Reload the tape file from disk |
| F7 | Start a fresh, blank tape |
| F8 | Mute or unmute the sound |
| Esc | Quit |

To capture the sound to a file, pass `--wav`:

```sh
./build/release/zx-spectrum-emulator --wav session.wav
```

## Keyboard

The Spectrum's 40 keys map onto a PC keyboard by **position**, not by the
character printed on the key. `S` at the start of a line gives `SAVE`, `J`
gives `LOAD`, and `"` is Right Shift + `P` — exactly as on the real machine.

One quirk worth knowing: extended mode followed by a *letter* gives that
letter's keyword, but extended mode followed by a *digit* gives a colour
control code. The digit keywords (`DEF FN`, `LINE`, `FORMAT`…) need extended
mode **plus** Right Shift.

The full mapping, with every legend read out of the ROM, is in
[docs/keyboard-map.pdf](docs/keyboard-map.pdf).

## Tape

The emulator has one virtual cassette, stored as `tape.tap` in the working
directory. If it doesn't exist yet, the first `SAVE` creates it.

| Command | Does |
|---|---|
| `SAVE "myprog"` | Saves the program — press any key at the prompt |
| `LOAD ""` | Loads the next program on the tape |
| `LOAD "myprog"` | Searches forward for that program |
| `VERIFY "myprog"` | Checks a save against memory |

It behaves like a real cassette: recording goes on the end, and loading only
searches *forward*. **Press F5 before each `LOAD`.** Repeated saves accumulate,
so press F7 to start clean when you want a fresh tape.

Loading is instant, because the emulator intercepts the ROM's tape routines and
moves the bytes directly rather than emulating the audio signal. The flip side
is that games with turbo or custom loaders will not load — see
[Limitations](#limitations).

The full guide is in [docs/tape-manual.pdf](docs/tape-manual.pdf).

## bas2tap and tap2bas

Write BASIC in any text editor, convert it to a tape, and load it in the
emulator:

```sh
bas2tap program.bas program.tap [name] [autostart_line]
```

Or go the other way, extracting the source from a tape — one `.bas` file per
BASIC program on it, named after the name each was saved under:

```sh
tap2bas program.tap [output_dir]
```

`tap2bas` prints the exact `bas2tap` command that rebuilds each program, and
skips anything that isn't BASIC (such as `CODE` blocks) with a note.

### Characters beyond ASCII

The Spectrum's character set extends past ASCII. So that `.bas` files stay
readable text, both tools use the same conventions:

| In the file | Means |
|---|---|
| `▝ ▘ ▀ ▗ ▐ ▚ ▜ ▖ ▞ ▌ ▛ ▄ ▟ ▙ █` | Block graphics `0x81`–`0x8F`, drawn as they appear on screen |
| `\xNN` | Any other byte: UDGs, colour controls, the blank graphic `0x80` |
| `\\` | A literal backslash |

A program passed through `bas2tap` and then `tap2bas` comes back byte-for-byte
identical.

## Tests

```sh
./build/debug/z80-emulator-test
```

The suite prints each test in green or red, with a summary at the end. It
covers every opcode, the interrupt and trap mechanisms, the address space and
ROM protection, the tape format and the deck, and instruction timing — including
an end-to-end check that boots the real ROM, types `BEEP 1,0`, and confirms the
note comes out as middle C.

It runs in well under a second: tests that drive the ROM switch off real-time
pacing with `Cpu::set_realtime(false)`, since they care about emulated time
rather than wall-clock time.

## Project layout

```
include/z80/           Public headers
  z80_cpu.h              The CPU
  z80_mem.h              Memory, with ROM write protection
  tape.h                 .tap parsing and the virtual tape deck
  tape_trap.h            Hooks the ROM's LD-BYTES / SA-BYTES routines
  beeper.h               Turns speaker toggles into PCM samples
  basic_tokens.h         BASIC keyword table and escapes, shared by the tools
  keyb_lookup.h          PC-to-Spectrum keyboard mapping
  globals.h              Memory map and file paths
src/z80/               The library
src/zx_spectrum_emulator/  The SDL front end
src/bas2tap/           .bas -> .tap
src/tap2bas/           .tap -> .bas
tests/z80_emulator_test/   The test suite
docs/                  Manuals and example programs
```

## Limitations

- **Turbo and custom tape loaders don't work.** They bypass the ROM routines the
  emulator intercepts. Standard-loader programs and type-ins are fine.
- **`.tzx` files aren't supported**, only `.tap`.
- **No memory contention.** On real hardware the ULA steals CPU cycles in
  `0x4000–0x7FFF`; timing-critical effects that depend on it won't be exact.
- **The border is drawn once per frame**, so mid-frame border effects don't show.
- **The emulator runs at exactly 50 frames per second.** A real 48K runs at
  50.08, so pitch and speed are about 0.16% slow — roughly three cents,
  inaudible.

## Documentation

- [docs/keyboard-map.pdf](docs/keyboard-map.pdf) — every key and legend, and how
  it maps to a PC keyboard
- [docs/tape-manual.pdf](docs/tape-manual.pdf) — saving, loading and the tape
  controls
- [docs/Examples/](docs/Examples/) — example BASIC programs

Each PDF has an HTML source beside it, with the command to regenerate it in a
comment at the top.

## License

This project is released under the MIT License — see [LICENSE](LICENSE).

The ZX Spectrum ROM is not part of this project. It is copyright Amstrad, is
not included in this repository, and is not covered by this licence. Amstrad
allows it to be distributed for use with emulators, which is how packages such
as `spectrum-roms` provide it — install it separately as described under
[Installation](#installation).
