#include "query/Snapshot.h"

#include <algorithm>

namespace faustlens {

const FileView *Snapshot::File(std::string_view path) const {
    for (const FileView &f : Files)
        if (f.Path == path) return &f;
    return nullptr;
}

std::vector<Span> Marks(const RefTree &refs, ValueId subject) {
    std::vector<Span> out;
    if (subject == NoTerm) return out;
    for (const TermRef &r : refs.Refs)
        if (r.ValueId == subject) out.push_back({r.SpanBegin, r.SpanEnd});
    return out;
}

std::vector<Span> Marks(const FileView &f, ValueId subject) { return Marks(f.Refs, subject); }

std::vector<Span> Marks(const FileView &f, const Diagnostic &d) {
    if (d.Subject == NoValue) {
        std::vector<Span> out;
        if (d.End > d.Begin) out.push_back({d.Begin, d.End});
        return out;
    }
    return Marks(f, d.Subject);
}

RefId Innermost(const FileView &f, uint32_t offset) { return f.Refs.Innermost(offset); }

ValueId ValueAt(const FileView &f, uint32_t offset) {
    const RefId r = Innermost(f, offset);
    return r == NoRef ? NoTerm : f.Refs.Refs[r].ValueId;
}

std::vector<ValueId> ValuesAt(const FileView &f, uint32_t offset) {
    std::vector<ValueId> out;
    for (const RefId r : f.Refs.Chain(offset)) out.push_back(f.Refs.Refs[r].ValueId);
    return out;
}

std::optional<uint32_t> OffsetOfRef(const FileView &f, RefId i) {
    if (i >= f.Refs.Refs.size()) return std::nullopt;
    const TermRef &r = f.Refs.Refs[i];
    // The first byte no child covers. Children are in source order and disjoint,
    // so one sweep finds it.
    uint32_t at = r.OuterBegin;
    for (const RefId c : f.Refs.Children(i)) {
        const TermRef &kid = f.Refs.Refs[c];
        if (at < kid.OuterBegin) return at;
        at = std::max(at, kid.OuterEnd);
    }
    if (at < r.OuterEnd) return at;
    // Children tile the span exactly. Unreachable for a leaf.
    return r.OuterBegin;
}

std::optional<uint32_t> OffsetOf(const FileView &f, ValueId v) {
    if (v == NoTerm) return std::nullopt;
    for (RefId i = 0; i < f.Refs.Refs.size(); ++i)
        if (f.Refs.Refs[i].ValueId == v) return OffsetOfRef(f, i);
    return std::nullopt;
}

ValueId ProcessBody(const Terms &t, ValueId program) {
    if (program == NoTerm) return NoTerm;
    for (const ValueId stmt : t.Children(program)) {
        if (t.KindOf(stmt) != Kind::Definition || t.Lexeme(stmt) != "process") continue;
        const auto kids = t.Children(t.Child(stmt, 0));
        return kids.empty() ? NoTerm : kids.back();
    }
    return NoTerm;
}

RefId ProcessBodyRef(const Terms &t, const FileView &f) {
    if (f.Refs.Refs.empty()) return NoRef;
    for (const RefId stmt : f.Refs.Children(f.Refs.Root())) {
        const ValueId v = f.Refs.Refs[stmt].ValueId;
        if (t.KindOf(v) != Kind::Definition || t.Lexeme(v) != "process") continue;
        const auto clauses = f.Refs.Children(stmt);
        if (clauses.empty()) return NoRef;
        const auto parts = f.Refs.Children(clauses.front());
        return parts.empty() ? NoRef : parts.back();
    }
    return NoRef;
}

Snapshot Publish(Session &s, const std::vector<std::string> &open_paths) {
    Snapshot snap;
    snap.Revision = s.Revision;
    for (const std::string &path : open_paths) {
        const TermsResult &t = s.TermsOf(path);
        FileView v;
        v.Path = path;
        v.Text = t.Text;
        v.Root = t.Root;
        v.Refs = t.Refs;
        v.Tokens = t.Tokens;
        snap.Files.push_back(std::move(v));
    }
    snap.Diags = s.Diagnostics();
    return snap;
}

} // namespace faustlens
