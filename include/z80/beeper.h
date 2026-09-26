#pragma once

#include <cstdint>
#include <vector>

#include "globals.h"
#include "z80_cpu.h"

namespace z80 {

const int BEEPER_DEFAULT_SAMPLE_RATE = 44100;

/// T-states per second the emulator actually runs at. Deliberately derived
/// from the emulator's own frame pacing rather than from real hardware's
/// 3.5MHz: a Spectrum's 69888-T-state frame at 3.5MHz is 50.08 frames a
/// second, but step() paces exactly FRMAES_PER_SECOND. Matching the emulator
/// keeps audio from drifting against the frames that produce it, and costs
/// 0.16% of pitch -- under three cents, inaudible.
const int BEEPER_DEFAULT_CPU_HZ = CYCLES_PER_FRAME * FRMAES_PER_SECOND;

/// Peak sample value. The beeper is a square wave and full scale is painfully
/// loud, so this is well below it.
const int16_t BEEPER_DEFAULT_AMPLITUDE = 8000;

/// Turns speaker-level changes into PCM.
///
/// A Spectrum makes sound by toggling one bit -- port 0xFE bit 4 -- so the
/// pitch is decided entirely by *when* the toggles happen. This class takes
/// those transitions, timestamped in T-states from Cpu::total_cycles(), and
/// renders them into mono 16-bit samples.
///
/// Each output sample is the *mean* level across the T-states it spans, not
/// the level sampled at an instant. That box filter is what stops a square
/// wave aliasing into noise: a toggle in the middle of a sample produces a
/// half-value rather than being rounded to one side or lost entirely. It also
/// means a tone above the Nyquist limit fades towards a steady half level
/// instead of folding down into an audible whistle.
///
/// Samples are unipolar, 0 for a level that stayed low and `amplitude` for one
/// that stayed high, matching the hardware: the speaker really is on or off,
/// and the click when it first switches on is a real Spectrum click. A
/// DC-blocking filter could be layered on later if the standing offset of an
/// idle-high speaker proves annoying.
///
/// Single-threaded by design, like TapeDeck: it is fed and drained from
/// whichever thread runs the CPU.
class Beeper {
public:
    explicit Beeper(int sample_rate = BEEPER_DEFAULT_SAMPLE_RATE,
                    int cpu_hz = BEEPER_DEFAULT_CPU_HZ,
                    int16_t amplitude = BEEPER_DEFAULT_AMPLITUDE);

    /// Records the speaker changing to `on` at `cycle`. Time up to that point
    /// is accounted for at the previous level, so callers may skip writes that
    /// do not change anything without affecting the output.
    void set_level(uint64_t cycle, bool on);

    /// Appends every sample completed up to `cycle`. The sample straddling
    /// `cycle` is held back until it is whole, so consecutive calls join
    /// seamlessly. Call it once a frame; samples accumulate internally until
    /// drained.
    void render_to(uint64_t cycle, std::vector<int16_t>& out);

    /// Re-origins the clock at `cycle` and discards any part-rendered sample.
    /// Use at start-up, not between frames.
    void restart(uint64_t cycle);

    bool level() const { return level_; }
    int sample_rate() const { return sample_rate_; }
    int cpu_hz() const { return cpu_hz_; }
    int16_t amplitude() const { return amplitude_; }
    void set_amplitude(int16_t amplitude) { amplitude_ = amplitude; }

    /// Samples rendered but not yet taken.
    size_t pending() const { return out_.size(); }

private:
    void advance_to(uint64_t cycle);
    void begin_sample();

    int sample_rate_;
    int cpu_hz_;
    int16_t amplitude_;

    // Sample boundaries land on exact T-states via a remainder carry, so they
    // never drift: cpu_hz / sample_rate whole T-states per sample, plus a
    // fraction accumulated in error_. At the defaults that is 79 T-states plus
    // 10500/44100, giving exactly 882 samples per 69888-T-state frame.
    uint64_t span_whole_ = 0;
    uint64_t span_fraction_ = 0;
    uint64_t error_ = 0;

    uint64_t sample_start_ = 0;  // first T-state of the sample in progress
    uint64_t sample_end_ = 0;    // one past its last T-state
    uint64_t cursor_ = 0;        // time accounted for so far
    uint64_t high_cycles_ = 0;   // of which the level was high for this many
    bool level_ = false;

    std::vector<int16_t> out_;
};

} // namespace z80
