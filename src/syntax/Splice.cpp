#include "syntax/Splice.h"

#include "syntax/Lexer.h"

#include <algorithm>
#include <format>

namespace faustlens {
namespace {

// Match printer traversal to refs in preorder.
struct CtxCollector : Sink {
    const RefTree &Refs;
    std::vector<Ctx> Out;

    explicit CtxCollector(const RefTree &r) : Refs(r) {}
    void Print(std::string_view) override {}
    void Enter(ValueId, const Ctx &ctx) override { Out.push_back(ctx); }
    // Use top-level precedence inside retained parentheses to preserve identity splices.
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
    struct Piece {
        uint32_t Begin = 0, End = 0;
        std::string Text;
        bool Retained = false;
    };
    std::span<const SourceLink> Links;
    bool Explicit = false;
    std::vector<uint32_t> Path, NextChild;
    Ctx Destination;
    std::vector<Piece> Pieces;
    std::vector<std::pair<uint32_t, uint32_t>> Retained;
    uint32_t ClaimCursor = 0, Cursor = 0;
    std::string Pending;
    size_t Anchor = 0;
    EditScript Script;

    SpliceSink(const SpliceContext &context, RefId t, std::span<const SourceLink> links = {}, bool explicit_links = false)
        : Sc(context), S(context.Src), Refs(context.Refs), Target(context.Refs.Refs[t]), Links(links), Explicit(explicit_links), ClaimCursor(Target.OuterBegin),
          Cursor(Target.OuterBegin) {}

    void Enter(ValueId, const Ctx &ctx) override {
        if (!NextChild.empty()) Path.push_back(NextChild.back()++);
        NextChild.push_back(0);
        Destination = ctx;
    }
    void Leave() override {
        NextChild.pop_back();
        if (!Path.empty()) Path.pop_back();
    }
    void Print(std::string_view text) override { Pieces.push_back({0, 0, std::string(text), false}); }

    void Keep(uint32_t begin, uint32_t end) {
        Pieces.push_back({begin, end, {}, true});
        Retained.emplace_back(begin, end);
        ClaimCursor = std::max(ClaimCursor, end);
    }

    bool Retain(ValueId v, bool needs_parens) override {
        const RefId r = Claim(v);
        if (r == NoRef) return false;
        const TermRef &ref = Refs.Refs[r];
        if (ref.OuterBegin != ref.SpanBegin || ref.OuterEnd != ref.SpanEnd) {
            Keep(ref.OuterBegin, ref.OuterEnd);
            return true;
        }
        // Wrap retained expressions used as arguments when commas occur below their root.
        const bool grammar_changes = Destination.Level == Level::Argument && Sc.Ctxs[r].Level == Level::Expression && RowOf(Sc.Terms, v).Level != 0;
        if (needs_parens || grammar_changes) Print("(");
        Keep(ref.SpanBegin, ref.SpanEnd);
        if (needs_parens || grammar_changes) Print(")");
        return true;
    }

    EditScript Finish() {
        std::ranges::sort(Retained);
        std::vector<std::pair<uint32_t, uint32_t>> regions;
        for (const auto &[begin, end] : Retained) {
            if (regions.empty() || begin > regions.back().second) regions.emplace_back(begin, end);
            else regions.back().second = std::max(regions.back().second, end);
        }
        Retained = std::move(regions);
        for (const Piece &piece : Pieces) {
            if (piece.Retained) RetainSpan(piece.Begin, piece.End);
            else Append(piece.Text);
        }
        Flush(Target.OuterEnd);
        return std::move(Script);
    }

    // Return the first matching ref at or after the cursor, or the first within the target.
    RefId Claim(ValueId v) const {
        if (Explicit) {
            for (const SourceLink &link : Links) {
                if (link.Path != Path || link.Source >= Refs.Refs.size()) continue;
                const TermRef &ref = Refs.Refs[link.Source];
                if (ref.ValueId == v && ref.OuterBegin >= Target.OuterBegin && ref.OuterEnd <= Target.OuterEnd) return link.Source;
            }
            return NoRef;
        }
        const std::vector<RefId> *list = Sc.Claims(v);
        if (list == nullptr) return NoRef;
        // Include both bounds so ancestors sharing the target start and the target itself qualify.
        const auto outer_begin = [this](RefId r) { return Refs.Refs[r].OuterBegin; };
        const auto begin = std::ranges::lower_bound(*list, Target.OuterBegin, {}, outer_begin);
        const auto end = std::ranges::upper_bound(begin, list->end(), Target.OuterEnd, {}, outer_begin);

        RefId first = NoRef;
        for (auto it = begin; it != end; ++it) {
            if (Refs.Refs[*it].OuterEnd > Target.OuterEnd) continue;
            if (first == NoRef) first = *it;
            if (Refs.Refs[*it].OuterBegin >= ClaimCursor) return *it;
        }
        return first;
    }

    void RetainSpan(uint32_t begin, uint32_t end) {
        if (begin >= Cursor) {
            Flush(begin);
            Cursor = end;
        } else {
            Append(S.substr(begin, end - begin));
        }
    }

    void Append(std::string_view text) { AppendUnfused(Pending, Anchor, text); }

    // Include left context from the final source token boundary.
    std::string_view FileLeftOf(uint32_t at) const {
        if (at == 0) return {};
        const TokenVector &toks = Sc.Tokens;
        const auto it = std::ranges::lower_bound(toks, at, {}, &Token::End);
        if (it == toks.end() || it->End != at) return {};
        return S.substr(it->Begin, at - it->Begin);
    }

    void Flush(uint32_t upto) {
        const std::string_view original = S.substr(Cursor, upto - Cursor);
        if (original != Pending) {
            std::string text = Salvage(Pending, Cursor, upto);
            // Check newly adjacent source tokens after an empty replacement.
            if (WouldFuse(FileLeftOf(Cursor), text.empty() ? S.substr(upto) : text)) text.insert(text.begin(), ' ');
            if (!text.empty() && WouldFuse(text, S.substr(upto))) text += ' ';
            Script.push_back({Cursor, upto, std::move(text)});
        }
        Pending.clear();
        Anchor = 0;
        Cursor = upto;
    }

    // Preserve comments in gaps between retained spans.
    std::string Salvage(std::string_view text, uint32_t begin, uint32_t end) const {
        const TokenVector &tokens = Sc.Tokens;
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
            // Exclude every retained region from comment salvage, including regions emitted later.
            const auto kept = std::ranges::upper_bound(Retained, it->Begin, {}, &std::pair<uint32_t, uint32_t>::first);
            if (kept != Retained.begin() && std::prev(kept)->second >= it->End) continue;
            const std::string_view comment = S.substr(it->Begin, it->End - it->Begin);
            if (it->Begin < pivot) {
                before += ' ';
                before.append(comment);
                // Preserve a newline after a line comment.
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
    // Scan forward to keep long-line processing linear.
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

EditScript SpliceContext::Splice(const Edit &edit) const {
    if (edit.Target >= Refs.Refs.size()) return {};
    const auto unchanged = [&](const SourceLink &link) {
        RefId r = edit.Target;
        for (const uint32_t index : link.Path) {
            const auto kids = Refs.Children(r);
            if (index >= kids.size()) return false;
            r = kids[index];
        }
        return r == link.Source;
    };
    if (edit.Value == Refs.Refs[edit.Target].ValueId && std::ranges::all_of(edit.Links, unchanged)) return {};
    SpliceSink sink(*this, edit.Target, edit.Links, true);
    Render(Terms, edit.Value, At(edit.Target), sink);
    return sink.Finish();
}

} // namespace faustlens
