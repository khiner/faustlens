// Normalize sums and products in SigId order to preserve deterministic floating-point association.
#pragma once

#include "signal/Signal.h"

namespace faustlens {

// Normalize one node in the same arena; children must already be normalized.
SigId NormalizeAddTerm(Signals &, SigId);

} // namespace faustlens
