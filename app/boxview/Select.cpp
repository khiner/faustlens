#include "boxview/Select.h"

#include <algorithm>

namespace faustlens::boxview {
namespace {

// Every move along the chain must re-anchor, or a reparse pulls it back.
void Anchor(Selection &s, const FileView &f) {
    const RefId r = SelectedRef(f, s);
    if (r == NoRef) return;
    if (const auto at = OffsetOfRef(f, r)) s.Caret = *at;
}

// Below the evaluated view the drawn tree and the ref tree stop corresponding.
const Node *Descend(const FileView &f, const Node &n, RefId r, RefId want) {
    if (r == want) return &n;
    const std::span<const RefId> kids = f.Refs.Children(r);
    for (size_t i = 0; i < n.Kids.size() && i < kids.size(); ++i) {
        if (n.Kids[i].Evaluated) continue;
        if (const Node *hit = Descend(f, n.Kids[i], kids[i], want)) return hit;
    }
    return nullptr;
}

} // namespace

Selection SelectAt(const FileView &f, const Node &root, RefId body, uint32_t at) {
    Selection s;
    s.Caret = at;
    if (body >= f.Refs.Refs.size() || f.Refs.Refs[body].ValueId != root.Term) return s;
    const auto &span = f.Refs.Refs[body];
    if (at < span.OuterBegin || at >= span.OuterEnd) return s;
    const Node *node = &root;
    RefId r = body;
    for (;;) {
        s.Chain.push_back({node->Term, r});
        const auto kids = f.Refs.Children(r);
        size_t i = 0;
        for (; i < node->Kids.size() && i < kids.size(); ++i) {
            const TermRef &child = f.Refs.Refs[kids[i]];
            if (!node->Kids[i].Evaluated && child.OuterBegin <= at && at < child.OuterEnd) break;
        }
        if (i >= node->Kids.size() || i >= kids.size()) break;
        r = kids[i];
        node = &node->Kids[i];
    }
    std::reverse(s.Chain.begin(), s.Chain.end());
    return s;
}

Selection SelectPath(const FileView &f, const Node &root, RefId body, std::span<const uint32_t> path) {
    Selection s;
    const Node *n = &root;
    RefId r = body;
    if (body >= f.Refs.Refs.size() || f.Refs.Refs[body].ValueId != root.Term) return s;
    s.Chain.push_back({root.Term, body});
    for (const uint32_t i : path) {
        if (i >= n->Kids.size() || n->Kids[i].Evaluated) break;
        const std::span<const RefId> kids = f.Refs.Children(r);
        if (i >= kids.size()) break;
        r = kids[i];
        n = &n->Kids[i];
        s.Chain.push_back({n->Term, r});
    }
    std::reverse(s.Chain.begin(), s.Chain.end());
    if (const auto at = OffsetOfRef(f, r)) s.Caret = *at;
    return s;
}

void SelectOut(Selection &s, const FileView &f) {
    if (s.Index + 1 >= s.Chain.size()) return;
    ++s.Index;
    Anchor(s, f);
}

void SelectIn(Selection &s, const FileView &f) {
    if (s.Index == 0) return;
    --s.Index;
    Anchor(s, f);
}

RefId SelectedRef(const FileView &f, const Selection &s) {
    if (s.Index >= s.Chain.size()) return NoRef;
    const RefId r = s.Chain[s.Index].Ref;
    if (r >= f.Refs.Refs.size() || f.Refs.Refs[r].ValueId != s.Value()) return NoRef;
    return r;
}

const Node *SelectedNode(const FileView &f, const Node &root, RefId body, const Selection &s) {
    const RefId want = SelectedRef(f, s);
    return want != NoRef && body < f.Refs.Refs.size() ? Descend(f, root, body, want) : nullptr;
}

} // namespace faustlens::boxview
