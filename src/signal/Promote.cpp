#include "signal/Promote.h"

#include "signal/Rewrite.h"
#include "signal/Simplify.h"
#include "signal/Type.h"

#include <span>

namespace faustlens {
namespace {

Nature Join(Nature a, Nature b) { return a == Nature::Real || b == Nature::Real ? Nature::Real : Nature::Int; }

bool IsBitwise(BinOpCode b) {
    switch (b) {
        case BinOpCode::AND:
        case BinOpCode::OR:
        case BinOpCode::XOR:
        case BinOpCode::LeftShift:
        case BinOpCode::RightShift:
        case BinOpCode::LRightShift: return true;
        default: return false;
    }
}

// `asin` alone among the transcendentals: `asinprim.hh` omits the `floatCast` its
// neighbours apply. A reference bug, reproduced.
bool ExtKeepsNature(Ext e) {
    switch (e) {
        case Ext::Abs:
        case Ext::Min:
        case Ext::Max:
        case Ext::Pow:
        case Ext::Asin: return true;
        default: return false;
    }
}

} // namespace

std::vector<Nature> InferNatures(const Signals &s) {
    std::vector<Nature> nat(s.Size(), Nature::Int);

    const auto kid = [&](SigId id, uint32_t i) { return nat[s.Child(id, i)]; };

    bool changed = true;
    while (changed) {
        changed = false;
        for (SigId id = 0; id < s.Size(); ++id) {
            const SigNode &n = s.Get(id);
            const Nature was = nat[id];
            Nature now = was;
            switch (s.KindOf(id)) {
                case SigKind::Int: now = Nature::Int; break;
                case SigKind::Real: now = Nature::Real; break;
                // On `Form`: `waveform{0., 1.}` and `waveform{0, 1}` store identical doubles.
                case SigKind::Waveform:
                case SigKind::FConst:
                case SigKind::FVar: now = n.Form == 0 ? Nature::Int : Nature::Real; break;
                case SigKind::Input: now = Nature::Real; break;

                case SigKind::BinOp: {
                    const BinOpCode b = BinOpCode(n.Form);
                    if (IsComparison(b) || IsBitwise(b)) now = Nature::Int;
                    else if (b == BinOpCode::Div) now = Nature::Real;
                    else now = Join(kid(id, 0), kid(id, 1));
                    break;
                }

                case SigKind::Delay1:
                case SigKind::Delay:
                case SigKind::Attach:
                case SigKind::Control:
                case SigKind::Enable:
                case SigKind::Gen: now = kid(id, 0); break;
                case SigKind::Prefix: now = Join(kid(id, 0), kid(id, 1)); break;

                case SigKind::IntCast:
                case SigKind::BitCast: now = Nature::Int; break;
                case SigKind::FloatCast: now = Nature::Real; break;

                case SigKind::Select2: now = Join(kid(id, 1), kid(id, 2)); break;
                case SigKind::Select3: now = Join(Join(kid(id, 1), kid(id, 2)), kid(id, 3)); break;

                case SigKind::WRTbl: now = kid(id, 1); break; // the generator's
                case SigKind::RDTbl: now = kid(id, 0); break;

                case SigKind::Button:
                case SigKind::Checkbox:
                case SigKind::VSlider:
                case SigKind::HSlider:
                case SigKind::NumEntry:
                case SigKind::VBargraph:
                case SigKind::HBargraph: now = Nature::Real; break;

                case SigKind::Soundfile:
                case SigKind::SoundfileLength:
                case SigKind::SoundfileRate: now = Nature::Int; break;
                case SigKind::SoundfileBuffer: now = Nature::Real; break;

                case SigKind::FFun: now = n.Form == 0 ? Nature::Int : Nature::Real; break;

                case SigKind::Extended: {
                    const Ext e = Ext(n.Form);
                    if (!ExtKeepsNature(e)) now = Nature::Real;
                    else {
                        now = kid(id, 0);
                        for (uint32_t i = 1; i < n.ChildCount; ++i) now = Join(now, kid(id, i));
                    }
                    break;
                }

                case SigKind::Proj: { // branch `Payload` of the group this reads
                    const SigId g = s.Child(id, 0);
                    if (n.Payload < s.Get(g).ChildCount) now = nat[s.Child(g, n.Payload)];
                    break;
                }
                case SigKind::Rec: now = Nature::Int; break; // a group has no nature of its own
                case SigKind::Error: now = Nature::Real; break;
                default: break;
            }
            if (now != was) {
                nat[id] = now;
                changed = true;
            }
        }
    }
    return nat;
}

namespace {

struct Promoter {
    Signals &S;
    const std::vector<Nature> &Nat;

