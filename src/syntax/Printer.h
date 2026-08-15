// `Sink` intercepts nodes mid-descent, so a retentive printer cannot reorder.
#pragma once

#include "syntax/Prec.h"
#include "syntax/Term.h"

#include <string>
#include <string_view>

namespace faustlens {

struct Ctx {
    // Lowest precedence a child may have here without parentheses.
    uint8_t MinPrec = 0;
    Level Level = Level::Expression;
    uint32_t Indent = 0;
    StrId Name = 0; // a `Clause` prints its parent `Definition`'s name
};

struct Sink {
    virtual ~Sink() = default;
    virtual void Print(std::string_view) = 0;
    // Every node, in pre-order.
    virtual void Enter(ValueId, const Ctx &) {}
    // True where the node already carries parens, so descent restarts at top level.
    virtual bool AlreadyGrouped(ValueId) { return false; }
    // True means the sink emitted the node itself and descent stops there.
    virtual bool Retain(ValueId, bool /*needs_parens*/) { return false; }
};

bool NeedsParens(const Terms &, ValueId, const Ctx &);
void Render(const Terms &, ValueId, const Ctx &, Sink &);

std::string PrintTerm(const Terms &, ValueId, const Ctx & = {});

} // namespace faustlens
