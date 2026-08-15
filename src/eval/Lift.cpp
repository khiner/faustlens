#include "eval/Lift.h"

#include <cmath>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

namespace faustlens {
namespace {

// The shortest spelling that reads back as itself and still lexes as a `Float`. Empty for `inf`
// and `nan`, which have no `Float` spelling at all: written out they lex as identifiers.
std::string RealText(double v) {
    if (!std::isfinite(v)) return {};
    // `std::format` is the shortest round-tripping spelling by construction, so there is
    // nothing to search and nothing to read back.
    std::string s = std::format("{}", v);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

std::string Quoted(std::string_view s) { return std::format("\"{}\"", s); }

struct Lifter {
    Terms &Terms;
    const Boxes &Boxes;
    std::unordered_map<BoxId, ValueId> Memo;
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

    // Copied out first, since `Terms::Make` grows the pool a held span would point into.
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
    if (const auto it = Memo.find(b); it != Memo.end()) return it->second;
    if (Depth >= MaxTermDepth) return Decline(b, "the evaluated graph is too deep to write");
    ++Depth;
    const ValueId v = Build(b);
    --Depth;
    if (v != NoTerm) Memo.emplace(b, v);
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
            // `^` is infix only, so the nullary spelling must be `pow`.
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
                // `Form` is whether the samples were all integral, which picks the nature.
                const ValueId v = n.Form == 0 ? Int(int32_t(d)) : Real(d);
                if (v == NoTerm) return Decline(b, "a sample the grammar cannot write");
                kids.push_back(v);
            }
            return Terms.Make(Kind::Waveform, kids);
        }

        case BoxKind::FConst:
        case BoxKind::FVar:
            // `Aux` is the include file's string id here, not a signature index.
            kids.push_back(Str(Terms.Str(n.Aux)));
            return Terms.Make(kind == BoxKind::FConst ? Kind::FConst : Kind::FVar, n.Form, 0, n.Payload, kids);
        case BoxKind::FFun: {
            // The *selected* name only: build precision resolved the four-name spelling.
            const Signature &sig = Boxes.SignatureAt(n.Aux);
            static constexpr std::string_view Types[] = {"int", "float", "any"};
            kids.push_back(Str(Types[size_t(sig.Result)]));
            kids.push_back(Str(Terms.Str(n.Payload)));
            for (const uint8_t a : sig.Args) kids.push_back(Str(Types[a]));
            kids.push_back(Str(Terms.Str(sig.Include)));
            kids.push_back(Str(Terms.Str(sig.Library)));
            return Terms.Make(Kind::FFun, 1, 0, 0, kids);
        }

        // `Symbolic(slot, body)` is `\(x).(body)`, named by the unique slot number.
        case BoxKind::Slot: return Terms.MakeLeaf(Kind::Ident, Terms.InternStr(std::format("fl_slot{}", n.Aux)));
        case BoxKind::Symbolic: {
            if (!Kids(b, kids)) return NoTerm;
            const StrId name = Terms.Get(kids[0]).Payload;
            return Terms.Make(Kind::Lambda, {Terms.MakeLeaf(Kind::Str, name), kids[1]});
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

Lifted Lift(Terms &terms, const Boxes &boxes, BoxId b) { return Lifter{terms, boxes}.Run(b); }

} // namespace faustlens
