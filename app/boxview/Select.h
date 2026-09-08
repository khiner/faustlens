// Resolve source positions against the occurrences present in the diagram.
#pragma once

#include "boxview/Layout.h"
#include "query/Snapshot.h"

#include <cstdint>
#include <span>
#include <vector>

namespace faustlens::boxview {

// Valid only for the source revision used to resolve it.
struct Selection {
    struct Stage {
        ValueId Value;
        RefId Ref;
        bool operator==(const Stage &) const = default;
    };
    std::vector<Stage> Chain; // drawn source occurrences, innermost first
    size_t Index = 0;
    // Use the selected stage's byte offset to preserve selection across reparses.
    uint32_t Caret = 0;

    bool Empty() const { return Chain.empty(); }
    ValueId Value() const { return Index < Chain.size() ? Chain[Index].Value : NoTerm; }
};

// Preserve the exact byte offset of a text click.
Selection SelectAt(const FileView &, const Node &root, RefId body, uint32_t at);

// Select a drawn occurrence by child-index path.
Selection SelectPath(const FileView &, const Node &root, RefId body, std::span<const uint32_t> path);

void SelectOut(Selection &, const FileView &);
void SelectIn(Selection &, const FileView &);

// Return the selected ref if valid in this snapshot.
RefId SelectedRef(const FileView &, const Selection &);

// Return the selected drawn occurrence.
const Node *SelectedNode(const FileView &, const Node &root, RefId body, const Selection &);

} // namespace faustlens::boxview
