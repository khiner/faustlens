// Text/diagram linking. A byte names a chain of values, the diagram draws only some,
// so a click resolves against what is drawn.
#pragma once

#include "boxview/Layout.h"
#include "query/Snapshot.h"

#include <cstdint>
#include <span>
#include <vector>

namespace faustlens::boxview {

// Nothing here outlives the revision it was resolved against.
struct Selection {
    std::vector<ValueId> Chain; // innermost first, only stages the view draws
    size_t Index = 0;
    // Carries the selection across a reparse, so it must name the *selected*
    // stage. A descendant makes it drift inward.
    uint32_t Caret = 0;

    bool Empty() const { return Chain.empty(); }
    ValueId Value() const { return Index < Chain.size() ? Chain[Index] : NoTerm; }
};

// A click in the text. The caret is the reader's byte, kept exactly.
Selection SelectAt(const FileView &, const Node &root, uint32_t at);

// A click on a stage, by `Layout::HitPath`'s path. Not a value: `_` interns once.
Selection SelectPath(const FileView &, const Node &root, RefId body, std::span<const uint32_t> path);

void SelectOut(Selection &, const FileView &);
void SelectIn(Selection &, const FileView &);

// A value used twice has two refs, so the caret picks: the one on its chain whose
// value is selected, not the innermost.
RefId SelectedRef(const FileView &, const Selection &);

// The drawn occurrence, where `Value()` is only the value.
const Node *SelectedNode(const FileView &, const Node &root, RefId body, const Selection &);

} // namespace faustlens::boxview
