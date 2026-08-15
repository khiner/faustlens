#include "property/Retentive.h"

#include "syntax/Edit.h"

#include <algorithm>
#include <span>

namespace faustlens::test {

Loaded::Loaded(const CorpusFile &f) : R(Parse(Terms, f.Text)) {
    Ctx.emplace(Terms, f.Text, R.Refs, R.Tokens);
    Parent.assign(R.Refs.Refs.size(), NoRef);
    for (RefId i = 0; i < R.Refs.Refs.size(); ++i)
        for (const RefId c : R.Refs.Children(i)) Parent[c] = i;
}

bool Loaded::AcceptsExpression(RefId i) const {
    if (!IsExpression(Terms.KindOf(R.Refs.Refs[i].ValueId))) return false;
    const RefId p = Parent[i];
    return p == NoRef || Terms.KindOf(R.Refs.Refs[p].ValueId) != Kind::Waveform;
}

const char *BadScript(const EditScript &script, const TermRef &target) {
    uint32_t at = target.OuterBegin;
    for (const Replacement &rep : script) {
        if (rep.Begin < at) return "overlapping or out of order";
        if (rep.End < rep.Begin) return "inverted range";
        if (rep.End > target.OuterEnd) return "escapes the target span";
        at = rep.End;
    }
    return nullptr;
}

bool BytesSurvive(std::string_view src, uint32_t begin, uint32_t end, const EditScript &script) {
    bool clobbered = false;
    for (const Replacement &rep : script) {
        if (rep.Begin >= end) break; // disjoint and in source order
        if (rep.End <= begin) continue;
        clobbered = true;
        break;
    }
    if (!clobbered) return true;
    const std::string_view bytes = src.substr(begin, end - begin);
    if (bytes.empty()) return true;
    for (const Replacement &rep : script)
        if (rep.Text.find(bytes) != std::string::npos) return true;
    return false;
}

bool Survives(const Loaded &l, std::string_view src, const TermRef &d, const TermRef &target, const EditScript &script) {
    if (BytesSurvive(src, d.SpanBegin, d.SpanEnd, script)) return true;
    const std::vector<RefId> *twins = l.Ctx->Claims(d.ValueId);
    if (twins == nullptr) return false;
    for (const RefId r : *twins) { // sorted by `OuterBegin`: refs are pre-order
        const TermRef &o = l.R.Refs.Refs[r];
        if (o.OuterBegin < target.OuterBegin) continue;
        if (o.OuterBegin > target.OuterEnd) break;
        if (o.OuterEnd > target.OuterEnd) continue;
        if (BytesSurvive(src, o.SpanBegin, o.SpanEnd, script)) return true;
    }
    return false;
}

RefId LostBytes(const Loaded &l, std::string_view src, RefId target, std::span<const uint32_t> path, RefId dropped, const EditScript &script) {
    const TermRef &t0 = l.R.Refs.Refs[target];
    struct Frame {
        RefId Ref;
        uint32_t Depth;
        bool OnPath;
    };
    std::vector<Frame> stack{{target, 0, true}};
    while (!stack.empty()) {
        const Frame fr = stack.back();
        stack.pop_back();
        if (fr.Ref == dropped) continue; // removed outright, so nobody owes its bytes
        if (!fr.OnPath && !Survives(l, src, l.R.Refs.Refs[fr.Ref], t0, script)) return fr.Ref;
        const auto kids = l.R.Refs.Children(fr.Ref);
        for (uint32_t i = 0; i < kids.size(); ++i) stack.push_back({kids[i], fr.Depth + 1, fr.OnPath && fr.Depth < path.size() && i == path[fr.Depth]});
    }
    return NoRef;
}

bool Skipped(const CorpusFile &f, Sweep &sw) {
    if (!IsGeneratedOutlier(f.Relative)) return false;
    sw.Skipped = true;
    return true;
}

bool CommentsSalvaged(const Loaded &l, const CorpusFile &f, const EditScript &script, size_t &seen) {
    bool kept = true;
    for (const Replacement &rep : script) {
        const auto first = std::lower_bound(l.R.Tokens.begin(), l.R.Tokens.end(), rep.Begin, [](const Token &tok, uint32_t at) { return tok.End <= at; });
        for (auto it = first; it != l.R.Tokens.end() && it->Begin < rep.End; ++it) {
            if (!IsComment(it->Kind)) continue;
            ++seen;
            if (rep.Text.find(f.Text.substr(it->Begin, it->End - it->Begin)) == std::string::npos) kept = false;
        }
    }
    return kept;
}

Merged Merge(std::span<const Sweep> per_file) {
    Merged m;
    for (const Sweep &s : per_file) {
        m.Failures.insert(m.Failures.end(), s.Failures.begin(), s.Failures.end());
        m.A += s.A;
        m.B += s.B;
        m.Skipped += s.Skipped;
    }
    return m;
}

} // namespace faustlens::test
