#include "syntax/Splice.h"

#include "syntax/Lexer.h"

#include <algorithm>
#include <format>

namespace faustlens {
namespace {

// Descent order is the ref tree's pre-order, so the k-th call belongs to ref k.
struct CtxCollector : Sink {
    const RefTree &Refs;
    std::vector<Ctx> Out;

    explicit CtxCollector(const RefTree &r) : Refs(r) {}
    void Print(std::string_view) override {}
    void Enter(ValueId, const Ctx &ctx) override { Out.push_back(ctx); }
    // A node the source parenthesized starts fresh, or an identity edit re-derives parens.
    bool AlreadyGrouped(ValueId) override {
        if (Out.empty() || Out.size() > Refs.Refs.size()) return false;
        const TermRef &t = Refs.Refs[Out.size() - 1];
        return t.OuterBegin != t.SpanBegin || t.OuterEnd != t.SpanEnd;
    }
};

struct SpliceSink : Sink {
    const SpliceContext &Sc;
    std::string_view S;
    const RefTree &Refs;
    const TermRef &Target;
    uint32_t Cursor = 0;
    std::string Pending;
    size_t Anchor = 0; // the start of `pending`'s final token
    EditScript Script;

    SpliceSink(const SpliceContext &context, RefId t)
        : Sc(context), S(context.Src), Refs(context.Refs), Target(context.Refs.Refs[t]), Cursor(Target.OuterBegin) {}

    void Print(std::string_view text) override { Append(text); }

    bool Retain(ValueId v, bool needs_parens) override {
        const RefId r = Claim(v);
        if (r == NoRef) return false;
        const TermRef &ref = Refs.Refs[r];
        // The outer span keeps the user's own parens, bytes the printer would not re-derive.
        if (ref.OuterBegin != ref.SpanBegin || ref.OuterEnd != ref.SpanEnd) {
            RetainSpan(ref.OuterBegin, ref.OuterEnd);
            return true;
        }
        if (needs_parens) {
            Append("(");
            RetainSpan(ref.SpanBegin, ref.SpanEnd);
            Append(")");
            return true;
        }
        RetainSpan(ref.SpanBegin, ref.SpanEnd);
        return true;
    }

    EditScript Finish() {
        Flush(Target.OuterEnd);
        return std::move(Script);
    }

    // The first ref for `v` in the target span at or after the cursor, else the first in
    // it. Refs are not consumed.
    RefId Claim(ValueId v) const {
        const std::vector<RefId> *list = Sc.Claims(v);
        if (list == nullptr) return NoRef;
        // Both bounds inclusive: an ancestor can share the target's start offset, and the
        // target's own ref must qualify.
        // One projection rather than two comparators whose argument orders mirror each other.
        const auto outer_begin = [this](RefId r) { return Refs.Refs[r].OuterBegin; };
        const auto begin = std::ranges::lower_bound(*list, Target.OuterBegin, {}, outer_begin);
        const auto end = std::ranges::upper_bound(begin, list->end(), Target.OuterEnd, {}, outer_begin);

        RefId first = NoRef;
        for (auto it = begin; it != end; ++it) {
            if (Refs.Refs[*it].OuterEnd > Target.OuterEnd) continue;
            if (first == NoRef) first = *it;
            if (Refs.Refs[*it].OuterBegin >= Cursor) return *it;
        }
        return first;
    }

    void RetainSpan(uint32_t begin, uint32_t end) {
        if (begin >= Cursor) { // keeps its place
            Flush(begin);
            Cursor = end;
        } else { // the node moved, or this is a second occurrence
            Append(S.substr(begin, end - begin));
        }
    }

    // `Pending` begins at `Anchor`, a token boundary, as `AppendUnfused` needs.
    void Append(std::string_view text) { AppendUnfused(Pending, Anchor, text); }

    // A seam's left context: the file's bytes back to the last token boundary.
    std::string_view FileLeftOf(uint32_t at) const {
        if (at == 0) return {};
        const TokenVector &toks = Sc.Tokens;
        const auto it = std::ranges::lower_bound(toks, at, {}, &Token::End);
        if (it == toks.end() || it->End != at) return {}; // not a token boundary
        return S.substr(it->Begin, at - it->Begin);
    }