    // `id` is the original node, which the nature table is keyed on. `p` its children.
    SigId operator()(SigId id, std::span<const SigId> p) {
        const SigNode n = S.Get(id); // both by value, see `Rewriter::Go`
        const std::vector<SigId> kids(S.Children(id).begin(), S.Children(id).end());

        switch (S.KindOf(id)) {
            case SigKind::BinOp: {
                const BinOpCode b = BinOpCode(n.Form);
                const Nature tx = NatOf(kids[0]), ty = NatOf(kids[1]);
                if (IsBitwise(b)) return S.MakeBin(b, ToInt(kids[0], p[0]), ToInt(kids[1], p[1]));
                if (b == BinOpCode::Div) // always a float
                    return S.MakeBin(b, ToFloat(kids[0], p[0]), ToFloat(kids[1], p[1]));
                if (b == BinOpCode::Rem) {
                    if (tx == Nature::Int && ty == Nature::Int) return S.MakeBin(b, p[0], p[1]);
                    return S.Make(SigKind::Extended, uint8_t(Ext::Fmod), 0, 0, {ToFloat(kids[0], p[0]), ToFloat(kids[1], p[1])});
                }
                if (tx == ty) return S.MakeBin(b, p[0], p[1]);
                return S.MakeBin(b, ToFloat(kids[0], p[0]), ToFloat(kids[1], p[1]));
            }

            case SigKind::Delay: return S.Make(SigKind::Delay, {p[0], ToInt(kids[1], p[1])});

            case SigKind::Prefix:
                if (NatOf(kids[0]) == NatOf(kids[1])) return S.Rebuild(id, p);
                return S.Make(SigKind::Prefix, {ToFloat(kids[0], p[0]), ToFloat(kids[1], p[1])});

            case SigKind::Select2: {
                const SigId sel = ToInt(kids[0], p[0]);
                if (NatOf(kids[1]) == NatOf(kids[2])) return S.Make(SigKind::Select2, {sel, p[1], p[2]});
                return S.Make(SigKind::Select2, {sel, ToFloat(kids[1], p[1]), ToFloat(kids[2], p[2])});
            }

            case SigKind::IntCast: return ToInt(kids[0], p[0]);
            case SigKind::FloatCast: return ToFloat(kids[0], p[0]);

            case SigKind::RDTbl: return S.Make(SigKind::RDTbl, {p[0], ToInt(kids[1], p[1])});

            case SigKind::WRTbl: {
                if (p.size() == 2) return S.Rebuild(id, p); // read-only, nothing to cast
                const Nature tg = NatOf(kids[1]), tw = NatOf(kids[3]);
                SigId ws = p[3];
                if (tg != tw) ws = tg == Nature::Real ? SimpFloatCast(S, ws) : SimpIntCast(S, ws);
                return S.Make(SigKind::WRTbl, {p[0], p[1], ToInt(kids[2], p[2]), ws});
            }

            case SigKind::SoundfileLength:
            case SigKind::SoundfileRate: return S.Rebuild(id, {p[0], ToInt(kids[1], p[1])});
            case SigKind::SoundfileBuffer: return S.Rebuild(id, {p[0], p[1], ToInt(kids[2], p[2]), ToInt(kids[3], p[3])});

            case SigKind::VBargraph:
            case SigKind::HBargraph: return S.Rebuild(id, {p[0], p[1], ToFloat(kids[2], p[2])});

            case SigKind::Extended: {
                const Ext e = Ext(n.Form);
                Nature want = Nature::Real;
                if (ExtKeepsNature(e)) {
                    want = NatOf(kids[0]);
                    for (size_t i = 1; i < kids.size(); ++i) want = Join(want, NatOf(kids[i]));
                }
                std::vector<SigId> cast;
                cast.reserve(kids.size());
                for (size_t i = 0; i < kids.size(); ++i) cast.push_back(want == Nature::Real ? ToFloat(kids[i], p[i]) : ToInt(kids[i], p[i]));
                return S.Rebuild(id, cast);
            }

            default: return S.Rebuild(id, p);
        }
    }

    Nature NatOf(SigId id) const { return Nat[id]; }

    SigId ToFloat(SigId old, SigId promoted) { return NatOf(old) == Nature::Real ? promoted : SimpFloatCast(S, promoted); }
    SigId ToInt(SigId old, SigId promoted) { return NatOf(old) == Nature::Int ? promoted : SimpIntCast(S, promoted); }
};

// `hi < size` and not `hi <= size - 1`: `[0, inf) % 100` comes back as `[0, nexttoward(100, 0)]`.
SigId Clamp(Signals &s, SigId i, SigId size_sig, const Interval &idx) {
    if (size_sig == NoSig || !s.IsInt(size_sig)) return i;
    const int32_t size = s.IntValue(size_sig);
    if (size <= 0) return i;
    if (!idx.IsEmpty() && idx.Lo >= 0 && idx.Hi < size) return i;
    const SigId lo = SimpExtended(s, Ext::Min, {i, s.MakeInt(size - 1)});
    return SimpExtended(s, Ext::Max, {s.MakeInt(0), lo});
}

} // namespace

std::vector<SigId> Promote(Signals &s, std::span<const SigId> roots) {
    const std::vector<Nature> nat = InferNatures(s);
    return Rewrite(s, roots, Promoter{s, nat});
}

std::vector<SigId> ClampTables(Signals &s, std::span<const SigId> roots) {
    // Before the rewrite appends, so the clamps added here are not themselves indices to prove.
    const std::vector<Interval> iv = InferIntervals(s);
    return Rewrite(s, roots, [&s, &iv](SigId id, std::span<const SigId> k) {
        switch (s.KindOf(id)) {
            case SigKind::RDTbl: {
                const SigId size = s.KindOf(k[0]) == SigKind::WRTbl ? s.Child(k[0], 0) : NoSig;
                return s.Make(SigKind::RDTbl, {k[0], Clamp(s, k[1], size, iv[s.Child(id, 1)])});
            }
            case SigKind::WRTbl:
                if (k.size() != 4) break; // read-only, no write index to clamp
                return s.Make(SigKind::WRTbl, {k[0], k[1], Clamp(s, k[2], k[0], iv[s.Child(id, 2)]), k[3]});
            default: break;
        }
        return s.Rebuild(id, k);
    });
}

} // namespace faustlens
