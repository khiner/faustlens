#include "box/Box.h"

#include <algorithm>
#include <array>

namespace faustlens {
namespace {

constexpr uint64_t Seed = 0x51ED270B7C4A1E35ull;

// Input counts, one row group per `PrimText` row in term.cpp -- keep the two in
// step. The trailing 0 is `Count_`.
constexpr std::array<uint8_t, size_t(Prim::Count_) + 1> PrimArities = {
    1, 1, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1,
    1, 2, 1, 1, 1, 1, 1, 1, 2, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 3, 5, 3, 4, 3, 1, 1, 0,
};

constexpr std::array<std::string_view, size_t(BoxKind::Count_)> Names = {
    "Int",   "Real",      "Wire",           "Cut",        "Prim", "FFun",  "FConst", "FVar", "Button", "Checkbox",    "NumericWidget", "Bargraph",
    "Group", "Soundfile", "Waveform",       "Seq",        "Par",  "Split", "Merge",  "Rec",  "Route",  "Environment", "Slot",          "Symbolic",
    "Error", "Closure",   "PatternMatcher", "PatternVar",
};

template<class Table, class T> uint32_t Append(Table &t, T &&x) {
    t.push_back(std::forward<T>(x));
    return uint32_t(t.size() - 1);
}

bool Compose(BoxKind k, const Arity &a, const Arity &b, Arity &out) {
    const int32_t u = a.Ins, v = a.Outs, x = b.Ins, y = b.Outs;
    switch (k) {
        case BoxKind::Seq:
            if (v != x) return false;
            out = {u, y, true};
            return true;
        case BoxKind::Par: out = {u + x, v + y, true}; return true;
        case BoxKind::Split:
            if (v == 0 || x == 0 || x % v != 0) return false;
            out = {u, y, true};
            return true;
        case BoxKind::Merge:
            if (v == 0 || x == 0 || v % x != 0) return false;
            out = {u, y, true};
            return true;
        case BoxKind::Rec:
            if (x > v || y > u) return false;
            out = {std::max(0, u - y), v, true};
            return true;
        default: return false;
    }
}

} // namespace

std::string_view BoxKindName(BoxKind k) { return Names[size_t(k)]; }

uint8_t PrimArity(Prim p) { return PrimArities[size_t(p)]; }

Boxes::Boxes() : Wire(Make(BoxKind::Wire, 0, 0, 0, {})) { Error = Make(BoxKind::Error, 0, 0, 0, {}); }

Arity Boxes::Infer(BoxKind k, uint32_t payload, uint32_t aux, std::span<const BoxId> children) const {
    switch (k) {
        case BoxKind::Int:
        case BoxKind::Real:
        case BoxKind::Slot:
        case BoxKind::Button:
        case BoxKind::Checkbox:
        case BoxKind::NumericWidget:
        case BoxKind::FConst:
        case BoxKind::FVar: return {0, 1, true};
        case BoxKind::Wire: return {1, 1, true};
        case BoxKind::Cut: return {1, 0, true};
        case BoxKind::Bargraph: return {1, 1, true};
        case BoxKind::Waveform: return {0, 2, true};
        case BoxKind::Environment: return {0, 0, true};
        case BoxKind::Prim: return {PrimArity(Prim(payload)), 1, true};
        case BoxKind::FFun: return {int32_t(Signatures[aux].Args.size()), 1, true};
        case BoxKind::Soundfile: return {2, 2 + int32_t(aux), true};
        case BoxKind::Route:
            if (aux == 0) return {};
            return {Routes[aux - 1].Ins, Routes[aux - 1].Outs, true};
        case BoxKind::Group: return Arities[children[0]];
        case BoxKind::Symbolic: {
            const Arity &b = Arities[children[1]];
            return b.Known ? Arity{b.Ins + 1, b.Outs, true} : Arity{};
        }
        case BoxKind::Seq:
        case BoxKind::Par:
        case BoxKind::Split:
        case BoxKind::Merge:
        case BoxKind::Rec: {
            const Arity &a = Arities[children[0]], &b = Arities[children[1]];
            Arity out;
            // Untyped rather than wrong, so a pattern holding a `PatternVar`
            // composes freely.
            if (!a.Known || !b.Known) return {};
            return Compose(k, a, b, out) ? out : Arity{};
        }
        default: return {}; // Error, PatternVar, Closure, PatternMatcher
    }
}

BoxId Boxes::Make(BoxKind kind, uint8_t form, uint32_t payload, uint32_t aux, std::span<const BoxId> children) {
    // `Error` absorbs its neighbours, so one typo yields one diagnostic and not
    // one per enclosing composition.
    if (Error != NoBox)
        for (const BoxId c : children)
            if (c == Error) return Error;

    uint64_t h = Mix(Seed, (uint64_t(kind) << 8) | form);
    h = Mix(h, payload);
    h = Mix(h, aux);
    for (const BoxId c : children) h = Mix(h, Hashes[c]);

    const BoxNode proto{uint8_t(kind), form, payload, aux, 0, 0};
    if (const BoxId id = Find(h, proto, children); id != NotFound) return id;

    const Arity arity = Infer(kind, payload, aux, children);
    // The net under `Composable`: a mismatched composition is unrepresentable.
    if (!arity.Known && IsComposition(kind) && Arities[children[0]].Known && Arities[children[1]].Known) return Error;

    Arities.push_back(arity);
    return Commit(proto, h, children);
}

bool Boxes::Composable(BoxKind k, BoxId a, BoxId b) const {
    const Arity &x = Arities[a], &y = Arities[b];
    if (!x.Known || !y.Known) return true; // nothing to disagree about yet
    Arity out;
    return Compose(k, x, y, out);
}

uint32_t Boxes::AddBounds(const faustlens::Bounds &b) { return Append(Bounds, b); }
uint32_t Boxes::AddRoute(RouteTable r) { return Append(Routes, std::move(r)); }
uint32_t Boxes::AddSignature(Signature s) { return Append(Signatures, std::move(s)); }
uint32_t Boxes::AddWaveform(std::vector<double> w) { return Append(Waveforms, std::move(w)); }
uint32_t Boxes::AddPMState(PMState s) { return Append(PmStates, std::move(s)); }

BoxId Boxes::NewSlot(StrId name) { return Make(BoxKind::Slot, 0, name, ++NextSlot, {}); }

} // namespace faustlens
