#include "conformance/SigCompare.h"

#include <format>
#include <map>

namespace faustlens::test {
namespace {

// The reference's name for one of our nodes, which is what the sides match on.
std::string OpName(const Signals &s, SigId id) {
    const SigNode &n = s.Get(id);
    switch (s.KindOf(id)) {
        case SigKind::BinOp: return std::string(BinOpName(BinOpCode(n.Form)));
        case SigKind::Extended: return std::string(ExtName(Ext(n.Form)));
        case SigKind::Delay1: return "'";
        case SigKind::Delay: return "@";
        case SigKind::Prefix: return "prefix";
        case SigKind::IntCast: return "int";
        case SigKind::FloatCast: return "float";
        case SigKind::BitCast: return "bit";
        case SigKind::Select2: return "select2";
        case SigKind::Select3: return "select3";
        case SigKind::RDTbl: return "sigRDTbl";
        case SigKind::WRTbl: return n.ChildCount == 2 ? "WRTbl2p" : "sigWRTbl4p";
        case SigKind::Gen: return "sigGen";
        case SigKind::Button: return "button";
        case SigKind::Checkbox: return "checkbox";
        case SigKind::VSlider: return "vslider";
        case SigKind::HSlider: return "hslider";
        case SigKind::NumEntry: return "nentry";
        case SigKind::VBargraph: return "vbargraph";
        case SigKind::HBargraph: return "hbargraph";
        case SigKind::Soundfile: return "soundfile";
        case SigKind::SoundfileLength: return "length";
        case SigKind::SoundfileRate: return "rate";
        case SigKind::SoundfileBuffer: return "buffer";
        case SigKind::Attach: return "attach";
        case SigKind::Control: return "control";
        case SigKind::Enable: return "enable";
        case SigKind::FFun: return std::string(s.Str(n.Payload));
        case SigKind::Proj: return std::format("proj{}", n.Payload);
        case SigKind::Rec: return "letrec";
        default: return std::string(SigKindName(s.KindOf(id)));
    }
}

struct Comparer {
    using Key = std::pair<SigId, const SigTerm *>;

    const Signals &S;
    const SigFile &F;
    std::map<Key, bool> Memo;
    // Which of our groups each `Wn` stands for, inside the branches it heads.
    std::map<int64_t, SigId> Rec;
    // `Wn` to its `letrec`, for occurrences outside those branches: the printer ids a
    // recursion's body before the `letrec` line.
    std::map<int64_t, const SigTerm *> Binder;
    std::vector<std::string> Path;

    Comparer(const Signals &sigs, const SigFile &file) : S(sigs), F(file) {
        for (const SigTerm &d : F.Defs)
            if (d.Kind == SigTerm::Kind::Op && d.Text == "letrec" && !d.Args.empty()) Binder[d.Args[0].I] = &d;
    }

    std::string Why;
    // Where the walk parted.
    SigId At = NoSig;

    bool Equal(SigId a, const SigTerm &b) {
        const SigTerm &t = Resolve(b);
        const Key key{a, &t};
        if (const auto it = Memo.find(key); it != Memo.end()) return it->second;
        Memo[key] = true; // assume, so a shared subgraph is not re-walked
        const bool r = Compare(a, t);
        Memo[key] = r;
        return r;
    }

    struct Step {
        Comparer &C;
        Step(Comparer &owner, std::string label) : C(owner) { C.Path.push_back(std::move(label)); }
        ~Step() { C.Path.pop_back(); }
    };

    std::string PathText() const {
        std::string out;
        for (const std::string &seg : Path) out += "/" + seg;
        return out.empty() ? "/" : out;
    }

    const SigTerm &Resolve(const SigTerm &t) const { return t.Kind == SigTerm::Kind::Id ? F.Defs[t.I] : t; }

    bool No(SigId a, const SigTerm &b, std::string_view what) {
        if (Why.empty()) {
            At = a;
            const std::string mine = a == NoSig ? "?" : OpName(S, a);
            std::string theirs;
            switch (b.Kind) {
                case SigTerm::Kind::Op: theirs = b.Text; break;
                case SigTerm::Kind::Int: theirs = "int"; break;
                case SigTerm::Kind::Real: theirs = "real"; break;
                case SigTerm::Kind::Input: theirs = "IN"; break;
                case SigTerm::Kind::RecVar: theirs = "W"; break;
                case SigTerm::Kind::Waveform: theirs = "waveform"; break;
                case SigTerm::Kind::Name: theirs = b.Text; break;
                default: theirs = "?"; break;
            }
            Why = std::format("{} [{} vs {}] at {}: ours is {}, theirs is {}", what, mine, theirs, PathText(), PrintSig(S, a), PrintSigTerm(b));
        }
        return false;
    }

    bool Children(SigId a, const SigTerm &t, size_t first_arg, size_t first_child) {
        const auto kids = S.Children(a);
        if (t.Args.size() - first_arg != kids.size() - first_child) return No(a, t, "operand count");
        for (size_t i = 0; i + first_child < kids.size(); ++i) {
            Step const step(*this, std::format("{}.{}", OpName(S, a), i + first_child));
            if (!Equal(kids[i + first_child], t.Args[i + first_arg])) return false;
        }
        return true;
    }

