#include "boxview/Select.h"

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

Selection SelectAt(const FileView &f, const Node &root, uint32_t at) {
    Selection s;
    s.Caret = at;
    // Interning means this finds *an* occurrence, not necessarily this byte's.
    for (const ValueId v : ValuesAt(f, at)) {
        const std::vector<ValueId> path = Layout::PathTo(root, v);
        if (path.empty()) continue;
        s.Chain.assign(path.rbegin(), path.rend());
        break;
    }
    return s;
}

Selection SelectPath(const FileView &f, const Node &root, RefId body, std::span<const uint32_t> path) {
    Selection s;
    const Node *n = &root;
    RefId r = body;
    std::vector<ValueId> down{root.Term};
    for (const uint32_t i : path) {
        if (i >= n->Kids.size() || n->Kids[i].Evaluated) break;
        const std::span<const RefId> kids = f.Refs.Children(r);
        if (i >= kids.size()) break;
        r = kids[i];
        n = &n->Kids[i];
        down.push_back(n->Term);
    }
    s.Chain.assign(down.rbegin(), down.rend());
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
    const ValueId v = s.Value();
    if (v == NoTerm) return NoRef;
    // Innermost first, so the first match is the occurrence around the caret.
    for (const RefId r : f.Refs.Chain(s.Caret))
        if (f.Refs.Refs[r].ValueId == v) return r;
    return NoRef;
}

const Node *SelectedNode(const FileView &f, const Node &root, RefId body, const Selection &s) {
    const ValueId v = s.Value();
    if (v == NoTerm) return nullptr;
    const RefId want = SelectedRef(f, s);
    if (want != NoRef && body != NoRef)
        if (const Node *n = Descend(f, root, body, want)) return n;
    return Layout::Find(root, v);
}

} // namespace faustlens::boxview
