#include "syntax/Edit.h"

#include "syntax/Lexer.h"
#include "syntax/Prec.h"

#include <charconv>
#include <span>
#include <string>

namespace faustlens {
namespace {

// Inline fields trim surrounding whitespace but never accept comments or lexer errors.
std::string_view Trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n\v\f");
    if (begin == std::string_view::npos) return {};
    return text.substr(begin, text.find_last_not_of(" \t\r\n\v\f") - begin + 1);
}

Tok SoleToken(std::string_view text) {
    const LexResult lex = Lex(text);
    if (!lex.Diags.empty() || lex.Tokens.size() != 2) return Tok::Eof;
    const Token &t = lex.Tokens.front();
    return t.Begin == 0 && t.End == text.size() && !IsTrivia(t.Kind) ? t.Kind : Tok::Eof;
}

// A signed literal is one leaf only inside a `waveform`. Elsewhere `-` is a `BinOp`.
Tok SoleNumber(std::string_view text, bool signed_ok) {
    if (signed_ok && (text.starts_with('-') || text.starts_with('+'))) text.remove_prefix(1);
    const Tok t = SoleToken(text);
    return t == Tok::Int || t == Tok::Float ? t : Tok::Eof;
}

// What an expression position rejects.
constexpr uint64_t StatementKinds = KindMask({
    Kind::Program,
    Kind::Import,
    Kind::Declare,
    Kind::DeclareDef,
    Kind::Definition,
    Kind::Clause,
    Kind::RecDef,
    Kind::Rule,
    Kind::Modulator,
    Kind::Str,
    Kind::Hole,
    Kind::MdocBlock,
    Kind::MdocProse,
    Kind::MdocEquation,
    Kind::MdocDiagram,
    Kind::MdocMetadata,
    Kind::MdocListing,
    Kind::MdocNotice,
});

// The value of an `Int` leaf in plain decimal. Any other spelling answers false.
bool DecimalInt(const Terms &t, ValueId v, uint32_t *out) {
    if (t.KindOf(v) != Kind::Int) return false;
    const std::string_view s = t.Lexeme(v);
    return std::from_chars(s.data(), s.data() + s.size(), *out).ec == std::errc();
}

// Every `Par` below `v`, not just the right spine a comma list forms.
void FlattenPar(const Terms &t, ValueId v, std::vector<ValueId> &out) {
    if (t.KindOf(v) != Kind::Par) {
        out.push_back(v);
        return;
    }
    FlattenPar(t, t.Child(v, 0), out);
    FlattenPar(t, t.Child(v, 1), out);
}

struct LinkedTerm {
    ValueId Value;
    std::vector<SourceLink> Links;
};

LinkedTerm Original(const EditContext &ed, RefId r) { return {ed.ValueOf(r), {{{}, r}}}; }

LinkedTerm Join(EditContext &ed, Kind kind, uint8_t form, LinkedTerm a, LinkedTerm b) {
    const ValueId v = ed.Terms.Make(kind, form, 0, 0, {a.Value, b.Value});
    if (a.Links.size() == 1 && a.Links[0].Path.empty() && b.Links.size() == 1 && b.Links[0].Path.empty()) {
        const RefId parent = ed.Parent(a.Links[0].Source);
        if (parent != NoRef && ed.ValueOf(parent) == v) {
            const auto kids = ed.Refs.Children(parent);
            if (kids.size() == 2 && kids[0] == a.Links[0].Source && kids[1] == b.Links[0].Source) return Original(ed, parent);
        }
    }
    for (auto &link : a.Links) link.Path.insert(link.Path.begin(), 0);
    for (auto &link : b.Links) {
        link.Path.insert(link.Path.begin(), 1);
        a.Links.push_back(std::move(link));
    }
    return {v, std::move(a.Links)};
}

LinkedTerm FoldLinked(EditContext &ed, Kind kind, uint8_t form, std::vector<LinkedTerm> terms) {
    if (RowOf(kind).Assoc == Assoc::Right) {
        LinkedTerm out = std::move(terms.back());
        for (size_t i = terms.size() - 1; i-- > 0;) out = Join(ed, kind, form, std::move(terms[i]), std::move(out));
        return out;
    }
    LinkedTerm out = std::move(terms.front());
    for (size_t i = 1; i < terms.size(); ++i) out = Join(ed, kind, form, std::move(out), std::move(terms[i]));
    return out;
}

Edit Replacement(RefId target, LinkedTerm term) { return {target, term.Value, nullptr, std::move(term.Links)}; }

} // namespace

