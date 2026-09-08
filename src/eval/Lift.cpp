#include "eval/Lift.h"

#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace faustlens {
namespace {

// Return the shortest round-tripping Float spelling, or empty for infinity and NaN.
std::string RealText(double v) {
    if (!std::isfinite(v)) return {};
    std::string s = std::format("{}", v);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

std::string Quoted(std::string_view s) { return std::format("\"{}\"", s); }

struct Lifter {
    Terms &Terms;
    const Boxes &Boxes;
    std::unordered_map<uint64_t, ValueId> Memo;
    std::unordered_map<uint32_t, StrId> Names;
    std::unordered_set<StrId> UsedNames;
    uint32_t Scope = 0, NextScope = 0;
    const char *Why = nullptr;
    BoxId At = NoBox;
    uint32_t Depth = 0;

    Lifted Run(BoxId b) {
        const ValueId v = Go(b);
        if (v != NoTerm) return {v, nullptr, NoBox};
        return {NoTerm, Why, At};
    }

    ValueId Decline(BoxId b, const char *reason) {
        if (Why == nullptr) {
            Why = reason;
            At = b;
        }
        return NoTerm;
    }

    ValueId Str(std::string_view lexeme) { return Terms.MakeLeaf(Kind::Str, Terms.InternStr(lexeme)); }
    ValueId Int(int32_t n) { return Terms.MakeLeaf(Kind::Int, Terms.InternStr(std::to_string(n))); }
    ValueId Real(double d) {
        const std::string text = RealText(d);
        return text.empty() ? NoTerm : Terms.MakeLeaf(Kind::Real, Terms.InternStr(text));
    }

    bool Kids(BoxId b, std::vector<ValueId> &out) {
        const auto span = Boxes.Children(b);
        const std::vector<BoxId> kids(span.begin(), span.end());
        for (const BoxId k : kids) {
            const ValueId v = Go(k);
            if (v == NoTerm) return false;
            out.push_back(v);
        }
        return true;
    }

    ValueId Go(BoxId b);
    ValueId Build(BoxId b);
};

ValueId Lifter::Go(BoxId b) {
    if (Why != nullptr) return NoTerm;
    const uint64_t key = (uint64_t(Scope) << 32) | b;
    if (const auto it = Memo.find(key); it != Memo.end()) return it->second;
    if (Depth >= MaxTermDepth) return Decline(b, "the evaluated graph is too deep to write");
    ++Depth;
    const ValueId v = Build(b);
    --Depth;
    if (v != NoTerm) Memo.emplace(key, v);
    return v;
}

ValueId Lifter::Build(BoxId b) {
    const BoxNode n = Boxes.Get(b);
    const BoxKind kind = Boxes.KindOf(b);
    std::vector<ValueId> kids;
    switch (kind) {
        case BoxKind::Int: return Int(Boxes.IntValue(b));
        case BoxKind::Real: {
            const ValueId v = Real(Boxes.RealValue(b));
            return v == NoTerm ? Decline(b, "a number the grammar cannot write") : v;
        }
        case BoxKind::Wire: return Terms.MakePrim(Prim::Wire);
        case BoxKind::Cut: return Terms.MakePrim(Prim::Cut);
        case BoxKind::Prim: {
            // Use pow for nullary power because ^ is infix-only.
            const auto p = Prim(n.Payload);
            const auto form = uint8_t(p == Prim::Pow ? PowSpelling::Fun : PowSpelling::Caret);
            return Terms.Make(Kind::Prim, form, 0, n.Payload, {});
        }

        case BoxKind::Seq:
        case BoxKind::Par:
        case BoxKind::Split:
        case BoxKind::Merge:
        case BoxKind::Rec: {
            if (!Kids(b, kids)) return NoTerm;
            static constexpr Kind Map[] = {Kind::Seq, Kind::Par, Kind::Split, Kind::Merge, Kind::RecComp};
            const auto i = size_t(kind) - size_t(BoxKind::Seq);
            return Terms.Make(Map[i], kids);
        }
        case BoxKind::Route: return Kids(b, kids) ? Terms.Make(Kind::Route, kids) : NoTerm;

        case BoxKind::Button:
        case BoxKind::Checkbox: return Terms.MakeLeaf(kind == BoxKind::Button ? Kind::Button : Kind::Checkbox, Terms.InternStr(Quoted(Terms.Str(n.Payload))));
        case BoxKind::NumericWidget:
        case BoxKind::Bargraph: {
            const Bounds bounds = Boxes.BoundsAt(n.Aux);
            const bool numeric = kind == BoxKind::NumericWidget;
            const double values[] = {bounds.Init, bounds.Min, bounds.Max, bounds.Step};
            for (int i = numeric ? 0 : 1; i < (numeric ? 4 : 3); ++i) {
                const ValueId v = Real(values[i]);
                if (v == NoTerm) return Decline(b, "a bound the grammar cannot write");
                kids.push_back(v);
            }
            const StrId label = Terms.InternStr(Quoted(Terms.Str(n.Payload)));
            return Terms.Make(numeric ? Kind::NumericWidget : Kind::Bargraph, n.Form, 0, label, kids);
        }
        case BoxKind::Group: {
            if (!Kids(b, kids)) return NoTerm;
            return Terms.Make(Kind::Group, n.Form, 0, Terms.InternStr(Quoted(Terms.Str(n.Payload))), kids);
        }
        case BoxKind::Soundfile: {
            kids.push_back(Int(int32_t(n.Aux)));
            return Terms.Make(Kind::SoundfileBox, 0, 0, Terms.InternStr(Quoted(Terms.Str(n.Payload))), kids);
        }
        case BoxKind::Waveform: {
            const std::vector<double> values = Boxes.WaveformAt(n.Aux);
            for (const double d : values) {
                const ValueId v = n.Form == 0 ? Int(int32_t(d)) : Real(d);
                if (v == NoTerm) return Decline(b, "a sample the grammar cannot write");
                kids.push_back(v);
            }
            return Terms.Make(Kind::Waveform, kids);
        }

        case BoxKind::FConst:
        case BoxKind::FVar:
            // Aux identifies the include filename here.
            kids.push_back(Str(Terms.Str(n.Aux)));
            return Terms.Make(kind == BoxKind::FConst ? Kind::FConst : Kind::FVar, n.Form, 0, n.Payload, kids);
        case BoxKind::FFun: {
            // Preserve the foreign name selected by build precision.
            const Signature &sig = Boxes.SignatureAt(n.Aux);
            static constexpr std::string_view Types[] = {"int", "float", "any"};
            kids.push_back(Str(Types[size_t(sig.Result)]));
            kids.push_back(Str(Terms.Str(n.Payload)));
            for (const uint8_t a : sig.Args) kids.push_back(Str(Types[a]));
            kids.push_back(Str(Terms.Str(sig.Include)));
            kids.push_back(Str(Terms.Str(sig.Library)));
            return Terms.Make(Kind::FFun, 1, 0, 0, kids);
        }

        case BoxKind::Slot: {
            const auto it = Names.find(n.Aux);
            return it == Names.end() ? Decline(b, "the evaluated expression has a free parameter outside this scope") : Terms.MakeLeaf(Kind::Ident, it->second);
        }
        case BoxKind::Symbolic: {
            const uint32_t slot = Boxes.Get(Boxes.Child(b, 0)).Aux;
            StrId name = Terms.InternStr(std::format("fl_slot{}", slot));
            for (uint32_t suffix = 1; UsedNames.contains(name); ++suffix) name = Terms.InternStr(std::format("fl_slot{}_{}", slot, suffix));
            UsedNames.insert(name);
            const auto previous = Names.find(slot);
            const std::optional<StrId> saved = previous == Names.end() ? std::nullopt : std::optional(previous->second);
            Names[slot] = name;
            const uint32_t outer = Scope;
            Scope = ++NextScope;
            const ValueId body = Go(Boxes.Child(b, 1));
            Scope = outer;
            if (saved) Names[slot] = *saved;
            else Names.erase(slot);
            if (body == NoTerm) return NoTerm;
            return Terms.Make(Kind::Lambda, {Terms.MakeLeaf(Kind::Str, name), body});
        }

        case BoxKind::Error: return Decline(b, "the program does not compile here");
        case BoxKind::Closure:
        case BoxKind::PatternMatcher:
        case BoxKind::PatternVar: return Decline(b, "an unapplied function is not a circuit");
        case BoxKind::Environment: return Decline(b, "an environment is not a circuit");
        case BoxKind::Count_: break;
    }
    return Decline(b, "not a circuit");
}

} // namespace

Lifted Lift(Terms &terms, const Boxes &boxes, BoxId b, std::span<const SlotName> ambient) {
    Lifter lifter{terms, boxes};
    for (const SlotName &slot : ambient) {
        lifter.Names.emplace(slot.Slot, slot.Name);
        lifter.UsedNames.insert(slot.Name);
    }
    return lifter.Run(b);
}

} // namespace faustlens