    void Flush(uint32_t upto) {
        const std::string_view original = S.substr(Cursor, upto - Cursor);
        if (original != Pending) {
            std::string text = Salvage(Pending, Cursor, upto);
            // With an empty replacement, check the file's two sides against each other.
            if (WouldFuse(FileLeftOf(Cursor), text.empty() ? S.substr(upto) : text)) text.insert(text.begin(), ' ');
            if (!text.empty() && WouldFuse(text, S.substr(upto))) text += ' ';
            Script.push_back({Cursor, upto, std::move(text)});
        }
        Pending.clear();
        Anchor = 0;
        Cursor = upto;
    }

    // Retention skips comments between two retained spans, so re-emit them around the text.
    std::string Salvage(std::string_view text, uint32_t begin, uint32_t end) const {
        const TokenVector &tokens = Sc.Tokens;
        // The first token reaching past `begin`, which `<=` on a lower bound was spelling.
        const auto first = std::ranges::upper_bound(tokens, begin, {}, &Token::End);

        uint32_t pivot = end;
        for (auto it = first; it != tokens.end() && it->Begin < end; ++it) {
            if (IsTrivia(it->Kind)) continue;
            pivot = it->Begin;
            break;
        }
        std::string before, after;
        for (auto it = first; it != tokens.end() && it->Begin < end; ++it) {
            if (!IsComment(it->Kind)) continue;
            const std::string_view comment = S.substr(it->Begin, it->End - it->Begin);
            if (it->Begin < pivot) {
                before += ' ';
                before.append(comment);
                // A line comment swallows the rest of its line.
                if (it->Kind == Tok::LineComment) before += '\n';
            } else {
                after.append(comment);
                after += it->Kind == Tok::LineComment ? '\n' : ' ';
            }
        }
        if (before.empty() && after.empty()) return std::string(text);
        return std::format("{}{}{}", before, text, after);
    }
};

} // namespace

std::string ApplyScript(std::string_view src, const EditScript &script) {
    std::string out;
    uint32_t at = 0;
    for (const Replacement &r : script) {
        out.append(src.substr(at, r.Begin - at));
        out.append(r.Text);
        at = r.End;
    }
    out.append(src.substr(at));
    return out;
}

SpliceContext::SpliceContext(const faustlens::Terms &t, std::string_view text, const RefTree &tree, const TokenVector &toks)
    : Terms(t), Src(text), Refs(tree), Tokens(toks) {
    if (!Refs.Refs.empty()) {
        CtxCollector collector(Refs);
        Render(Terms, Refs.Refs[Refs.Root()].ValueId, Ctx{}, collector);
        Ctxs = std::move(collector.Out);
    }
    LineStarts.push_back(0);
    for (uint32_t i = 0; i < Src.size(); ++i)
        if (Src[i] == '\n') LineStarts.push_back(i + 1);
    for (RefId r = 0; r < Refs.Refs.size(); ++r) ByValue[Refs.Refs[r].ValueId].push_back(r);
}

const std::vector<RefId> *SpliceContext::Claims(ValueId v) const {
    const auto it = ByValue.find(v);
    return it == ByValue.end() ? nullptr : &it->second;
}

Ctx SpliceContext::At(RefId target) const {
    Ctx ctx = target < Ctxs.size() ? Ctxs[target] : Ctx{};
    const uint32_t at = Refs.Refs[target].OuterBegin;
    // A backward scan would go quadratic on a file with very long lines.
    const auto line = std::upper_bound(LineStarts.begin(), LineStarts.end(), at) - 1;
    ctx.Indent = at - *line;
    return ctx;
}

EditScript SpliceContext::Splice(RefId target, ValueId new_root, const Ctx &ctx0) const {
    SpliceSink sink(*this, target);
    Render(Terms, new_root, ctx0, sink);
    return sink.Finish();
}

EditScript SpliceContext::Splice(RefId target, ValueId new_root) const { return Splice(target, new_root, At(target)); }

} // namespace faustlens