bool IsComposition(Kind k) { return k >= Kind::Seq && k <= Kind::RecComp; }

bool IsExpression(Kind k) { return ((StatementKinds >> int(k)) & 1) == 0; }

EditContext::EditContext(faustlens::Terms &t, const RefTree &tree) : Terms(t), Refs(tree) {
    ParentOf.assign(Refs.Refs.size(), NoRef);
    for (RefId r = 0; r < Refs.Refs.size(); ++r)
        for (const RefId c : Refs.Children(r)) ParentOf[c] = r;
}

Edit EditContext::Compose(RefId sel, Kind comp, uint8_t form, Side side, ValueId stage) {
    if (!IsComposition(comp)) return {NoRef, NoTerm, "not a composition"};
    if (sel >= Refs.Refs.size()) return {NoRef, NoTerm, "nothing is selected"};
    if (!IsExpression(KindAt(sel))) return {NoRef, NoTerm, "not an expression"};
    const RefId up = Parent(sel);
    if (up != NoRef && KindAt(up) == Kind::Waveform) return {NoRef, NoTerm, "a waveform holds numbers, not stages"};
    if (stage == NoTerm) stage = Terms.MakePrim(Prim::Wire);

    if (up != NoRef && KindAt(up) == comp && Terms.Get(ValueOf(up)).Form == form) {
        const auto kids = Refs.Children(up);
        if (kids.size() == 2) {
            const bool sel_is_left = kids[0] == sel;
            if (sel_is_left == (RowOf(comp).Assoc == Assoc::Right)) {
                std::vector<LinkedTerm> chain{Original(*this, kids[0]), Original(*this, kids[1])};
                const size_t at = (sel_is_left ? 0 : 1) + (side == Side::After ? 1 : 0);
                chain.insert(chain.begin() + long(at), LinkedTerm{stage, {}});
                return Replacement(up, FoldLinked(*this, comp, form, std::move(chain)));
            }
        }
    }
    LinkedTerm a = Original(*this, sel), b{stage, {}};
    if (side == Side::Before) std::swap(a, b);
    return Replacement(sel, Join(*this, comp, form, std::move(a), std::move(b)));
}

Edit EditContext::Delete(RefId sel) {
    if (sel >= Refs.Refs.size()) return {NoRef, NoTerm, "nothing is selected"};
    const RefId up = Parent(sel);
    if (up == NoRef) return {NoRef, NoTerm, "nothing holds this stage"};
    if (!IsComposition(KindAt(up))) return {NoRef, NoTerm, "only a stage of a composition can be removed"};
    const auto kids = Refs.Children(up);
    if (kids.size() != 2) return {NoRef, NoTerm, "only a stage of a composition can be removed"};
    return Replacement(up, Original(*this, kids[0] == sel ? kids[1] : kids[0]));
}

Edit EditContext::Retext(RefId sel, std::string_view text) {
    text = Trim(text);
    if (sel >= Refs.Refs.size()) return {NoRef, NoTerm, "nothing is selected"};
    const ValueId self = ValueOf(sel);
    const Kind kind = Terms.KindOf(self);
    if (kind == Kind::Int || kind == Kind::Real) {
        const RefId up = Parent(sel);
        const bool signed_ok = up != NoRef && KindAt(up) == Kind::Waveform;
        const Tok tok = SoleNumber(text, signed_ok);
        if (tok == Tok::Eof) return {NoRef, NoTerm, "not a number"};
        return {sel, Terms.MakeLeaf(tok == Tok::Int ? Kind::Int : Kind::Real, Terms.InternStr(text))};
    }
    if (IsLabelled(kind)) {
        if (SoleToken(text) != Tok::String) return {NoRef, NoTerm, "a label is a quoted string"};
        // Copied out first: the intern below can grow the table and dangle the span.
        const TermValue node = Terms.Get(self);
        const auto span = Terms.Children(self);
        const std::vector<ValueId> kids(span.begin(), span.end());
        Edit edit{sel, Terms.Make(kind, node.Form, node.Variants, Terms.InternStr(text), kids)};
        const auto refs = Refs.Children(sel);
        for (uint32_t i = 0; i < refs.size(); ++i) edit.Links.push_back({{i}, refs[i]});
        return edit;
    }
    return {NoRef, NoTerm, "this node has no text of its own"};
}

