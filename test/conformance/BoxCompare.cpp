#include "conformance/BoxCompare.h"

#include <cmath>
#include <format>
#include <map>
#include <set>
#include <span>
#include <tuple>
#include <utility>

namespace faustlens::test {
namespace {

enum class Payload { None, Str, Int, Real, Prim };

Payload PayloadOf(BoxKind k) {
    switch (k) {
        case BoxKind::Int: return Payload::Int;
        case BoxKind::Real: return Payload::Real;
        case BoxKind::Prim: return Payload::Prim;
        case BoxKind::FFun:
        case BoxKind::FConst:
        case BoxKind::FVar:
        case BoxKind::Button:
        case BoxKind::Checkbox:
        case BoxKind::NumericWidget:
        case BoxKind::Bargraph:
        case BoxKind::Group:
        case BoxKind::Soundfile:
        case BoxKind::PatternVar: return Payload::Str;
        // A slot's payload is a name reparsing renames, so only the bijection counts.
        case BoxKind::Slot: return Payload::None;
        default: return Payload::None;
    }
}

// `.box` files print at limited precision, so reals need a relative tolerance.
bool Close(double p, double q) { return p == q || std::fabs(p - q) <= 1e-9 * std::max(1.0, std::fabs(p)); }

struct Comparer {
    struct Key {
        BoxId X, Y;
        uint32_t Depth;
        bool operator<(const Key &o) const { return std::tie(X, Y, Depth) < std::tie(o.X, o.Y, o.Depth); }
    };

    const BoxSide &A;
    const BoxSide &B;
    std::map<Key, bool> Memo;
    // Enclosing `Symbolic` bindings, innermost last. Scoped, not global: one shared subtree
    // can carry two slot numbers.
    std::vector<std::pair<uint32_t, uint32_t>> Binders;
    std::string Why;

    bool Equal(BoxId x, BoxId y) {
        // Binder depth is in the key: one pair can be asked under two `Symbolic` scopes.
        const Key key{x, y, uint32_t(Binders.size())};
        if (const auto it = Memo.find(key); it != Memo.end()) return it->second;
        Memo[key] = true; // assume, so a shared subgraph is not re-walked
        const bool r = Compare(x, y);
        Memo[key] = r;
        return r;
    }

    bool No(BoxId x, BoxId y, std::string reason) {
        if (Why.empty()) Why = std::move(reason) + "\n  left:  " + PrintBox(A, x, 6) + "\n  right: " + PrintBox(B, y, 6);
        return false;
    }

