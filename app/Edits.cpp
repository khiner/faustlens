#include "Edits.h"

#include "syntax/Printer.h"
#include "syntax/Splice.h"

#include <algorithm>

namespace faustlens::app {
namespace {

// Walking outward from the caret is what makes a widget's arguments reachable.
RefId TextRef(const Terms &terms, const FileView &f, const boxview::Selection &sel) {
    for (const RefId r : f.Refs.Chain(sel.Caret)) {
        const Kind k = terms.KindOf(f.Refs.Refs[r].ValueId);
        if (k == Kind::Int || k == Kind::Real || IsLabelled(k)) return r;
    }
    return NoRef;
}

constexpr Connective ConnectiveTable[] = {
    {':', Key::Sequence}, {',', Key::Parallel}, {'<', Key::Split}, {'>', Key::Merge}, {'~', Key::Recursive},
};

struct Composition {
    Kind Kind = Kind::Count_;
    uint8_t Form = 0;
};

Composition CompositionFor(Key k) {
    switch (k) {
        case Key::Sequence: return {Kind::Seq, 0};
        case Key::Parallel: return {Kind::Par, 0};
        case Key::Split: return {Kind::Split, 0};
        case Key::Merge: return {Kind::Merge, uint8_t(MergeSpelling::Colon)};
        case Key::Recursive: return {Kind::RecComp, 0};
        default: return {};
    }
}

} // namespace

std::span<const Connective> Connectives() { return ConnectiveTable; }

Key KeyForChar(unsigned c) {
    for (const Connective &b : ConnectiveTable)
        if (unsigned(b.Char) == c) return b.Edit;
    return Key::None;
}

Edit EditFor(Terms &terms, const FileView &f, const boxview::Selection &sel, Key key) {
    const RefId at = boxview::SelectedRef(f, sel);
    if (at == NoRef) return {NoRef, NoTerm, "nothing is selected"};
    EditContext ed(terms, f.Refs);
    if (key == Key::Remove) return ed.Delete(at);
    const Composition c = CompositionFor(key);
    if (c.Kind == Kind::Count_) return {NoRef, NoTerm, "no edit is bound to that key"};
    return ed.Compose(at, c.Kind, c.Form, Side::After);
}

std::string ComposeExample(Terms &terms, Key key) {
    const Composition c = CompositionFor(key);
    if (c.Kind == Kind::Count_) return {};
    // A stand-in for the selection, and the `_` a bare key press supplies.
    const ValueId a = terms.MakeLeaf(Kind::Ident, terms.InternStr("a"));
    const ValueId wire = terms.MakePrim(Prim::Wire);
    return PrintTerm(terms, terms.Make(c.Kind, c.Form, 0, 0, {a, wire}));
}

std::string_view TextOf(const Terms &terms, const FileView &f, const boxview::Selection &sel) {
    const RefId at = TextRef(terms, f, sel);
    return at == NoRef ? std::string_view() : terms.Lexeme(f.Refs.Refs[at].ValueId);
}

Edit EditForText(Terms &terms, const FileView &f, const boxview::Selection &sel, std::string_view text) {
    const RefId at = TextRef(terms, f, sel);
    if (at == NoRef) return {NoRef, NoTerm, "this node has no text of its own"};
    return EditContext(terms, f.Refs).Retext(at, text);
}

Edit RewireDrag(Terms &terms, const FileView &f, const boxview::Selection &route, uint32_t in, uint32_t out) {
    const RefId at = boxview::SelectedRef(f, route);
    if (at == NoRef) return {NoRef, NoTerm, "that is not a node this file wrote"};
    EditContext ctx(terms, f.Refs);
    // Read off the term rather than off what the drag looked like.
    const Wiring w = RouteWiring(terms, f.Refs.Refs[at].ValueId);
    const bool wired = std::ranges::contains(w.Pairs, std::pair{in, out});
    return wired ? ctx.Disconnect(at, in, out) : ctx.Connect(at, in, out);
}

bool Apply(Session &session, Workspace &ws, const std::string &path, const FileView &view, const Edit &e) {
    if (e.Target == NoRef) return false;
    const Buffer *b = ws.Find(path);
    // `view`'s refs address `view.text`, and mean nothing against other bytes.
    if (b == nullptr || view.Text != b->Text()) return false;
    const SpliceContext ctx(session.Terms, view.Text, view.Refs, view.Tokens);
    const EditScript script = ctx.Splice(e.Target, e.Value);
    if (script.empty()) return false; // an identity edit owes no revision
    if (!ws.Edit(path, script)) return false;
    session.SetBuffer(path, ws.Find(path)->Text());
    return true;
}

} // namespace faustlens::app
