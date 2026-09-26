// The beeper renderer (milestone 2 of the sound work): speaker transitions
// timestamped in T-states, in; PCM samples, out. No CPU and no SDL involved.

#include "z80/beeper.h"
#include "z80/z80_cpu.h"
#include "test_harness.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

using testing::add_test;
using z80::Beeper;
using z80::Cpu;
using z80::RegisterState;

namespace {

using Samples = std::vector<int16_t>;

constexpr int16_t AMP = 8000;

/// Toggles the speaker every `half_period` T-states for `cycles`, then renders.
Samples square_wave(uint64_t half_period, uint64_t cycles, Beeper& b) {
    Samples out;
    bool level = false;
    for (uint64_t t = 0; t < cycles; t += half_period) {
        b.set_level(t, level);
        level = !level;
    }
    b.render_to(cycles, out);
    return out;
}

/// Counts upward crossings of the half-amplitude line -- one per wave cycle.
int count_cycles(const Samples& s, int16_t amplitude) {
    const int16_t threshold = static_cast<int16_t>(amplitude / 2);
    int crossings = 0;
    bool above = false;
    for (int16_t v : s) {
        if (!above && v > threshold) { crossings++; above = true; }
        else if (above && v < threshold) { above = false; }
    }
    return crossings;
}

bool near(int actual, int expected, int tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

/// Boots the ROM, types `BEEP 1,0`, and returns every speaker transition
/// timestamped in T-states. Real-time pacing is off: what matters is emulated
/// time, and pacing would make this take ten wall-clock seconds.
struct BeepCapture {
    std::vector<uint64_t> toggles;
    bool booted = false;
};

BeepCapture capture_beep() {
    static uint8_t keys[8];
    static std::vector<uint64_t> toggles;
    static bool level;
    for (int i = 0; i < 8; i++) keys[i] = 0x1F;
    toggles.clear();
    level = false;

    BeepCapture result;
    Cpu cpu;
    std::string path = ROM_FILE_NAME;
    if (!cpu.get_memory()->load_rom(path, ROM_START_ADDRESS, ROM_SIZE)) return result;
    cpu.reset();
    cpu.set_realtime(false);

    static long frames;
    frames = 0;
    cpu.set_frame_callback([](Cpu*) { frames++; });
    cpu.set_port_in_callback([](word port) -> byte {
        if ((port & 1) != 0) return 0xFF;
        const byte sel = static_cast<byte>(port >> 8);
        byte k = 0x1F;
        for (int r = 0; r < 8; r++) if ((sel & (1 << r)) == 0) k &= keys[r];
        return static_cast<byte>(k | 0xE0);
    });
    Cpu* cpu_ptr = &cpu;
    cpu.set_port_out_callback([cpu_ptr](word port, byte value) {
        if ((port & 1) != 0) return;
        const bool on = (value & 0x10) != 0;
        if (on != level) { toggles.push_back(cpu_ptr->total_cycles()); level = on; }
    });

    auto advance = [&](long n) { const long t = frames + n; while (frames < t) cpu.step(); };
    auto tap = [&](std::vector<std::pair<int,int>> combo) {
        for (auto& k : combo) keys[k.first] = static_cast<uint8_t>(keys[k.first] & ~(1 << k.second));
        advance(4);
        for (auto& k : combo) keys[k.first] = static_cast<uint8_t>(keys[k.first] | (1 << k.second));
        advance(10);
    };

    advance(140);                  // boot
    result.booted = true;
    toggles.clear();               // ignore the clicks from typing so far

    // BEEP is EXTENDED + SYM SHIFT + Z, then "1,0".
    tap({{0,0},{7,1}});            // both shifts -> extended mode
    tap({{7,1},{0,1}});            // SYM SHIFT + Z -> BEEP
    tap({{3,0}});                  // 1
    tap({{7,1},{7,3}});            // SYM SHIFT + N -> ,
    tap({{4,0}});                  // 0
    tap({{6,0}});                  // ENTER
    advance(120);                  // let the note finish

    result.toggles = toggles;
    return result;
}

} // namespace

void register_beeper_tests() {

    // =============================== the ROM's own beeper, end to end ==

    add_test("beep: BEEP 1,0 plays middle C for one second", [] {
        // The whole chain: the ROM's delay loop, the T-state clock that
        // timestamps its port writes, and the arithmetic that turns those
        // into a pitch. A wrong T-state count for any instruction in that
        // loop shows up here as a detuned note.
        const BeepCapture cap = capture_beep();
        if (!cap.booted || cap.toggles.size() < 100) return false;

        // Isolated clicks bracket the note (the keyboard scan writes this port
        // too), so take the median interval rather than the mean.
        std::vector<uint64_t> gaps;
        for (size_t i = 1; i < cap.toggles.size(); i++) {
            gaps.push_back(cap.toggles[i] - cap.toggles[i - 1]);
        }
        std::sort(gaps.begin(), gaps.end());
        const uint64_t half_period = gaps[gaps.size() / 2];

        // Middle C is 261.63Hz, so a half-cycle is cpu_hz / (2 * 261.63).
        Beeper b;
        const double expected = b.cpu_hz() / (2 * 261.63);
        const double error = std::abs(double(half_period) - expected) / expected;
        if (error > 0.01) return false;

        // BEEP 1,0 asks for one second, which is 523 half-cycles.
        size_t steady = 0;
        for (uint64_t g : gaps) {
            if (std::abs(double(g) - double(half_period)) <= 2) steady++;
        }
        return steady >= 500 && steady <= 540;
    });

    add_test("beep: the rendered samples carry that tone", [] {
        // Same capture, but pushed through the Beeper the application uses,
        // and measured from the PCM rather than from the timestamps.
        const BeepCapture cap = capture_beep();
        if (!cap.booted || cap.toggles.size() < 100) return false;

        Beeper b(44100, 3494400, AMP);
        Samples out;
        bool level = false;
        for (uint64_t t : cap.toggles) { b.set_level(t, level); level = !level; }
        b.render_to(cap.toggles.back(), out);
        if (out.size() < 40000) return false;

        // The longest run of evenly spaced crossings is the note itself.
        const int16_t threshold = AMP / 2;
        std::vector<size_t> xs;
        bool above = false;
        for (size_t i = 0; i < out.size(); i++) {
            if (!above && out[i] > threshold) { xs.push_back(i); above = true; }
            else if (above && out[i] < threshold) { above = false; }
        }
        if (xs.size() < 100) return false;
        std::vector<size_t> gaps;
        for (size_t i = 1; i < xs.size(); i++) gaps.push_back(xs[i] - xs[i-1]);
        std::vector<size_t> sorted = gaps;
        std::sort(sorted.begin(), sorted.end());
        const double period = double(sorted[sorted.size()/2]);   // samples per cycle
        const double freq = 44100.0 / period;
        return near(static_cast<int>(freq), 262, 3);
    });


    // ============================================================= geometry ==

    add_test("beeper: defaults are 44100Hz against the emulator's own clock", [] {
        Beeper b;
        return b.sample_rate() == 44100
            && b.cpu_hz() == CYCLES_PER_FRAME * FRMAES_PER_SECOND
            && b.cpu_hz() == 3494400;
    });

    add_test("beeper: one frame renders exactly 882 samples", [] {
        // 69888 T-states at 3494400Hz is 1/50s, and 44100/50 = 882 exactly.
        Beeper b;
        Samples out;
        b.render_to(CYCLES_PER_FRAME, out);
        return out.size() == 882;
    });

    add_test("beeper: one second renders exactly sample_rate samples", [] {
        Beeper b;
        Samples out;
        b.render_to(b.cpu_hz(), out);
        return out.size() == 44100;
    });

    add_test("beeper: boundaries do not drift over many frames", [] {
        // The remainder carry is the whole point: 79 T-states a sample would
        // lose 210 samples a frame.
        Beeper b;
        Samples out;
        for (int frame = 1; frame <= 500; frame++) {
            b.render_to(static_cast<uint64_t>(frame) * CYCLES_PER_FRAME, out);
        }
        return out.size() == 500 * 882;
    });

    add_test("beeper: frame-by-frame rendering matches one long render", [] {
        Beeper a, b;
        Samples piecemeal, whole;
        for (int frame = 1; frame <= 10; frame++) {
            a.render_to(static_cast<uint64_t>(frame) * CYCLES_PER_FRAME, piecemeal);
        }
        b.render_to(10ull * CYCLES_PER_FRAME, whole);
        return piecemeal == whole;
    });

    // ================================================== constant levels ==

    add_test("beeper: a silent speaker renders zeros", [] {
        Beeper b;
        Samples out;
        b.render_to(CYCLES_PER_FRAME, out);
        for (int16_t v : out) if (v != 0) return false;
        return !out.empty();
    });

    add_test("beeper: a speaker held high renders full amplitude", [] {
        Beeper b(44100, 3494400, AMP);
        Samples out;
        b.set_level(0, true);
        b.render_to(CYCLES_PER_FRAME, out);
        for (int16_t v : out) if (v != AMP) return false;
        return !out.empty();
    });

    add_test("beeper: redundant writes of the same level change nothing", [] {
        Beeper a(44100, 3494400, AMP), b(44100, 3494400, AMP);
        Samples with_dupes, without;
        a.set_level(0, true);
        for (uint64_t t = 100; t < 20000; t += 100) a.set_level(t, true);
        a.render_to(CYCLES_PER_FRAME, with_dupes);
        b.set_level(0, true);
        b.render_to(CYCLES_PER_FRAME, without);
        return with_dupes == without;
    });

    // ==================================================== a single toggle ==

    add_test("beeper: a toggle on a sample boundary splits cleanly", [] {
        // The first sample spans [0,79); switching on at 79 makes sample 0
        // silent and sample 1 fully high.
        Beeper b(44100, 3494400, AMP);
        Samples out;
        b.set_level(79, true);
        b.render_to(1000, out);
        return out.size() >= 2 && out[0] == 0 && out[1] == AMP;
    });

    add_test("beeper: a toggle mid-sample gives a partial value", [] {
        // Half way through the first 79-T-state sample, so about half scale --
        // a point reading would have had to round it to 0 or to full.
        Beeper b(44100, 3494400, AMP);
        Samples out;
        b.set_level(40, true);
        b.render_to(1000, out);
        if (out.empty()) return false;
        const int expected = AMP * 39 / 79;      // high for T-states 40..78
        return near(out[0], expected, 1) && out[1] == AMP;
    });

    add_test("beeper: a brief pulse inside one sample is not lost", [] {
        // On for 20 of the sample's 79 T-states. Point sampling would miss it
        // entirely; averaging keeps its energy.
        Beeper b(44100, 3494400, AMP);
        Samples out;
        b.set_level(10, true);
        b.set_level(30, false);
        b.render_to(1000, out);
        if (out.empty()) return false;
        return near(out[0], AMP * 20 / 79, 1) && out[1] == 0;
    });

    // ======================================================= square waves ==

    add_test("beeper: a 1kHz square wave comes out at 1kHz", [] {
        Beeper b(44100, 3494400, AMP);
        // half period = cpu_hz / (2 * 1000)
        const Samples out = square_wave(3494400 / 2000, 3494400, b);
        return out.size() == 44100 && near(count_cycles(out, AMP), 1000, 2);
    });

    add_test("beeper: middle C comes out at 261Hz", [] {
        // The pitch BEEP 1,0 plays -- the target for the end-to-end test later.
        Beeper b(44100, 3494400, AMP);
        const uint64_t half = static_cast<uint64_t>(3494400 / (2 * 261.63));
        const Samples out = square_wave(half, 3494400, b);
        return near(count_cycles(out, AMP), 262, 2);
    });

    add_test("beeper: a square wave swings the full range", [] {
        Beeper b(44100, 3494400, AMP);
        const Samples out = square_wave(3494400 / 2000, 3494400 / 10, b);
        int16_t lo = AMP, hi = 0;
        for (int16_t v : out) { if (v < lo) lo = v; if (v > hi) hi = v; }
        return lo == 0 && hi == AMP;
    });

    add_test("beeper: a tone above Nyquist averages out instead of aliasing", [] {
        // Toggling every 4 T-states is ~437kHz, far above the 22kHz limit.
        // Averaging settles near half scale; point sampling would fold it down
        // into an audible whistle at full swing.
        Beeper b(44100, 3494400, AMP);
        const Samples out = square_wave(4, 3494400 / 100, b);
        if (out.size() < 100) return false;
        int16_t lo = AMP, hi = 0;
        for (size_t i = 1; i < out.size(); i++) {   // skip the first partial
            if (out[i] < lo) lo = out[i];
            if (out[i] > hi) hi = out[i];
        }
        // Everything should sit close to half scale, so the swing is tiny.
        return near(lo, AMP / 2, AMP / 12) && near(hi, AMP / 2, AMP / 12);
    });

    // ========================================================== mechanics ==

    add_test("beeper: rendering is continuous across calls", [] {
        Beeper a(44100, 3494400, AMP), b(44100, 3494400, AMP);
        Samples split, whole;
        for (Beeper* p : { &a, &b }) {
            bool level = false;
            for (uint64_t t = 0; t < 40000; t += 1747) {
                p->set_level(t, level);
                level = !level;
            }
        }
        a.render_to(10000, split);
        a.render_to(20000, split);
        a.render_to(40000, split);
        b.render_to(40000, whole);
        return split == whole && !split.empty();
    });

    add_test("beeper: the part-rendered sample is held back until whole", [] {
        Beeper b;
        Samples out;
        b.render_to(40, out);          // less than one sample
        return out.empty() && b.pending() == 0;
    });

    add_test("beeper: a backwards timestamp is ignored, not honoured", [] {
        Beeper a(44100, 3494400, AMP), b(44100, 3494400, AMP);
        Samples with_bad, without;
        a.set_level(1000, true);
        a.set_level(500, false);       // bogus: the clock never rewinds
        a.render_to(20000, with_bad);
        b.set_level(1000, true);
        b.set_level(1000, false);
        b.render_to(20000, without);
        return with_bad == without;
    });

    add_test("beeper: restart re-origins the clock", [] {
        Beeper b;
        Samples out;
        b.render_to(CYCLES_PER_FRAME, out);
        out.clear();
        b.restart(1000000);
        b.render_to(1000000 + CYCLES_PER_FRAME, out);
        return out.size() == 882;
    });

    add_test("beeper: restart drops a part-rendered sample", [] {
        Beeper b(44100, 3494400, AMP);
        Samples out;
        b.set_level(0, true);
        b.render_to(40, out);          // mid-sample
        b.restart(40);
        b.render_to(40 + 79, out);
        // The sample after the restart is whole and high, not a half-value
        // carried over from before.
        return out.size() == 1 && out[0] == AMP;
    });

    add_test("beeper: amplitude scales the output", [] {
        Beeper b(44100, 3494400, 1000);
        Samples out;
        b.set_level(0, true);
        b.render_to(CYCLES_PER_FRAME, out);
        if (out.empty() || out[0] != 1000) return false;
        b.set_amplitude(4000);
        out.clear();
        b.render_to(2 * CYCLES_PER_FRAME, out);
        return !out.empty() && out.back() == 4000;
    });

    add_test("beeper: a non-default sample rate still divides exactly", [] {
        Beeper b(22050, 3494400, AMP);
        Samples out;
        b.render_to(b.cpu_hz(), out);
        return out.size() == 22050;
    });
}
