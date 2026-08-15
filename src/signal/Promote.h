// Faust's implicit conversions as cast nodes. Nature only, the rest in `type.h`.
#pragma once

#include "signal/Signal.h"

#include <cstdint>
#include <span>
#include <vector>

namespace faustlens {

enum class Nature : uint8_t { Int, Real };

// A fixpoint, not one sweep: a group reserves its id before its branches exist.
// Nature only widens from `Int`, so it terminates.
std::vector<Nature> InferNatures(const Signals &);

// Adds casts and nothing else, since simplifying here would re-run the rules of
// the pass meant to be last.
std::vector<SigId> Promote(Signals &, std::span<const SigId> roots);

// A table index becomes `max(0, min(i, size-1))` unless the interval proves it in range.
std::vector<SigId> ClampTables(Signals &, std::span<const SigId> roots);

} // namespace faustlens
