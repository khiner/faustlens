// Type analysis: variability and interval. Nature lives in `promote.h`.
#pragma once

#include "signal/Interval.h"
#include "signal/Signal.h"

#include <cstdint>
#include <vector>

namespace faustlens {

// The reference's values: the gap at 2 is what makes `|` the lattice join.
enum class Variability : uint8_t { Konst = 0, Block = 1, Samp = 3 };

constexpr Variability Join(Variability a, Variability b) { return Variability(uint8_t(a) | uint8_t(b)); }

// Per node id. A fixpoint, since group ids precede their branches, and it only widens.
std::vector<Variability> InferVariability(const Signals &);

// Per node id. A widening climb: a branch starts at `[0, 0]` and jumps to `±inf` on first move.
std::vector<Interval> InferIntervals(const Signals &);

} // namespace faustlens
