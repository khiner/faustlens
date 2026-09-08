#pragma once

#include "syntax/Prec.h"
#include "syntax/Term.h"

#include <string>
#include <string_view>

namespace faustlens {

struct Ctx {
    uint8_t MinPrec = 0;
    Level Level = Level::Expression;
    uint32_t Indent = 0;
    StrId Name = 0; // parent definition name for Clause printing
};

struct Sink {
    virtual ~Sink() = default;
    virtual void Print(std::string_view) = 0;
    // Visit each node in preorder.
    virtual void Enter(ValueId, const Ctx &) {}
    virtual void Leave() {}
    // Return whether retained grouping permits top-level printing inside the node.
    virtual bool AlreadyGrouped(ValueId) { return false; }
    // Return true when the sink emits the complete node.
    virtual bool Retain(ValueId, bool /*needs_parens*/) { return false; }
};

bool NeedsParens(const Terms &, ValueId, const Ctx &);
void Render(const Terms &, ValueId, const Ctx &, Sink &);

std::string PrintTerm(const Terms &, ValueId, const Ctx & = {});

} // namespace faustlens
