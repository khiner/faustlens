#include "signal/Type.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace faustlens {

std::vector<Variability> InferVariability(const Signals &s) {
    std::vector<Variability> var(s.Size(), Variability::Konst);

    const auto kid = [&](SigId id, uint32_t i) { return var[s.Child(id, i)]; };
    const auto all = [&](SigId id) {
        Variability v = Variability::Konst;
        for (const SigId c : s.Children(id)) v = Join(v, var[c]);
        return v;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (SigId id = 0; id < s.Size(); ++id) {
            const SigNode &n = s.Get(id);
            const Variability was = var[id];
            Variability now = was;
            switch (s.KindOf(id)) {
                case SigKind::Int:
                case SigKind::Real: now = Variability::Konst; break;
                // Not constant: one element per sample.
                case SigKind::Waveform: now = Variability::Samp; break;
                case SigKind::Input: now = Variability::Samp; break;

                // Sample rate whatever the source, and the delay index is not joined in.
                case SigKind::Delay1:
                case SigKind::Delay:
                case SigKind::Prefix: now = Variability::Samp; break;

                case SigKind::BinOp: now = Join(kid(id, 0), kid(id, 1)); break;
                case SigKind::IntCast:
                case SigKind::BitCast:
                case SigKind::FloatCast: now = kid(id, 0); break;

                case SigKind::Select2:
                case SigKind::Select3: now = all(id); break;

                case SigKind::Attach:
                case SigKind::Control:
                case SigKind::Enable: now = kid(id, 0); break;

                case SigKind::WRTbl: now = n.ChildCount == 4 ? Join(kid(id, 2), kid(id, 3)) : Variability::Konst; break;
                case SigKind::RDTbl: now = Join(kid(id, 0), kid(id, 1)); break;
                case SigKind::Gen: now = kid(id, 0); break;

                case SigKind::Button:
                case SigKind::Checkbox:
                case SigKind::VSlider:
                case SigKind::HSlider:
                case SigKind::NumEntry: now = Variability::Block; break;
                // Forwards what it meters (third child), at block rate or faster.
                case SigKind::VBargraph:
                case SigKind::HBargraph: now = Join(kid(id, 2), Variability::Block); break;

                case SigKind::Soundfile: now = Variability::Block; break;
                case SigKind::SoundfileLength:
                case SigKind::SoundfileRate: now = Join(kid(id, 1), Variability::Block); break;
                case SigKind::SoundfileBuffer: now = Variability::Samp; break;

                // A nullary `ffunction` is sample rate by assumption, modelled on `rand()`.
                case SigKind::FFun: now = n.ChildCount == 0 ? Variability::Samp : all(id); break;
                case SigKind::FConst: now = Variability::Konst; break;
                case SigKind::FVar: now = Variability::Block; break;

                case SigKind::Rec: now = all(id); break;
                case SigKind::Proj: now = var[s.Child(id, 0)]; break;

                case SigKind::Extended: {
                    switch (Ext(n.Form)) {
                        // Moves only the interval, so it takes its third child.
                        case Ext::AssertBounds: now = kid(id, 2); break;
                        // A bound is a compile-time number however the argument varies.
                        case Ext::Lowest:
                        case Ext::Highest: now = Variability::Konst; break;
                        default: now = all(id); break;
                    }
                    break;
                }

                // Poison: the top cannot make a downstream band wrong.
                case SigKind::Error: now = Variability::Samp; break;
                default: break;
            }
            if (now != was) {
                var[id] = now;
                changed = true;
            }
        }
    }
    return var;
}

