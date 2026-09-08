#pragma once

#include "signal/Interval.h"
#include "signal/Signal.h"

#include <cstdint>
#include <vector>

namespace faustlens {

// Reference values leave a gap at 2 so bitwise OR implements the variability join.
enum class Variability : uint8_t { Konst = 0, Block = 1, Samp = 3 };

constexpr Variability Join(Variability a, Variability b) { return Variability(uint8_t(a) | uint8_t(b)); }

// Infer per-node variability to a fixed point across recursive groups.
std::vector<Variability> InferVariability(const Signals &);

// Start recursive bounds at [0, 0] and widen each changing side to infinity.
std::vector<Interval> InferIntervals(const Signals &);

} // namespace faustlens
