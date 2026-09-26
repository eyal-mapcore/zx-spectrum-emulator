#include "z80/beeper.h"

namespace z80 {

Beeper::Beeper(int sample_rate, int cpu_hz, int16_t amplitude)
    : sample_rate_(sample_rate > 0 ? sample_rate : BEEPER_DEFAULT_SAMPLE_RATE),
      cpu_hz_(cpu_hz > 0 ? cpu_hz : BEEPER_DEFAULT_CPU_HZ),
      amplitude_(amplitude)
{
    span_whole_    = static_cast<uint64_t>(cpu_hz_ / sample_rate_);
    span_fraction_ = static_cast<uint64_t>(cpu_hz_ % sample_rate_);
    restart(0);
}

void Beeper::restart(uint64_t cycle)
{
    error_        = 0;
    sample_start_ = cycle;
    cursor_       = cycle;
    begin_sample();
    out_.clear();
}

void Beeper::begin_sample()
{
    uint64_t span = span_whole_;

    // Carry the leftover fraction of a T-state forward, so that boundaries sit
    // on exact T-states and a second of samples spans exactly a second of
    // T-states however long the emulator runs.
    error_ += span_fraction_;
    if (error_ >= static_cast<uint64_t>(sample_rate_)) {
        error_ -= static_cast<uint64_t>(sample_rate_);
        span++;
    }

    sample_end_  = sample_start_ + span;
    high_cycles_ = 0;
}

void Beeper::advance_to(uint64_t cycle)
{
    // The T-state clock is monotonic, so this should not happen; ignoring it
    // rather than rewinding keeps a stray caller from corrupting the stream.
    if (cycle <= cursor_) return;

    while (cycle >= sample_end_) {
        if (level_) high_cycles_ += sample_end_ - cursor_;

        // The sample is the mean level over its own span -- see the note in
        // the header on why this is an average and not a point reading.
        const uint64_t span = sample_end_ - sample_start_;
        const uint64_t value =
            (static_cast<uint64_t>(amplitude_) * high_cycles_) / span;
        out_.push_back(static_cast<int16_t>(value));

        cursor_       = sample_end_;
        sample_start_ = sample_end_;
        begin_sample();
    }

    if (level_) high_cycles_ += cycle - cursor_;
    cursor_ = cycle;
}

void Beeper::set_level(uint64_t cycle, bool on)
{
    advance_to(cycle);
    level_ = on;
}

void Beeper::render_to(uint64_t cycle, std::vector<int16_t>& out)
{
    advance_to(cycle);
    out.insert(out.end(), out_.begin(), out_.end());
    out_.clear();
}

} // namespace z80