namespace {

const Interval Top{};
const Interval Zero{0, 0};

double Infinity() { return std::numeric_limits<double>::infinity(); }

// The shifts and bitwise operators are `intCast`-wrapped, so they saturate where `+` and `*` do not.
Interval Arithmetic(BinOpCode b, const Interval &x, const Interval &y) {
    switch (b) {
        case BinOpCode::Add: return ivl::Add(x, y);
        case BinOpCode::Sub: return ivl::Sub(x, y);
        case BinOpCode::Mul: return ivl::Mul(x, y);
        case BinOpCode::Div: return ivl::Div(x, y);
        case BinOpCode::Rem: return ivl::Mod(x, y);
        case BinOpCode::LeftShift: return ivl::IntCast(ivl::Lsh(x, y));
        case BinOpCode::RightShift:
        case BinOpCode::LRightShift: return ivl::IntCast(ivl::Rsh(x, y));
        case BinOpCode::GT: return ivl::Gt(x, y);
        case BinOpCode::LT: return ivl::Lt(x, y);
        case BinOpCode::GE: return ivl::Ge(x, y);
        case BinOpCode::LE: return ivl::Le(x, y);
        case BinOpCode::EQ: return ivl::Eq(x, y);
        case BinOpCode::NE: return ivl::Ne(x, y);
        case BinOpCode::AND: return ivl::IntCast(ivl::And(x, y));
        case BinOpCode::OR: return ivl::IntCast(ivl::Or(x, y));
        case BinOpCode::XOR: return ivl::IntCast(ivl::Xor(x, y));
        default: return Top;
    }
}

// Indexed by `Ext`, nullptr where `ExtInterval`'s switch handles the operation.
constexpr Interval (*Unary[])(const Interval &) = {
    ivl::Abs,           ivl::Acos,
    ivl::Acosh,         ivl::Asin,
    ivl::Asinh,         ivl::Atan,
    nullptr /*Atan2*/,  ivl::Atanh,
    ivl::Ceil,          ivl::Cos,
    ivl::Cosh,          ivl::Exp,
    ivl::Floor,         nullptr /*Fmod*/,
    ivl::Log,           ivl::Log10,
    nullptr /*Max*/,    nullptr /*Min*/,
    nullptr /*Pow*/,    ivl::Remainder,
    ivl::Rint,          ivl::Round,
    ivl::Sin,           ivl::Sinh,
    ivl::Sqrt,          ivl::Tan,
    ivl::Tanh,          nullptr /*AssertBounds*/,
    nullptr /*Lowest*/, nullptr /*Highest*/,
};
static_assert(std::size(Unary) == size_t(Ext::Count_));

Interval ExtInterval(Ext e, const Signals &s, SigId id, std::span<const Interval> iv) {
    const auto k = [&](uint32_t i) { return iv[s.Child(id, i)]; };
    if (const auto f = e < Ext::Count_ ? Unary[size_t(e)] : nullptr) return f(k(0));
    switch (e) {
        case Ext::Atan2: return ivl::Atan2(k(0), k(1));
        case Ext::Fmod: return ivl::Mod(k(0), k(1));
        case Ext::Max: return ivl::Max(k(0), k(1));
        case Ext::Min: return ivl::Min(k(0), k(1));
        case Ext::Pow: return ivl::Pow(k(0), k(1));

        // The bound children are point intervals, hence `lo()` for both.
        case Ext::AssertBounds: {
            const double lo = k(0).Lo, hi = k(1).Lo;
            const Interval cur = k(2);
            if (cur.IsEmpty()) return {lo, hi};
            return {std::max(cur.Lo, lo), std::min(cur.Hi, hi)};
        }
        case Ext::Lowest: return Interval(k(0).Lo);
        case Ext::Highest: return Interval(k(0).Hi);
        default: return Top;
    }
}

} // namespace

