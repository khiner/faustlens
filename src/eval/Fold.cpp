#include "eval/Fold.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <span>

namespace faustlens {
namespace {

Num Join(const Num &a, const Num &b, int32_t (*fi)(int32_t, int32_t), double (*fd)(double, double)) {
    if (a.IsInt && b.IsInt) return Num::Int(fi(a.I, b.I));
    return Num::Real(fd(a.AsDouble(), b.AsDouble()));
}

Num FoldPow(const Num &a, const Num &b) {
    if (a.IsInt && b.IsInt && b.I > 0) {
        int32_t r = 1;
        for (int32_t k = 0; k < b.I; ++k) r *= a.I;
        return Num::Int(r);
    }
    const double r = std::pow(a.AsDouble(), b.AsDouble());
    return (a.IsInt && b.IsInt) ? Num::Int(ToInt(r)) : Num::Real(r);
}

Num Compare(bool v) { return Num::Int(v ? 1 : 0); }

} // namespace

int32_t ToInt(double x) {
    if (std::isnan(x)) return 0;
    return int32_t(std::min(2147483647.0, std::max(x, -2147483648.0)));
}

std::optional<Num> ApplyPrim(Prim p, std::span<const Num> a) {
    if (a.size() != PrimArity(p)) return std::nullopt;
    switch (p) {
        case Prim::Wire: return a[0];
        case Prim::IntCast: return Num::Int(a[0].AsInt());
        case Prim::FloatCast: return Num::Real(a[0].AsDouble());

        case Prim::Add: return Join(a[0], a[1], [](int32_t x, int32_t y) { return x + y; }, [](double x, double y) { return x + y; });
        case Prim::Sub: return Join(a[0], a[1], [](int32_t x, int32_t y) { return x - y; }, [](double x, double y) { return x - y; });
        case Prim::Mul: return Join(a[0], a[1], [](int32_t x, int32_t y) { return x * y; }, [](double x, double y) { return x * y; });
        // "Division always yields a float" is a *type* rule, not a folding rule: an exact
        // division keeps its integer here, or `hadamard(n/2)` never reaches its base case.
        case Prim::Div: {
            if (a[1].AsDouble() == 0) return std::nullopt;
            if (!a[0].IsInt || !a[1].IsInt) return Num::Real(a[0].AsDouble() / a[1].AsDouble());
            const int32_t q = a[0].I / a[1].I;
            const double d = double(a[0].I) / double(a[1].I);
            return double(q) == d ? Num::Int(q) : Num::Real(d);
        }
        case Prim::Mod:
            if (a[1].AsDouble() == 0) return std::nullopt;
            return Join(a[0], a[1], [](int32_t x, int32_t y) { return x % y; }, [](double x, double y) { return std::fmod(x, y); });

        case Prim::And: return Num::Int(a[0].AsInt() & a[1].AsInt());
        case Prim::Or: return Num::Int(a[0].AsInt() | a[1].AsInt());
        case Prim::Xor: return Num::Int(a[0].AsInt() ^ a[1].AsInt());
        case Prim::Lsh: return Num::Int(int32_t(uint32_t(a[0].AsInt()) << (a[1].AsInt() & 31)));
        case Prim::Rsh: return Num::Int(a[0].AsInt() >> (a[1].AsInt() & 31));

        case Prim::Lt: return Compare(a[0].AsDouble() < a[1].AsDouble());
        case Prim::Le: return Compare(a[0].AsDouble() <= a[1].AsDouble());
        case Prim::Gt: return Compare(a[0].AsDouble() > a[1].AsDouble());
        case Prim::Ge: return Compare(a[0].AsDouble() >= a[1].AsDouble());
        case Prim::Eq: return Compare(a[0].AsDouble() == a[1].AsDouble());
        case Prim::Ne: return Compare(a[0].AsDouble() != a[1].AsDouble());

        case Prim::Acos: return Num::Real(std::acos(a[0].AsDouble()));
        case Prim::Asin: return Num::Real(std::asin(a[0].AsDouble()));
        case Prim::Atan: return Num::Real(std::atan(a[0].AsDouble()));
        case Prim::Atan2: return Num::Real(std::atan2(a[0].AsDouble(), a[1].AsDouble()));
        case Prim::Cos: return Num::Real(std::cos(a[0].AsDouble()));
        case Prim::Sin: return Num::Real(std::sin(a[0].AsDouble()));
        case Prim::Tan: return Num::Real(std::tan(a[0].AsDouble()));
        case Prim::Exp: return Num::Real(std::exp(a[0].AsDouble()));
        case Prim::Log: return Num::Real(std::log(a[0].AsDouble()));
        case Prim::Log10: return Num::Real(std::log10(a[0].AsDouble()));
        case Prim::Sqrt: return Num::Real(std::sqrt(a[0].AsDouble()));
        case Prim::Pow: return FoldPow(a[0], a[1]);

        case Prim::Abs: return a[0].IsInt ? Num::Int(std::abs(a[0].I)) : Num::Real(std::fabs(a[0].D));
        case Prim::Min: return Join(a[0], a[1], [](int32_t x, int32_t y) { return std::min(x, y); }, [](double x, double y) { return std::min(x, y); });
        case Prim::Max: return Join(a[0], a[1], [](int32_t x, int32_t y) { return std::max(x, y); }, [](double x, double y) { return std::max(x, y); });
        case Prim::Fmod: return Join(a[0], a[1], [](int32_t x, int32_t y) { return y == 0 ? 0 : x % y; }, [](double x, double y) { return std::fmod(x, y); });
        case Prim::Remainder:
            return Join(
                a[0], a[1], [](int32_t x, int32_t y) { return y == 0 ? 0 : int32_t(std::remainder(x, y)); },
                [](double x, double y) { return std::remainder(x, y); }
            );

        // Rounding folds to a *double* whatever it was given, unlike `abs`.
        case Prim::Floor: return Num::Real(std::floor(a[0].AsDouble()));
        case Prim::Ceil: return Num::Real(std::ceil(a[0].AsDouble()));
        case Prim::Rint: return Num::Real(std::rint(a[0].AsDouble()));
        case Prim::Round: return Num::Real(std::round(a[0].AsDouble()));

        // Not functions of their arguments alone. `@0` is the one exception, read as the identity.
        case Prim::FDelay:
            if (a[1].IsInt && a[1].I == 0) return a[0];
            return std::nullopt;

        case Prim::Cut:
        case Prim::Mem:
        case Prim::Prefix:
        case Prim::Attach:
        case Prim::Enable:
        case Prim::Control: return std::nullopt;

        case Prim::Select2:
            if (!a[0].IsInt) return std::nullopt;
            if (a[0].I == 0) return a[1];
            if (a[0].I == 1) return a[2];
            return std::nullopt;
        case Prim::Select3:
            if (!a[0].IsInt || a[0].I < 0 || a[0].I > 2) return std::nullopt;
            return a[1 + a[0].I];

        case Prim::RdTable:
        case Prim::RwTable:
        case Prim::AssertBounds:
        case Prim::Lowest:
        case Prim::Highest:
        case Prim::Count_: return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::vector<Num>> FoldOutputs(const Boxes &b, BoxId id, std::span<const Num> inputs) {
    const Arity &ar = b.ArityOf(id);
    if (!ar.Known || size_t(ar.Ins) != inputs.size()) return std::nullopt;
    const auto kids = b.Children(id);
    switch (b.KindOf(id)) {
        case BoxKind::Int: return std::vector<Num>{Num::Int(b.IntValue(id))};
        case BoxKind::Real: return std::vector<Num>{Num::Real(b.RealValue(id))};
        case BoxKind::Wire: return std::vector<Num>(inputs.begin(), inputs.end());
        case BoxKind::Cut: return std::vector<Num>{};
        case BoxKind::Prim: {
            const auto v = ApplyPrim(Prim(b.Get(id).Payload), inputs);
            if (!v) return std::nullopt;
            return std::vector<Num>{*v};
        }
        case BoxKind::Group: return FoldOutputs(b, kids[0], inputs);
        case BoxKind::Seq: {
            const auto mid = FoldOutputs(b, kids[0], inputs);
            if (!mid) return std::nullopt;
            return FoldOutputs(b, kids[1], *mid);
        }
        case BoxKind::Par: {
            const int32_t split = b.ArityOf(kids[0]).Ins;
            const std::vector<Num> left(inputs.begin(), inputs.begin() + split);
            const std::vector<Num> right(inputs.begin() + split, inputs.end());
            auto a = FoldOutputs(b, kids[0], left);
            if (!a) return std::nullopt;
            const auto c = FoldOutputs(b, kids[1], right);
            if (!c) return std::nullopt;
            a->insert(a->end(), c->begin(), c->end());
            return a;
        }
        case BoxKind::Split: {
            // Replicates modulo the input count.
            const auto a = FoldOutputs(b, kids[0], inputs);
            if (!a) return std::nullopt;
            std::vector<Num> fan;
            const auto n = size_t(b.ArityOf(kids[1]).Ins);
            fan.reserve(n);
            for (size_t k = 0; k < n; ++k) fan.push_back((*a)[k % a->size()]);
            return FoldOutputs(b, kids[1], fan);
        }
        case BoxKind::Merge: {
            // Output bus `k` is the sum of inputs `k`, `k + n`, `k + 2n`, ...
            const auto a = FoldOutputs(b, kids[0], inputs);
            if (!a) return std::nullopt;
            const auto n = size_t(b.ArityOf(kids[1]).Ins);
            std::vector<Num> summed(n, Num::Int(0));
            for (size_t j = 0; j < a->size(); ++j) {
                const auto add = ApplyPrim(Prim::Add, std::array{summed[j % n], (*a)[j]});
                if (!add) return std::nullopt;
                summed[j % n] = *add;
            }
            return FoldOutputs(b, kids[1], summed);
        }
        case BoxKind::Route: {
            // 1-based. Out-of-range entries drop, shared destinations sum, unconnected reads 0.
            const RouteTable &r = b.RouteAt(b.Get(id).Aux - 1);
            std::vector<Num> outs(size_t(r.Outs), Num::Int(0));
            for (size_t k = 0; k + 1 < r.Pairs.size(); k += 2) {
                const int32_t src = r.Pairs[k], dst = r.Pairs[k + 1];
                if (src < 1 || src > r.Ins || dst < 1 || dst > r.Outs) continue;
                const auto add = ApplyPrim(Prim::Add, std::array{outs[dst - 1], inputs[src - 1]});
                if (!add) return std::nullopt;
                outs[dst - 1] = *add;
            }
            return outs;
        }
        default: return std::nullopt;
    }
}

std::optional<Num> FoldConstant(const Boxes &b, BoxId id) {
    const Arity &ar = b.ArityOf(id);
    if (!ar.Known || ar.Ins != 0 || ar.Outs != 1) return std::nullopt;
    const auto outs = FoldOutputs(b, id, {});
    if (!outs || outs->size() != 1) return std::nullopt;
    return (*outs)[0];
}

} // namespace faustlens
