// Evaluate a subterm and lift it back into Term. Only the open set is state.
#pragma once

#include "boxview/Select.h"
#include "query/Query.h"
#include "query/Snapshot.h"
#include "syntax/Edit.h"
#include "syntax/Term.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace faustlens::app {

struct Expansion {
    ValueId Term = NoTerm;
    const char *Declined = nullptr; // a literal, so it outlives the call
    // Evaluated under several environments that disagree, so the first is shown.
    bool Ambiguous = false;

    explicit operator bool() const { return Term != NoTerm; }
};

Expansion Expand(Session &, ValueId);

// A declined node is absent from the result and draws as it always did.
std::unordered_map<ValueId, ValueId> ExpandAll(Session &, std::span<const ValueId> open);

// The evaluated subgraph lifted into Term and the source rewritten to match. Comments
// survive only by salvage.
Edit Materialize(Session &, const FileView &, const boxview::Selection &);

} // namespace faustlens::app
