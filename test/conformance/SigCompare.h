// Compare signal graph structure and leaves across independent node numbering.
#pragma once

#include "conformance/SigParse.h"
#include "signal/Signal.h"

#include <expected>
#include <span>
#include <string>
#include <vector>

namespace faustlens::test {

// Return the first mismatched path and set diverged to its graph node.
std::expected<void, std::string> SigIsomorphic(const Signals &, std::span<const SigId> ours, const SigFile &theirs, SigId *diverged = nullptr);

std::string PrintSig(const Signals &, SigId, int max_depth = 3);

} // namespace faustlens::test