    bool Compare(BoxId x, BoxId y) {
        const BoxNode &nx = A.Boxes.Get(x);
        const BoxNode &ny = B.Boxes.Get(y);
        const BoxKind kx = A.Boxes.KindOf(x), ky = B.Boxes.KindOf(y);
        // Before the kind test: `x : (a <: b)` and `(x : a) <: b` are one diagram.
        if (Chained(kx) || Chained(ky)) return SameChain(x, y);
        if (kx != ky) return No(x, y, "different kinds");
        if (nx.Form != ny.Form) return No(x, y, "different form tags");
        if (nx.ChildCount != ny.ChildCount) return No(x, y, "different arities");

        switch (PayloadOf(kx)) {
            case Payload::Int:
                if (A.Boxes.IntValue(x) != B.Boxes.IntValue(y)) return No(x, y, "different integers");
                break;
            case Payload::Real:
                if (!Close(A.Boxes.RealValue(x), B.Boxes.RealValue(y))) return No(x, y, "different reals");
                break;
            case Payload::Prim:
                if (nx.Payload != ny.Payload) return No(x, y, "different primitives");
                break;
            case Payload::Str:
                if (A.Terms.Str(nx.Payload) != B.Terms.Str(ny.Payload)) return No(x, y, "different labels");
                break;
            case Payload::None: break;
        }

        switch (kx) {
            case BoxKind::Slot: {
                for (size_t i = Binders.size(); i-- > 0;) {
                    const auto &[l, r] = Binders[i];
                    if (l != nx.Aux && r != ny.Aux) continue;
                    return (l == nx.Aux && r == ny.Aux) || No(x, y, "slot mismatch");
                }
                return No(x, y, "a slot with no binder in scope");
            }
            case BoxKind::Symbolic: {
                Binders.emplace_back(A.Boxes.Get(A.Boxes.Child(x, 0)).Aux, B.Boxes.Get(B.Boxes.Child(y, 0)).Aux);
                const bool r = Equal(A.Boxes.Child(x, 1), B.Boxes.Child(y, 1));
                Binders.pop_back();
                return r;
            }
            case BoxKind::Soundfile:
                if (nx.Aux != ny.Aux) return No(x, y, "different channel counts");
                break;
            case BoxKind::NumericWidget:
            case BoxKind::Bargraph: {
                const Bounds &u = A.Boxes.BoundsAt(nx.Aux);
                const Bounds &v = B.Boxes.BoundsAt(ny.Aux);
                if (!Close(u.Init, v.Init) || !Close(u.Min, v.Min) || !Close(u.Max, v.Max) || !Close(u.Step, v.Step))
                    return No(x, y, "different widget bounds");
                break;
            }
            case BoxKind::Route: {
                if ((nx.Aux == 0) != (ny.Aux == 0)) return No(x, y, "one route folded, one did not");
                if (nx.Aux != 0) {
                    const RouteTable &u = A.Boxes.RouteAt(nx.Aux - 1);
                    const RouteTable &v = B.Boxes.RouteAt(ny.Aux - 1);
                    if (u.Ins != v.Ins || u.Outs != v.Outs || u.Pairs != v.Pairs) return No(x, y, "different route tables");
                }
                break;
            }
            case BoxKind::Waveform:
                if (A.Boxes.WaveformAt(nx.Aux) != B.Boxes.WaveformAt(ny.Aux)) return No(x, y, "different waveforms");
                break;
            case BoxKind::FFun: {
                const Signature &u = A.Boxes.SignatureAt(nx.Aux);
                const Signature &v = B.Boxes.SignatureAt(ny.Aux);
                if (u.Result != v.Result || u.Args != v.Args || A.Terms.Str(u.Include) != B.Terms.Str(v.Include) ||
                    A.Terms.Str(u.Library) != B.Terms.Str(v.Library))
                    return No(x, y, "different foreign signatures");
                break;
            }
            case BoxKind::FConst:
            case BoxKind::FVar:
                if (A.Terms.Str(nx.Aux) != B.Terms.Str(ny.Aux)) return No(x, y, "different include files");
                break;
            default: break;
        }

        // One printed priority for `:`, `<:` and `:>`, so the tree is unrecoverable and the
        // chain comparison is exact, not a weakening.
        if (kx == BoxKind::Par) return SameChain(x, y);

        for (uint32_t i = 0; i < nx.ChildCount; ++i)
            if (!Equal(A.Boxes.Child(x, i), B.Boxes.Child(y, i))) return false;
        return true;
    }

