// Box back to Term, so an evaluated subgraph can be printed or edited. Partial, and the
// round trip is over circuits, not text.
#pragma once

#include "box/Box.h"
#include "syntax/Term.h"

namespace faustlens {

struct Lifted {
    ValueId Term = NoTerm;
    const char *Declined = nullptr;
    BoxId At = NoBox;

    explicit operator bool() const { return Term != NoTerm; }
};

Lifted Lift(Terms &, const Boxes &, BoxId);

} // namespace faustlens