std::vector<ValueId> EditContext::Entries(RefId route) const {
    std::vector<ValueId> out;
    if (route >= Refs.Refs.size() || KindAt(route) != Kind::Route) return out;
    const ValueId self = ValueOf(route);
    if (Terms.Children(self).size() > 2) FlattenPar(Terms, Terms.Child(self, 2), out);
    return out;
}

std::vector<RefId> EditContext::EntryRefs(RefId route) const {
    std::vector<RefId> refs;
    const auto walk = [&](auto &&self, RefId r) -> void {
        if (KindAt(r) != Kind::Par) refs.push_back(r);
        else
            for (const RefId child : Refs.Children(r)) self(self, child);
    };
    const auto kids = Refs.Children(route);
    if (kids.size() > 2) walk(walk, kids[2]);
    return refs;
}

Edit EditContext::Rewire(RefId route, uint32_t in, uint32_t out, bool connect) {
    if (route >= Refs.Refs.size() || KindAt(route) != Kind::Route) return {NoRef, NoTerm, "not a route"};
    const auto entries = EntryRefs(route);
    if (entries.size() % 2 != 0) return {NoRef, NoTerm, "its entries do not pair up"};
    size_t at = 0;
    for (; at < entries.size(); at += 2) {
        uint32_t a = 0, b = 0;
        if (DecimalInt(Terms, ValueOf(entries[at]), &a) && DecimalInt(Terms, ValueOf(entries[at + 1]), &b) && a == in && b == out) break;
    }
    if (connect && at != entries.size()) return {route, ValueOf(route)};
    if (!connect && at == entries.size()) return {NoRef, NoTerm, "no such connection"};

    std::vector<LinkedTerm> chain;
    for (size_t i = 0; i < entries.size(); ++i)
        if (connect || i < at || i >= at + 2) chain.push_back(Original(*this, entries[i]));
    if (connect)
        for (const uint32_t channel : {in, out}) chain.push_back({Terms.MakeLeaf(Kind::Int, Terms.InternStr(std::to_string(channel))), {}});

    const auto refs = Refs.Children(route);
    std::vector<ValueId> kids{ValueOf(refs[0]), ValueOf(refs[1])};
    std::vector<SourceLink> links{{{0}, refs[0]}, {{1}, refs[1]}};
    // No entries is `route(n,m)`, distinct from an empty list.
    if (!chain.empty()) {
        auto folded = FoldLinked(*this, Kind::Par, 0, std::move(chain));
        kids.push_back(folded.Value);
        for (auto &link : folded.Links) {
            link.Path.insert(link.Path.begin(), 2);
            links.push_back(std::move(link));
        }
    }
    return {route, Terms.Make(Kind::Route, kids), nullptr, std::move(links)};
}

Wiring RouteWiring(const Terms &t, ValueId v) {
    Wiring w;
    if (t.KindOf(v) != Kind::Route) return w;
    const std::span<const ValueId> kids = t.Children(v);
    if (kids.size() < 2) return w;
    if (!DecimalInt(t, kids[0], &w.Ins) || !DecimalInt(t, kids[1], &w.Outs)) return {};
    if (kids.size() < 3) return w;

    std::vector<ValueId> entries;
    FlattenPar(t, kids[2], entries);
    // The rewires refuse an odd list, so drawing half would offer a drag always declined.
    if (entries.size() % 2 != 0) return w;
    for (size_t i = 0; i + 1 < entries.size(); i += 2) {
        uint32_t a = 0, b = 0;
        if (!DecimalInt(t, entries[i], &a) || !DecimalInt(t, entries[i + 1], &b)) continue;
        // An out-of-range pair is legal and routes nothing, so do not draw it.
        if (a == 0 || b == 0 || a > w.Ins || b > w.Outs) continue;
        w.Pairs.emplace_back(a, b);
    }
    return w;
}

} // namespace faustlens