    bool Compare(SigId a, const SigTerm &t) {
        const SigNode &n = S.Get(a);
        const SigKind k = S.KindOf(a);

        switch (t.Kind) {
            case SigTerm::Kind::Int:
                if (k == SigKind::Int && S.IntValue(a) == t.I) return true;
                return No(a, t, "integer");
            case SigTerm::Kind::Real:
                // The printer writes full precision, so exact comparison hides no drift.
                if (k == SigKind::Real && S.RealValue(a) == t.D) return true;
                return No(a, t, "real");
            case SigTerm::Kind::Input:
                if (k == SigKind::Input && n.Payload == uint32_t(t.I)) return true;
                return No(a, t, "input");
            case SigTerm::Kind::Waveform:
                // The printer elides the contents, so shape is all there is.
                if (k == SigKind::Waveform) return true;
                return No(a, t, "waveform");
            case SigTerm::Kind::Name:
                if ((k == SigKind::FConst || k == SigKind::FVar) && S.Str(n.Payload) == t.Text) return true;
                return No(a, t, "foreign name");
            case SigTerm::Kind::RecVar: {
                if (const auto it = Rec.find(t.I); it != Rec.end() && it->second == a) return true;
                // Not the group we are inside, so compare against its `letrec`, stopping the cycle.
                const auto b = Binder.find(t.I);
                if (b == Binder.end()) return No(a, t, "a recursive variable with no letrec");
                return Equal(a, *b->second);
            }
            case SigTerm::Kind::String: return No(a, t, "a label outside a widget");
            case SigTerm::Kind::List: return No(a, t, "a list where a signal was expected");
            case SigTerm::Kind::Id: // `Resolve` already followed it
            case SigTerm::Kind::Op: break;
        }

        // `x'` covers both a one-sample delay and a delay whose index folded to 1.
        if (t.Text == "'") {
            if (k == SigKind::Delay1) return Children(a, t, 0, 0);
            if (k == SigKind::Delay && S.IsInt(S.Child(a, 1)) && S.IntValue(S.Child(a, 1)) == 1) {
                Step const step(*this, "'.0");
                return Equal(S.Child(a, 0), t.Args[0]);
            }
            return No(a, t, "one-sample delay");
        }

        if (t.Text == "letrec") {
            if (k != SigKind::Rec) return No(a, t, "recursive group");
            const int64_t var = t.Args[0].I;
            const auto prev = Rec.find(var);
            const SigId saved = prev == Rec.end() ? NoSig : prev->second;
            Rec[var] = a;
            const bool ok = Children(a, t.Args[1], 0, 0);
            if (saved == NoSig) Rec.erase(var);
            else Rec[var] = saved;
            return ok;
        }

        if (OpName(S, a) != t.Text) return No(a, t, "operation");

        if (IsLabelled(k)) {
            if (t.Args.empty() || t.Args[0].Kind != SigTerm::Kind::String) return No(a, t, "a widget without a label");
            if (S.Str(n.Payload) != t.Args[0].Text) return No(a, t, "label");
            // A `soundfile`'s channel count stays on the box, so the dump lacks it.
            if (k == SigKind::Soundfile) return true;
            return Children(a, t, 1, 0);
        }
        return Children(a, t, 0, 0);
    }
};

} // namespace

std::string PrintSig(const Signals &s, SigId id, int max_depth) {
    const SigNode &n = s.Get(id);
    std::string out;
    switch (s.KindOf(id)) {
        case SigKind::Int: return std::to_string(s.IntValue(id));
        case SigKind::Real: return std::to_string(s.RealValue(id));
        case SigKind::Input: return std::format("IN[{}]", n.Payload);
        case SigKind::FConst:
        case SigKind::FVar: return std::string(s.Str(n.Payload));
        case SigKind::Waveform: return "waveform";
        default: out = OpName(s, id); break;
    }
    if (IsLabelled(s.KindOf(id))) out += std::format("\"{}\"", s.Str(n.Payload));
    if (n.ChildCount == 0) return out;
    if (max_depth <= 0) return out + "(...)";
    out += "(";
    for (uint32_t i = 0; i < n.ChildCount; ++i) {
        if (i) out += ", ";
        out += PrintSig(s, s.Child(id, i), max_depth - 1);
    }
    return out + ")";
}

std::expected<void, std::string> SigIsomorphic(const Signals &s, std::span<const SigId> ours, const SigFile &theirs, SigId *diverged) {
    if (ours.size() != theirs.Outputs.Args.size()) return std::unexpected(std::format("{} outputs against {}", ours.size(), theirs.Outputs.Args.size()));
    Comparer c(s, theirs);
    for (size_t i = 0; i < ours.size(); ++i) {
        if (c.Equal(ours[i], theirs.Outputs.Args[i])) continue;
        if (diverged) *diverged = c.At;
        return std::unexpected(std::format("output {}: {}", i, c.Why));
    }
    return {};
}

} // namespace faustlens::test
