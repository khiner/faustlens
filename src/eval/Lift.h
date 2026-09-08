// Lift Box to Term while preserving circuit semantics.
#pragma once

#include "box/Box.h"
#include "syntax/Term.h"

namespace faustlens {

struct SlotName {
    uint32_t Slot;
    StrId Name;
};

struct Lifted {
    ValueId Term = NoTerm;
    const char *Declined = nullptr;
    BoxId At = NoBox;

    explicit operator bool() const { return Term != NoTerm; }
};

// Free slots require visible source binder names.
Lifted Lift(Terms &, const Boxes &, BoxId, std::span<const SlotName> ambient = {});

} // namespace faustlens
