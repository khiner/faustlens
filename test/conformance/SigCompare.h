// A `Signals` arena against the reference's `.sig` dump, by isomorphism: the two agree on shape
// and leaves but never on numbering.
#pragma once

#include "conformance/SigParse.h"
#include "signal/Signal.h"

#include <expected>
#include <span>
#include <string>
#include <vector>

namespace faustlens::test {

// Unexpected names the first disagreement and its path, `diverged` our node there.
std::expected<void, std::string> SigIsomorphic(const Signals &, std::span<const SigId> ours, const SigFile &theirs, SigId *diverged = nullptr);

std::string PrintSig(const Signals &, SigId, int max_depth = 3);

} // namespace faustlens::test
