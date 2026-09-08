#pragma once

#include "signal/Propagate.h"
#include "syntax/Diagnostic.h"

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace faustlens {

struct MetaSet;

// Label excludes metadata; Raw preserves text for sorting.
struct UiNode {
    bool IsGroup = false;
    uint8_t Orient = 0; // group: 0 vertical, 1 horizontal, 2 tab
    UiKind Kind = UiKind::Button;
    std::string Label, Raw;
    // Preserve all metadata, with an empty string for valueless keys.
    std::map<std::string, std::set<std::string>> Meta;
    double Init = 0, Min = 0, Max = 0, Step = 0;
    uint32_t WidgetLabel = 0;
    std::vector<UiNode> Children;
};

// Visit leaves in emission order and return false if the visitor stops.
template<class Visit> bool ForEachWidget(const UiNode &n, Visit visit) {
    if (!n.IsGroup) return visit(n);
    for (const UiNode &c : n.Children)
        if (!ForEachWidget(c, visit)) return false;
    return true;
}

// Sort group contents by raw label and retain the compiled widget count per path.
UiNode BuildUiTree(std::span<const UiItem>, std::string_view root_name, const std::map<uint32_t, int> &keep);

// Return the last declared name without surrounding quotes.
std::string RootLabel(const MetaSet &);

// Count reachable widgets by interned label path.
std::map<uint32_t, int> KeepCounts(const Signals &, std::span<const SigId> roots);

// Reject duplicate inputs and mixed input/bargraph paths; warn for duplicate bargraphs.
std::vector<Diagnostic> CheckPaths(const UiNode &);

} // namespace faustlens