std::vector<Interval> InferIntervals(const Signals &s) {
    std::vector<Interval> iv(s.Size(), Top);

    // Climb state per branch, keyed by group id. Group ids precede their branches, so one pass suffices.
    std::vector<std::vector<Interval>> assumed(s.Size());
    std::vector<SigId> groups;
    for (SigId id = 0; id < s.Size(); ++id) {
        if (s.KindOf(id) != SigKind::Rec || s.Get(id).ChildCount == 0) continue;
        groups.push_back(id);
        assumed[id].assign(s.Get(id).ChildCount, Zero);
    }

    bool settled = false;
    while (!settled) {
        for (SigId id = 0; id < s.Size(); ++id) {
            const SigNode &n = s.Get(id);
            const auto k = [&](uint32_t i) { return iv[s.Child(id, i)]; };
            Interval now = Top;
            switch (s.KindOf(id)) {
                case SigKind::Int: now = ivl::IntNum(s.IntValue(id)); break;
                case SigKind::Real: now = ivl::RealNum(s.RealValue(id)); break;

                case SigKind::Waveform: {
                    const std::vector<double> &w = s.WaveformAt(n.Aux);
                    for (size_t i = 0; i < w.size(); ++i) {
                        const Interval e = n.Form == 0 ? ivl::IntNum(int64_t(w[i])) : ivl::RealNum(w[i]);
                        now = i == 0 ? e : Reunion(now, e);
                    }
                    break;
                }

                case SigKind::Input: now = {-1, 1}; break;
                case SigKind::FConst:
                case SigKind::FVar:
                case SigKind::FFun: now = Top; break;

                // Zero for the samples before the line has filled. The delay amount is not read.
                case SigKind::Delay1:
                case SigKind::Delay: now = Reunion(k(0), Zero); break;
                case SigKind::Prefix: now = Reunion(k(0), k(1)); break;

                case SigKind::BinOp: now = Arithmetic(BinOpCode(n.Form), k(0), k(1)); break;
                case SigKind::IntCast: now = ivl::IntCast(k(0)); break;
                case SigKind::FloatCast: now = ivl::FloatCast(k(0)); break;
                // `bitCast` moves the nature only, unlike the `intCast` next to it.
                case SigKind::BitCast: now = k(0); break;

                // The selector is not joined in: only a branch can be the value.
                case SigKind::Select2: now = Reunion(k(1), k(2)); break;
                case SigKind::Select3: now = Reunion(Reunion(k(1), k(2)), k(3)); break;

                case SigKind::Attach:
                case SigKind::Control:
                case SigKind::Enable:
                case SigKind::Gen: now = k(0); break;

                case SigKind::WRTbl: now = n.ChildCount == 4 ? Reunion(k(1), k(3)) : k(1); break;
                case SigKind::RDTbl: now = k(0); break;

                case SigKind::Button:
                case SigKind::Checkbox: now = {0, 1, 0}; break;
                // Children are `{init, min, max, step}`.
                case SigKind::VSlider:
                case SigKind::HSlider:
                case SigKind::NumEntry: now = ivl::Slider(k(1), k(2), k(3)); break;
                // Its own bounds are a display range and do not clamp.
                case SigKind::VBargraph:
                case SigKind::HBargraph: now = k(2); break;

                case SigKind::Soundfile:
                case SigKind::SoundfileLength:
                case SigKind::SoundfileRate: now = {0, 2147483647.0}; break;
                case SigKind::SoundfileBuffer: now = {-1, 1}; break;

                case SigKind::Extended: now = ExtInterval(Ext(n.Form), s, id, iv); break;

                // The assumed value, not the branch.
                case SigKind::Proj: {
                    const SigId g = s.Child(id, 0);
                    if (n.Payload < assumed[g].size()) now = assumed[g][n.Payload];
                    break;
                }
                case SigKind::Rec: continue; // a tuplet, merged after the pass
                case SigKind::Error: now = Top; break;
                default: break;
            }
            iv[id] = now;
        }

        for (const SigId g : groups) {
            Interval merged = iv[s.Child(g, 0)];
            for (uint32_t j = 1; j < s.Get(g).ChildCount; ++j) merged = Reunion(merged, iv[s.Child(g, j)]);
            iv[g] = merged;
        }

        // The widening limit is 0, so the first move on a side goes straight to infinity.
        settled = true;
        for (const SigId g : groups) {
            for (uint32_t j = 0; j < s.Get(g).ChildCount; ++j) {
                const Interval &was = assumed[g][j];
                const Interval merged = Reunion(iv[s.Child(g, j)], was);
                double lo = merged.Lo, hi = merged.Hi;
                if (merged.Lo != was.Lo) lo = -Infinity();
                if (merged.Hi != was.Hi) hi = Infinity();
                const Interval next{lo, hi, merged.Lsb};
                // Bounds only: the resolution never converges.
                if (!(next == was)) settled = false;
                assumed[g][j] = next;
            }
        }
    }
    return iv;
}

} // namespace faustlens
