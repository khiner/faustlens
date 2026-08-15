// Sum-of-products normal form with repeated gcd factoring. It reassociates float
// addition, so everything here is ordered by `SigId`.
#pragma once

#include "signal/Signal.h"

namespace faustlens {

// Add-normal form of one node, into the same arena. Children must be normalized.
SigId NormalizeAddTerm(Signals &, SigId);

} // namespace faustlens
