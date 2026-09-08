#pragma once

#include "signal/Signal.h"

#include <cstdint>
#include <span>
#include <vector>

namespace faustlens {

enum class Nature : uint8_t { Int, Real };

// Infer nature to a fixed point because recursive group ids precede their branches.
std::vector<Nature> InferNatures(const Signals &);

// Add implicit cast nodes while preserving the simplification pass order.
std::vector<SigId> Promote(Signals &, std::span<const SigId> roots);

// Clamp table indices to [0, size-1] unless intervals prove them in range.
std::vector<SigId> ClampTables(Signals &, std::span<const SigId> roots);

} // namespace faustlens