    struct Chain {
        std::vector<BoxId> Operands;
        std::vector<BoxKind> Ops;
    };
    bool SameChain(BoxId x, BoxId y) {
        Chain cx, cy;
        Flatten(A, x, cx);
        Flatten(B, y, cy);
        if (cx.Ops != cy.Ops) return No(x, y, std::format("different composition chains ({} vs {})", cx.Ops.size(), cy.Ops.size()));
        for (size_t i = 0; i < cx.Operands.size(); ++i)
            if (!Equal(cx.Operands[i], cy.Operands[i])) return false;
        return true;
    }
    static bool Chained(BoxKind k) { return k == BoxKind::Seq || k == BoxKind::Split || k == BoxKind::Merge; }
    // Composition operators chain together, `,` only with itself.
    static void Flatten(const BoxSide &s, BoxId b, Chain &out, bool par = false) {
        const BoxKind k = s.Boxes.KindOf(b);
        if (out.Operands.empty() && out.Ops.empty()) par = k == BoxKind::Par;
        const bool joins = par ? k == BoxKind::Par : Chained(k);
        if (!joins) {
            out.Operands.push_back(b);
            return;
        }
        Flatten(s, s.Boxes.Child(b, 0), out, par);
        out.Ops.push_back(k);
        Flatten(s, s.Boxes.Child(b, 1), out, par);
    }
};

} // namespace

std::expected<void, std::string> Isomorphic(const BoxSide &a, BoxId x, const BoxSide &b, BoxId y) {
    Comparer c{a, b};
    if (!c.Equal(x, y)) return std::unexpected(std::move(c.Why));
    return {};
}

std::string PrintBox(const BoxSide &s, BoxId b, int max_depth) {
    const BoxNode &n = s.Boxes.Get(b);
    const BoxKind k = s.Boxes.KindOf(b);
    std::string out(BoxKindName(k));
    switch (PayloadOf(k)) {
        case Payload::Int: out += std::format("({})", s.Boxes.IntValue(b)); break;
        // `{:f}`, not `{}`: the pinned shape text spells a real as `1.500000`.
        case Payload::Real: out += std::format("({:f})", s.Boxes.RealValue(b)); break;
        case Payload::Prim: out += std::format("({})", PrimText(Prim(n.Payload))); break;
        case Payload::Str: out += std::format("({})", s.Terms.Str(n.Payload)); break;
        case Payload::None: break;
    }
    if (k == BoxKind::Slot) out += std::format("#{}", n.Aux);
    if (n.ChildCount == 0) return out;
    if (max_depth <= 0) return out + "[...]";
    out += "[";
    for (uint32_t i = 0; i < n.ChildCount; ++i) {
        if (i) out += " ";
        out += PrintBox(s, s.Boxes.Child(b, i), max_depth - 1);
    }
    return out + "]";
}

std::map<std::string, std::vector<std::string>> DeclareView(const MetaSet &m) {
    // `MetaSet` already deduplicates on (key, value), so only grouping is left.
    std::map<std::string, std::vector<std::string>> raw;
    std::vector<std::string> order;
    for (const auto &[k, v] : m.Entries) {
        if (raw.find(k) == raw.end()) order.push_back(k);
        raw[k].push_back(v);
    }

    std::map<std::string, std::vector<std::string>> out;
    for (const std::string &k : order) {
        const std::vector<std::string> &vs = raw[k];
        // `author` and `contributor` are the only keys whose duplicates survive printing.
        if (k == "author") {
            out["author"].push_back(vs.front());
            for (size_t i = 1; i < vs.size(); ++i) out["contributor"].push_back(vs[i]);
            continue;
        }
        if (k == "contributor") {
            for (const std::string &v : vs) out["contributor"].push_back(v);
            continue;
        }
        std::string key = k;
        for (char &c : key)
            if (c == '.' || c == ':' || c == '/') c = '_';
        out[key].push_back(vs.front());
    }
    return out;
}

namespace {

// Compilation keys, not program ones. `name` and `filename` are excluded: both sides make them.
bool IsSynthesizedKey(const std::string &k) {
    if (k == "version" || k == "compile_options") return true;
    constexpr std::string_view Path = "library_path";
    if (!k.starts_with(Path) || k.size() == Path.size()) return false;
    return k.find_first_not_of("0123456789", Path.size()) == std::string::npos;
}

std::string Join(std::span<const std::string> vs) {
    std::string out;
    for (const std::string &v : vs) out += (out.empty() ? "\"" : "\", \"") + v;
    return out.empty() ? "<none>" : out + "\"";
}

} // namespace

std::expected<void, std::string> SameDeclares(const MetaSet &ours, const MetaSet &theirs) {
    auto a = DeclareView(ours), b = DeclareView(theirs);
    for (auto *m : {&a, &b})
        for (auto it = m->begin(); it != m->end();) it = IsSynthesizedKey(it->first) ? m->erase(it) : std::next(it);

    for (const auto &[k, vs] : a) {
        const auto it = b.find(k);
        if (it == b.end()) return std::unexpected("declare " + k + ": " + Join(vs) + " on our side, absent on theirs");
        if (it->second != vs) return std::unexpected("declare " + k + ": " + Join(vs) + " against " + Join(it->second));
    }
    for (const auto &[k, vs] : b) {
        if (a.contains(k)) continue;
        return std::unexpected("declare " + k + ": absent on our side, " + Join(vs) + " on theirs");
    }
    return {};
}

} // namespace faustlens::test
