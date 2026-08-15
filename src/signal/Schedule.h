// The two lowering steps that decide state: bands, and delay-line sizing.
#pragma once

#include "signal/Interval.h"
#include "signal/Promote.h"
#include "signal/Signal.h"
#include "signal/Type.h"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace faustlens {

// Once at a known sample rate, once per `compute` call, once per frame.
enum class Band : uint8_t { Init, Control, Sample };

std::vector<Band> AssignBands(std::span<const Variability>);

// Maximum delay per node id, by a local rule: `(x@10)@10` keeps ten samples of `x@10`
// and ten of `x`. Unexpected where an index is not bounded, saying which.
std::expected<std::vector<int32_t>, std::string> MaxDelays(const Signals &, std::span<const Interval>, std::span<const SigId> roots);

// State migration keys on exactly this, so the sizing below is behaviour, not tuning.
struct DelayLine {
    SigId Sig = NoSig;
    Nature Nature = Nature::Real;
    int32_t MaxDelay = 0;
    uint32_t Extent = 0; // slots, 0 where no array is needed
    bool Ring = false; // indexed through the shared `IOTA` counter
};

// Separate from `DelayLines` because lowering allocates on demand, and both must agree.
DelayLine LineFor(const Signals &, SigId, int32_t max_delay, Nature);

// In ascending node id order, one entry per line `LineFor` sizes above zero.
std::vector<DelayLine> DelayLines(const Signals &, std::span<const int32_t> max_delay, std::span<const Nature>, std::span<const SigId> roots);

} // namespace faustlens
