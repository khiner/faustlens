#include "signal/Simplify.h"

#include "signal/NormalForm.h"
#include "signal/Promote.h"
#include "signal/Rewrite.h"

#include <cmath>
#include <span>
#include <utility>

namespace faustlens {
namespace {

bool IsIntNode(const Signals &s, SigId id) { return s.KindOf(id) == SigKind::Int; }

bool IsNegativeNum(const Signals &s, SigId id) { return IsNum(s, id) && NumOf(s, id) < 0; }

int32_t AsI(const Signals &s, SigId id) { return IsIntNode(s, id) ? s.IntValue(id) : int32_t(s.RealValue(id)); }

enum class Ntrl : uint8_t { None, Zero, One, MinusOne };

struct OpAlgebra {
    Ntrl LeftNeutral, RightNeutral, LeftAbsorbing, RightAbsorbing;
};

constexpr OpAlgebra Algebra[] = {
    /* + */ {Ntrl::Zero, Ntrl::Zero, Ntrl::None, Ntrl::None},
    /* - */ {Ntrl::None, Ntrl::Zero, Ntrl::None, Ntrl::None},
    /* * */ {Ntrl::One, Ntrl::One, Ntrl::Zero, Ntrl::Zero},
    /* / */ {Ntrl::None, Ntrl::One, Ntrl::None, Ntrl::None},
    /* % */ {Ntrl::None, Ntrl::None, Ntrl::None, Ntrl::None},
    /* <<*/ {Ntrl::None, Ntrl::Zero, Ntrl::None, Ntrl::None},
    /* >>*/ {Ntrl::None, Ntrl::Zero, Ntrl::None, Ntrl::None},
    /*>>>*/ {Ntrl::None, Ntrl::Zero, Ntrl::None, Ntrl::None},
    /* > */ {Ntrl::None, Ntrl::None, Ntrl::None, Ntrl::None},
    /* < */ {Ntrl::None, Ntrl::None, Ntrl::None, Ntrl::None},
    /* >=*/{Ntrl::None, Ntrl::None, Ntrl::None, Ntrl::None},
    /* <=*/{Ntrl::None, Ntrl::None, Ntrl::None, Ntrl::None},
    /* ==*/{Ntrl::None, Ntrl::None, Ntrl::None, Ntrl::None},
    /* !=*/{Ntrl::None, Ntrl::None, Ntrl::None, Ntrl::None},
    /* & */ {Ntrl::MinusOne, Ntrl::MinusOne, Ntrl::Zero, Ntrl::Zero},
    /* | */ {Ntrl::Zero, Ntrl::Zero, Ntrl::MinusOne, Ntrl::MinusOne},
    /* ^ */ {Ntrl::None, Ntrl::None, Ntrl::None, Ntrl::None},
};
static_assert(std::size(Algebra) == size_t(BinOpCode::Count_));

bool Holds(const Signals &s, Ntrl n, SigId id) {
    switch (n) {
        case Ntrl::Zero: return IsZeroNum(s, id);
        case Ntrl::One: return IsOneNum(s, id);
        case Ntrl::MinusOne: return IsMinusOne(s, id);
        default: return false;
    }
}

// Already known to be 0 or 1, which is what makes `1 & b -> b` sound.
bool IsBool(const Signals &s, SigId id) { return s.KindOf(id) == SigKind::BinOp && IsComparison(BinOpCode(s.Get(id).Form)); }

bool BothInt(const Signals &s, SigId a, SigId b) { return IsIntNode(s, a) && IsIntNode(s, b); }

// `/` keeps an integer where exact, which the recursive standard library needs.
SigId Compute(Signals &s, BinOpCode op, SigId x, SigId y) {
    const bool ints = BothInt(s, x, y);
    const double a = NumOf(s, x), b = NumOf(s, y);
    switch (op) {
        case BinOpCode::Add: return ints ? s.MakeInt(s.IntValue(x) + s.IntValue(y)) : s.MakeReal(a + b);
        case BinOpCode::Sub: return ints ? s.MakeInt(s.IntValue(x) - s.IntValue(y)) : s.MakeReal(a - b);
        case BinOpCode::Mul: return ints ? s.MakeInt(s.IntValue(x) * s.IntValue(y)) : s.MakeReal(a * b);
        case BinOpCode::Div: {
            if (b == 0) return NoSig; // decline rather than invent an infinity
            if (!ints) return s.MakeReal(a / b);
            const int32_t q = s.IntValue(x) / s.IntValue(y);
            return double(q) == a / b ? s.MakeInt(q) : s.MakeReal(a / b);
        }
        case BinOpCode::Rem:
            if (b == 0) return NoSig;
            return ints ? s.MakeInt(s.IntValue(x) % s.IntValue(y)) : s.MakeReal(std::fmod(a, b));
        case BinOpCode::LeftShift: return s.MakeInt(int32_t(uint32_t(AsI(s, x)) << (AsI(s, y) & 31)));
        case BinOpCode::RightShift: return s.MakeInt(AsI(s, x) >> (AsI(s, y) & 31));
        case BinOpCode::LRightShift: return s.MakeInt(int32_t(uint32_t(AsI(s, x)) >> (AsI(s, y) & 31)));
        case BinOpCode::GT: return s.MakeInt(a > b);
        case BinOpCode::LT: return s.MakeInt(a < b);
        case BinOpCode::GE: return s.MakeInt(a >= b);
        case BinOpCode::LE: return s.MakeInt(a <= b);
        case BinOpCode::EQ: return s.MakeInt(a == b);
        case BinOpCode::NE: return s.MakeInt(a != b);
        case BinOpCode::AND: return s.MakeInt(AsI(s, x) & AsI(s, y));
        case BinOpCode::OR: return s.MakeInt(AsI(s, x) | AsI(s, y));
        case BinOpCode::XOR: return s.MakeInt(AsI(s, x) ^ AsI(s, y));
        default: return NoSig;
    }
}

SigId Negate(Signals &s, SigId n) { return IsIntNode(s, n) ? s.MakeInt(-s.IntValue(n)) : s.MakeReal(-s.RealValue(n)); }

} // namespace

bool IsNum(const Signals &s, SigId id) {
    const SigKind k = s.KindOf(id);
    return k == SigKind::Int || k == SigKind::Real;
}

bool IsBinOp(const Signals &s, SigId id, BinOpCode op) { return s.KindOf(id) == SigKind::BinOp && BinOpCode(s.Get(id).Form) == op; }

// Every rewrite ends in `Raw`, never a recursive `SimpBinOp`. The reference takes
// no fixpoint, so one pass leaves `-1*(-1*x)` standing.
SigId SimpBinOp(Signals &s, BinOpCode op, SigId x, SigId y) {
    if (IsNum(s, x) && IsNum(s, y)) {
        const SigId v = Compute(s, op, x, y);
        if (v != NoSig) return v;
        return s.MakeBin(op, x, y);
    }

    if (op == BinOpCode::Mul)
        for (const auto [n, d] : {std::pair{x, y}, std::pair{y, x}})
            if (IsNegativeNum(s, n) && IsBinOp(s, d, BinOpCode::Sub)) {
                const SigId v1 = s.Child(d, 0), v2 = s.Child(d, 1);
                if (IsMinusOne(s, n)) return s.MakeBin(BinOpCode::Sub, v2, v1);
                return s.MakeBin(BinOpCode::Mul, Negate(s, n), s.MakeBin(BinOpCode::Sub, v2, v1));
            }

    if (op == BinOpCode::Mul && IsNum(s, x) && IsBinOp(s, y, BinOpCode::Mul))
        for (const auto [v1, v2] : {std::pair{s.Child(y, 0), s.Child(y, 1)}, std::pair{s.Child(y, 1), s.Child(y, 0)}})
            if (IsNum(s, v1)) {
                const SigId m = Compute(s, BinOpCode::Mul, x, v1);
                return IsOneNum(s, m) ? v2 : s.MakeBin(BinOpCode::Mul, m, v2);
            }

    if (op == BinOpCode::Sub && IsZeroNum(s, x)) return s.MakeBin(BinOpCode::Mul, s.MakeInt(-1), y);

    const OpAlgebra &alg = Algebra[size_t(op)];
    if (Holds(s, alg.LeftNeutral, x)) return y;
    if (Holds(s, alg.LeftAbsorbing, x)) return x;
    if (Holds(s, alg.RightNeutral, y)) return x;
    if (Holds(s, alg.RightAbsorbing, y)) return y;

    if (x == y) {
        switch (op) {
            case BinOpCode::AND:
            case BinOpCode::OR: return x;
            case BinOpCode::GE:
            case BinOpCode::LE:
            case BinOpCode::EQ: return s.MakeInt(1);
            case BinOpCode::GT:
            case BinOpCode::LT:
            case BinOpCode::NE:
            case BinOpCode::Rem:
            case BinOpCode::XOR: return s.MakeInt(0);
            default: break;
        }
    } else if (op == BinOpCode::AND || op == BinOpCode::OR) {
        if (IsOneNum(s, x) && IsBool(s, y)) return op == BinOpCode::AND ? y : s.MakeInt(1);
        if (IsOneNum(s, y) && IsBool(s, x)) return op == BinOpCode::AND ? x : s.MakeInt(1);
    }

    return s.MakeBin(op, x, y);
}

SigId SimpIntCast(Signals &s, SigId x) {
    if (IsIntNode(s, x)) return x;
    if (s.KindOf(x) == SigKind::Real) return s.MakeInt(int32_t(s.RealValue(x)));
    return s.Make(SigKind::IntCast, {x});
}

SigId SimpFloatCast(Signals &s, SigId x) {
    if (IsIntNode(s, x)) return s.MakeReal(double(s.IntValue(x)));
    if (s.KindOf(x) == SigKind::Real) return x;
    return s.Make(SigKind::FloatCast, {x});
}

SigId SimpSelect2(Signals &s, SigId sel, SigId a, SigId b) {
    if (IsZeroNum(s, sel)) return a;
    if (IsNum(s, sel)) return b;
    if (a == b) return a;
    return s.Make(SigKind::Select2, {sel, a, b});
}

SigId SimpControl(Signals &s, SigId x, SigId cond) {
    if (IsZeroNum(s, cond)) return s.MakeInt(0);
    if (IsOneNum(s, cond)) return x;
    return s.Make(SigKind::Control, {x, cond});
}

SigId SimpDelay(Signals &s, SigId x, SigId d) {
    if (IsZeroNum(s, d)) {
        // A group's `@0` is what puts a recursive output on the current sample.
        if (s.KindOf(x) == SigKind::Proj) return s.Make(SigKind::Delay, {x, d});
        return x;
    }
    if (IsZeroNum(s, x)) return x;

    // Only where the operand staying put is constant, or it would be read at the wrong time.
    if (s.KindOf(x) == SigKind::BinOp) {
        const BinOpCode b = BinOpCode(s.Get(x).Form);
        const SigId u = s.Child(x, 0), v = s.Child(x, 1);
        if (b == BinOpCode::Mul) {
            if (s.Order(u) < 2) return s.MakeBin(BinOpCode::Mul, u, SimpDelay(s, v, d));
            if (s.Order(v) < 2) return s.MakeBin(BinOpCode::Mul, v, SimpDelay(s, u, d));
        } else if (b == BinOpCode::Div && s.Order(v) < 2) {
            return s.MakeBin(BinOpCode::Div, SimpDelay(s, u, d), v);
        }
    }

    // The sum puts the outer delay first, not the order the rule suggests.
    if (s.KindOf(x) == SigKind::Delay && s.Order(s.Child(x, 1)) < 2) return SimpDelay(s, s.Child(x, 0), SimpBinOp(s, BinOpCode::Add, d, s.Child(x, 1)));

    return s.Make(SigKind::Delay, {x, d});
}

SigId SimpDelay1(Signals &s, SigId x) { return SimpDelay(s, x, s.MakeInt(1)); }

SigId SimpExtended(Signals &s, Ext e, std::span<const SigId> args) {
    // At the signal layer, so these reach `.sig`. The codegen layer's rewrites do not.
    if (e == Ext::Pow && args.size() == 2 && IsNum(s, args[1]) && !IsNum(s, args[0])) {
        const double x = NumOf(s, args[1]);
        if (x == 0.0) return s.MakeReal(1.0);
        if (x == 1.0) return args[0];
        if (x == 0.5) return SimpExtended(s, Ext::Sqrt, {args[0]});
        if (x == 0.25) return SimpExtended(s, Ext::Sqrt, {SimpExtended(s, Ext::Sqrt, {args[0]})});
    }
    if (e == Ext::Pow && args.size() == 2 && IsNum(s, args[0]) && IsNum(s, args[1]) && IsIntNode(s, args[0]) && IsIntNode(s, args[1]) &&
        s.IntValue(args[1]) > 0) {
        int32_t r = 1;
        for (int32_t i = 0; i < s.IntValue(args[1]); ++i) r *= s.IntValue(args[0]);
        return s.MakeInt(r);
    }

    bool all_num = !args.empty();
    for (const SigId a : args) all_num = all_num && IsNum(s, a);
    if (all_num) {
        const double a = NumOf(s, args[0]);
        const double b = args.size() > 1 ? NumOf(s, args[1]) : 0;
        const bool ints = args.size() > 1 ? BothInt(s, args[0], args[1]) : IsIntNode(s, args[0]);
        switch (e) {
            case Ext::Abs: return ints ? s.MakeInt(std::abs(s.IntValue(args[0]))) : s.MakeReal(std::fabs(a));
            case Ext::Min: return ints ? s.MakeInt(int32_t(std::min(a, b))) : s.MakeReal(std::min(a, b));
            case Ext::Max: return ints ? s.MakeInt(int32_t(std::max(a, b))) : s.MakeReal(std::max(a, b));
            case Ext::Pow: {
                const double p = std::pow(a, b);
                return ints ? s.MakeInt(int32_t(p)) : s.MakeReal(p);
            }
            case Ext::Acos: return s.MakeReal(std::acos(a));
            case Ext::Asin: return ints ? s.MakeInt(int32_t(std::asin(a))) : s.MakeReal(std::asin(a));
            case Ext::Atan: return s.MakeReal(std::atan(a));
            case Ext::Atan2: return s.MakeReal(std::atan2(a, b));
            case Ext::Cos: return s.MakeReal(std::cos(a));
            case Ext::Sin: return s.MakeReal(std::sin(a));
            case Ext::Tan: return s.MakeReal(std::tan(a));
            case Ext::Exp: return s.MakeReal(std::exp(a));
            case Ext::Log: return s.MakeReal(std::log(a));
            case Ext::Log10: return s.MakeReal(std::log10(a));
            case Ext::Sqrt: return s.MakeReal(std::sqrt(a));
            case Ext::Floor: return s.MakeReal(std::floor(a));
            case Ext::Ceil: return s.MakeReal(std::ceil(a));
            case Ext::Rint: return s.MakeReal(std::rint(a));
            case Ext::Round: return s.MakeReal(std::round(a));
            case Ext::Fmod: return s.MakeReal(std::fmod(a, b));
            case Ext::Remainder: return s.MakeReal(std::remainder(a, b));
            default: break;
        }
    }
    return s.Make(SigKind::Extended, uint8_t(e), 0, 0, args);
}

namespace {

// Comparing shape builds nothing, where interning a node to compare would leave one abandoned.
bool Rewrote(const Signals &s, SigId r, uint8_t form, SigId x, SigId y) {
    if (s.KindOf(r) != SigKind::BinOp || s.Get(r).Form != form) return true;
    return s.Get(r).ChildCount != 2 || s.Child(r, 0) != x || s.Child(r, 1) != y;
}

} // namespace

std::vector<SigId> Simplify(Signals &s, std::span<const SigId> roots, bool add_normal_form) {
    return Rewrite(s, roots, [&s, add_normal_form](SigId id, std::span<const SigId> k) {
        const SigNode n = s.Get(id); // by value, see `Rewriter::Go`
        switch (s.KindOf(id)) {
            case SigKind::BinOp: {
                const SigId r = SimpBinOp(s, BinOpCode(n.Form), k[0], k[1]);
                if (!add_normal_form || Rewrote(s, r, n.Form, k[0], k[1])) return r;
                return NormalizeAddTerm(s, r);
            }
            case SigKind::Delay1: return SimpDelay1(s, k[0]);
            case SigKind::Delay: return SimpDelay(s, k[0], k[1]);
            case SigKind::IntCast: return SimpIntCast(s, k[0]);
            case SigKind::FloatCast: return SimpFloatCast(s, k[0]);
            case SigKind::Select2: return SimpSelect2(s, k[0], k[1], k[2]);
            case SigKind::Control: return SimpControl(s, k[0], k[1]);
            case SigKind::Extended: {
                const Ext e = Ext(n.Form);
                const SigId r = SimpExtended(s, e, k);
                // Not behind the option, which is how `x^-2` becomes `1.0/x^2`.
                return e == Ext::Pow ? NormalizeAddTerm(s, r) : r;
            }
            default: return s.Rebuild(id, k);
        }
    });
}

std::vector<SigId> Normalize(Signals &s, std::span<const SigId> roots, bool add_normal_form) {
    const std::vector<SigId> promoted = Promote(s, roots);
    return ClampTables(s, Promote(s, Simplify(s, promoted, add_normal_form)));
}

} // namespace faustlens
