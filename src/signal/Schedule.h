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

enum class Band : uint8_t { Init, Control, Sample };

std::vector<Band> AssignBands(std::span<const Variability>);

// Return maximum local delays per node, or an error for unbounded indices.
std::expected<std::vector<int32_t>, std::string> MaxDelays(const Signals &, std::span<const Interval>, std::span<const SigId> roots);

// Preserve these layout categories for state migration.
struct DelayLine {
    SigId Sig = NoSig;
    Nature Nature = Nature::Real;
    int32_t MaxDelay = 0;
    uint32_t Extent = 0; // slots, 0 where no array is needed
    bool Ring = false; // indexed through the shared `IOTA` counter
};

// Use the same sizing rule for analysis and on-demand lowering.
DelayLine LineFor(const Signals &, SigId, int32_t max_delay, Nature);

// Return nonempty delay lines in ascending node-id order.
std::vector<DelayLine> DelayLines(const Signals &, std::span<const int32_t> max_delay, std::span<const Nature>, std::span<const SigId> roots);

} // namespace faustlens
