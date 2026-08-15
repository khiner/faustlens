// The UI tree, built from propagation's paths rather than from box structure.
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

// `Label` is cleaned of metadata, `Raw` is the source text and orders the tree.
struct UiNode {
    bool IsGroup = false;
    uint8_t Orient = 0; // group: 0 vertical, 1 horizontal, 2 tab
    UiKind Kind = UiKind::Button;
    std::string Label, Raw;
    // Every key, acted on here or not. A key with no value maps to the empty string.
    std::map<std::string, std::set<std::string>> Meta;
    double Init = 0, Min = 0, Max = 0, Step = 0;
    uint32_t WidgetLabel = 0; // the interned path, this widget's identity
    std::vector<UiNode> Children;
};

// Leaves in emission order. `visit(n)` false stops, and the result says if it finished.
template<class Visit> bool ForEachWidget(const UiNode &n, Visit visit) {
    if (!n.IsGroup) return visit(n);
    for (const UiNode &c : n.Children)
        if (!ForEachWidget(c, visit)) return false;
    return true;
}

// Group contents sorted by raw label, groups and widgets in one order. `keep` counts
// widget nodes per path in the *compiled* graph, so a folded-away widget has no entry
// and two sharing a label stay two.
UiNode BuildUiTree(std::span<const UiItem>, std::string_view root_name, const std::map<uint32_t, int> &keep);

// The last `declare name` entry, one pair of surrounding quotes stripped.
std::string RootLabel(const MetaSet &);

// Widget nodes reachable from `roots`, per interned label path.
std::map<uint32_t, int> KeepCounts(const Signals &, std::span<const SigId> roots);

// Two inputs on one path is an error, an input plus a bargraph too, two bargraphs only
// a warning: two meters read one value, where a shared input's writes go elsewhere.
std::vector<Diagnostic> CheckPaths(const UiNode &);

} // namespace faustlens
