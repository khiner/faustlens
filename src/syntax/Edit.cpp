#include "syntax/Edit.h"

#include "syntax/Lexer.h"
#include "syntax/Prec.h"

#include <charconv>
#include <span>
#include <string>

namespace faustlens {
namespace {

// The one non-trivia token of `text`, or `Tok::Eof` where it is not exactly one.
Tok SoleToken(std::string_view text) {
    Tok found = Tok::Eof;
    for (const Token &t : Lex(text).Tokens) {
        if (t.Kind == Tok::Eof || IsTrivia(t.Kind)) continue;
        if (found != Tok::Eof) return Tok::Eof;
        found = t.Kind;
    }
    return found;
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

// The entry index of the (in, out) pair, or `entries.size()` where there is none.
size_t FindPair(const Terms &t, std::span<const ValueId> entries, uint32_t in, uint32_t out) {
    for (size_t i = 0; i + 1 < entries.size(); i += 2) {
        uint32_t a = 0, b = 0;
        if (DecimalInt(t, entries[i], &a) && DecimalInt(t, entries[i + 1], &b) && a == in && b == out) return i;
    }
    return entries.size();
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

} // namespace

bool IsComposition(Kind k) { return k >= Kind::Seq && k <= Kind::RecComp; }

bool IsExpression(Kind k) { return ((StatementKinds >> int(k)) & 1) == 0; }

EditContext::EditContext(faustlens::Terms &t, const RefTree &tree) : Terms(t), Refs(tree) {
    ParentOf.assign(Refs.Refs.size(), NoRef);
    for (RefId r = 0; r < Refs.Refs.size(); ++r)
        for (const RefId c : Refs.Children(r)) ParentOf[c] = r;
}

ValueId EditContext::Fold(Kind comp, uint8_t form, std::span<const ValueId> chain) {
    if (chain.size() == 1) return chain.front();
    if (RowOf(comp).Assoc == Assoc::Right) {
        ValueId v = chain.back();
        for (size_t i = chain.size() - 1; i-- > 0;) v = Terms.Make(comp, form, 0, 0, {chain[i], v});
        return v;
    }
    ValueId v = chain.front();
    for (size_t i = 1; i < chain.size(); ++i) v = Terms.Make(comp, form, 0, 0, {v, chain[i]});
    return v;
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
                std::vector<ValueId> chain{ValueOf(kids[0]), ValueOf(kids[1])};
                const size_t at = (sel_is_left ? 0 : 1) + (side == Side::After ? 1 : 0);
                chain.insert(chain.begin() + long(at), stage);
                return {up, Fold(comp, form, chain)};
            }
        }
    }
    const ValueId self = ValueOf(sel);
    return {sel, Terms.Make(comp, form, 0, 0, {side == Side::After ? self : stage, side == Side::After ? stage : self})};
}

Edit EditContext::Delete(RefId sel) {
    if (sel >= Refs.Refs.size()) return {NoRef, NoTerm, "nothing is selected"};
    const RefId up = Parent(sel);
    if (up == NoRef) return {NoRef, NoTerm, "nothing holds this stage"};
    if (!IsComposition(KindAt(up))) return {NoRef, NoTerm, "only a stage of a composition can be removed"};
    const auto kids = Refs.Children(up);
    if (kids.size() != 2) return {NoRef, NoTerm, "only a stage of a composition can be removed"};
    return {up, ValueOf(kids[0] == sel ? kids[1] : kids[0])};
}

Edit EditContext::Retext(RefId sel, std::string_view text) {
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
        return {sel, Terms.Make(kind, node.Form, node.Variants, Terms.InternStr(text), kids)};
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

Edit EditContext::Rewire(RefId route, std::span<const ValueId> entries) {
    const ValueId self = ValueOf(route);
    const ValueId ins = Terms.Child(self, 0), outs = Terms.Child(self, 1);
    std::vector<ValueId> kids{ins, outs};
    // No entries is `route(n,m)`, distinct from an empty list.
    if (!entries.empty()) kids.push_back(Fold(Kind::Par, 0, entries));
    return {route, Terms.Make(Kind::Route, kids)};
}

const char *EditContext::PairedEntries(RefId route, std::vector<ValueId> &out) const {
    if (route >= Refs.Refs.size() || KindAt(route) != Kind::Route) return "not a route";
    out = Entries(route);
    return out.size() % 2 == 0 ? nullptr : "its entries do not pair up";
}

Edit EditContext::Connect(RefId route, uint32_t in, uint32_t out) {
    std::vector<ValueId> entries;
    if (const char *why = PairedEntries(route, entries)) return {NoRef, NoTerm, why};
    // Already wired: the same value back, so the splice is empty.
    if (FindPair(Terms, entries, in, out) != entries.size()) return {route, ValueOf(route)};
    entries.push_back(Terms.MakeLeaf(Kind::Int, Terms.InternStr(std::to_string(in))));
    entries.push_back(Terms.MakeLeaf(Kind::Int, Terms.InternStr(std::to_string(out))));
    return Rewire(route, entries);
}

Edit EditContext::Disconnect(RefId route, uint32_t in, uint32_t out) {
    std::vector<ValueId> entries;
    if (const char *why = PairedEntries(route, entries)) return {NoRef, NoTerm, why};
    const size_t at = FindPair(Terms, entries, in, out);
    if (at == entries.size()) return {NoRef, NoTerm, "no such connection"};
    entries.erase(entries.begin() + long(at), entries.begin() + long(at) + 2);
    return Rewire(route, entries);
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
