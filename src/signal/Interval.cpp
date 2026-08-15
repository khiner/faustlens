#include "signal/Interval.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <numbers>
#include <tuple>
#include <utility>

namespace faustlens {

namespace {

constexpr double Inf = HUGE_VAL;

int SaturatedIntCast(double d) { return int(std::min(2147483647.0, std::max(d, -2147483648.0))); }

double MinValAbs(const Interval &x) { return std::abs(x.Lo) < std::abs(x.Hi) ? x.Lo : x.Hi; }
double MaxValAbs(const Interval &x) { return std::abs(x.Lo) < std::abs(x.Hi) ? x.Hi : x.Lo; }
int SignMinValAbs(const Interval &x) { return std::abs(x.Lo) < std::abs(x.Hi) ? 1 : -1; }
int SignMaxValAbs(const Interval &x) { return std::abs(x.Lo) < std::abs(x.Hi) ? -1 : 1; }

// The bound nearest zero and the sign of the step off it, zero if the interval straddles.
std::pair<double, int> NearZero(const Interval &x) {
    if (x.HasZero()) return {0.0, 1};
    return {MinValAbs(x), SignMinValAbs(x)};
}

// Returns `INT_MIN` when it cannot bound, and every caller tests for it.
int ExactPrecisionUnary(double (*f)(double), long double x, long double u) {
    const double d = std::abs((f(double(x + u)) - f(double(x))));
    if (!(d > 0) || !std::isfinite(d)) return INT_MIN;
    const double r = std::floor(std::log2(d));
    if (!std::isfinite(r)) return INT_MIN;
    return int(r);
}

int Precision(double (*f)(double), long double x, long double u, int fallback) {
    const int p = ExactPrecisionUnary(f, x, u);
    return p == INT_MIN ? fallback : p;
}

// `inf * 0` is 0 here, not NaN.
double SpecialMult(double a, double b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }

// Spelled out rather than cast: infinities and NaNs do reach here, and casting one is UB.
int ToInt(double v) {
    if (std::isnan(v)) return 0;
    if (v >= double(INT_MAX)) return INT_MAX;
    if (v <= double(INT_MIN)) return INT_MIN;
    return int(v);
}

// Deliberate two's complement wrap, without the signed overflow UB.
int WrapAdd(int a, int b) { return int(uint32_t(a) + uint32_t(b)); }

int Twice(int a) { return WrapAdd(a, a); }

int Taylor(double v) { return ToInt(std::floor(v)); }

struct UItv {
    unsigned Lo, Hi;
};
struct SItv {
    int Lo, Hi;
};

constexpr UItv UEmpty{UINT_MAX, 0};
constexpr SItv SEmpty{INT_MAX, INT_MIN};

bool IsEmptyU(const UItv &i) { return i.Lo > i.Hi; }
bool IsEmptyS(const SItv &i) { return i.Lo > i.Hi; }

UItv UnionU(const UItv &a, const UItv &b) {
    if (IsEmptyU(a)) return b;
    if (IsEmptyU(b)) return a;
    return {std::min(a.Lo, b.Lo), std::max(a.Hi, b.Hi)};
}

std::pair<UItv, UItv> SignSplit(const SItv &x) {
    if (IsEmptyS(x)) return {UEmpty, UEmpty};
    if (x.Hi < 0) return {{unsigned(x.Lo), unsigned(x.Hi)}, UEmpty};
    if (x.Lo >= 0) return {UEmpty, {unsigned(x.Lo), unsigned(x.Hi)}};
    return {{unsigned(x.Lo), unsigned(-1)}, {0u, unsigned(x.Hi)}};
}

SItv SignMerge(const UItv &np, const UItv &pp) {
    if (IsEmptyU(np)) return IsEmptyU(pp) ? SEmpty : SItv{int(pp.Lo), int(pp.Hi)};
    if (IsEmptyU(pp)) return {int(np.Lo), int(np.Hi)};
    return {int(np.Lo), int(pp.Hi)};
}

UItv NotU(const UItv &a) { return {~a.Hi, ~a.Lo}; }
SItv NotS(const SItv &a) { return {~a.Hi, ~a.Lo}; }

UItv Unoffset(const UItv &a, unsigned d) { return {a.Lo - d, a.Hi - d}; }

bool ContainsU(const UItv &i, unsigned x) { return i.Lo <= x && x <= i.Hi; }

unsigned Msb32(unsigned x) {
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x & ~(x >> 1);
}

std::tuple<unsigned, UItv, UItv> SplitInterval(const UItv &x) {
    if (x.Lo == 0 && x.Hi == 0) return {0, {1, 0}, x};
    const unsigned m = Msb32(x.Hi);
    if (m <= x.Lo) return {m, {1, 0}, x};
    return {m, {x.Lo, m - 1}, {m, x.Hi}};
}

unsigned HiOr2(UItv a, UItv b) {
    if (a.Lo == 0 && a.Hi == 0) return b.Hi;
    if (b.Lo == 0 && b.Hi == 0) return a.Hi;
    const auto [ma, a0, a1] = SplitInterval(a);
    const auto [mb, b0, b1] = SplitInterval(b);
    if (a.Hi == 2 * ma - 1 || b.Hi == 2 * mb - 1) return a.Hi | b.Hi;
    if (mb > ma) {
        if (ContainsU(a, mb - 1)) return 2 * mb - 1;
        return HiOr2(Unoffset(b1, mb), a) + mb;
    }
    if (ma > mb) {
        if (ContainsU(b, ma - 1)) return 2 * ma - 1;
        return HiOr2(Unoffset(a1, ma), b) + ma;
    }
    if (IsEmptyU(a0) && IsEmptyU(b0)) return HiOr2(Unoffset(a1, ma), Unoffset(b1, ma)) + ma;
    if (IsEmptyU(a0)) return std::max(HiOr2(Unoffset(a1, ma), Unoffset(b1, ma)), HiOr2(Unoffset(a1, ma), b0)) + ma;
    if (IsEmptyU(b0)) return std::max(HiOr2(Unoffset(a1, ma), Unoffset(b1, ma)), HiOr2(a0, Unoffset(b1, ma))) + ma;
    return std::max({HiOr2(Unoffset(a1, ma), Unoffset(b1, ma)), HiOr2(Unoffset(a1, ma), b0), HiOr2(a0, Unoffset(b1, ma))}) + ma;
}

unsigned LoOr2(UItv a, UItv b) {
    if (IsEmptyU(a) || IsEmptyU(b)) return 0;
    if (a.Lo == 0) return b.Lo;
    if (b.Lo == 0) return a.Lo;
    const auto [ma, a0, a1] = SplitInterval(a);
    const auto [mb, b0, b1] = SplitInterval(b);
    if (ma > mb) {
        if (IsEmptyU(a0)) return LoOr2(Unoffset(a1, ma), b) | ma;
        return LoOr2(a0, b);
    }
    if (mb > ma) {
        if (IsEmptyU(b0)) return LoOr2(a, Unoffset(b1, mb)) | mb;
        return LoOr2(a, b0);
    }
    if (!IsEmptyU(a0) && !IsEmptyU(b0)) return LoOr2(a0, b0);
    if (IsEmptyU(a0) && IsEmptyU(b0)) return LoOr2(Unoffset(a1, ma), Unoffset(b1, ma)) | ma;
    if (IsEmptyU(a0)) return std::min(LoOr2(Unoffset(a1, ma), b0) | ma, LoOr2(Unoffset(a1, ma), Unoffset(b1, ma)) | ma);
    return std::min(LoOr2(a0, Unoffset(b1, mb)) | mb, LoOr2(Unoffset(a1, ma), Unoffset(b1, ma)) | ma);
}

UItv OrU(const UItv &a, const UItv &b) {
    if (a.Lo == 0 && a.Hi == 0) return b;
    if (b.Lo == 0 && b.Hi == 0) return a;
    if (IsEmptyU(a)) return a;
    if (IsEmptyU(b)) return b;
    return {LoOr2(a, b), HiOr2(a, b)};
}

// Via De Morgan, as the reference defines them. Deriving directly would round differently.
UItv AndU(const UItv &a, const UItv &b) { return NotU(OrU(NotU(a), NotU(b))); }
UItv XorU(const UItv &a, const UItv &b) { return AndU(OrU(a, b), NotU(AndU(a, b))); }

SItv OrS(const SItv &a, const SItv &b) {
    const auto [an, ap] = SignSplit(a);
    const auto [bn, bp] = SignSplit(b);
    return SignMerge(UnionU(UnionU(OrU(an, bp), OrU(an, bn)), OrU(ap, bn)), OrU(ap, bp));
}

SItv AndS(const SItv &a, const SItv &b) { return NotS(OrS(NotS(a), NotS(b))); }

SItv XorS(const SItv &a, const SItv &b) {
    const auto [an, ap] = SignSplit(a);
    const auto [bn, bp] = SignSplit(b);
    return SignMerge(UnionU(XorU(an, bp), XorU(ap, bn)), UnionU(XorU(ap, bp), XorU(an, bn)));
}

SItv AsSigned(const Interval &x) { return {SaturatedIntCast(x.Lo), SaturatedIntCast(x.Hi)}; }

std::pair<Interval, Interval> Split(const Interval &x, bool nonzero) {
    if (x.Lo >= 0) return {Interval::Empty(), x};
    if (x.Hi < 0) return {x, Interval::Empty()};
    return {Interval{x.Lo, std::nexttoward(0.0, -1.0), x.Lsb}, Interval{nonzero ? std::nexttoward(0.0, 1.0) : 0.0, x.Hi, x.Lsb}};
}

Interval PositiveFMod(const Interval &x, const Interval &y) {
    if (x.IsEmpty() || y.IsEmpty()) return Interval::Empty();
    const int n = ToInt(x.Lo / y.Hi);
    const int precision = std::min(x.Lsb, y.Lsb);
    const double hi = x.Hi / (n + 1);
    if (y.Hi <= hi) return {0.0, std::nexttoward(y.Hi, 0), precision};
    if (y.Lo <= hi) return {0.0, std::nexttoward(hi, 0), precision};
    return {x.Lo - n * y.Hi, x.Hi - n * y.Lo, precision};
}

Interval IPow(const Interval &x, int k) {
    if (k == 0) return {1, 1, 0};
    int precision = x.Lsb * k;
    if (!x.HasZero()) {
        const double v = MinValAbs(x);
        const int sign = SignMinValAbs(x);
        const int p1 = k * ToInt(std::log2(std::abs(v)));
        const double u = std::pow(2, x.Lsb);
        const double delta = std::abs(std::pow(1 + sign * u / v, k) - 1);
        const int p2 = delta == 0 ? Taylor(std::log2(double(k)) + x.Lsb - std::log2(std::abs(v))) : Taylor(std::log2(delta));
        precision = WrapAdd(p1, p2);
    }
    const double a = std::pow(x.Lo, k), b = std::pow(x.Hi, k);
    if ((k % 2 == 0) && x.HasZero()) return {0, std::max(a, b), precision};
    return {std::min(a, b), std::max(a, b), precision};
}

Interval Rounded(double (*f)(double), const Interval &x, int lsb) {
    if (x.IsEmpty()) return Interval::Empty();
    return {f(x.Lo), f(x.Hi), lsb};
}

Interval Increasing(double (*f)(double), const Interval &x, int fallback) {
    if (x.IsEmpty()) return Interval::Empty();
    const double v = MaxValAbs(x);
    const int precision = Precision(f, v, SignMaxValAbs(x) * std::pow(2, x.Lsb), fallback);
    return {f(x.Lo), f(x.Hi), precision};
}

Interval Compare(const Interval &x, const Interval &y, bool strict) {
    if (x.IsEmpty() || y.IsEmpty()) return Interval::Empty();
    if (strict ? x.Lo > y.Hi : x.Lo >= y.Hi) return {1, 1, 0};
    if (strict ? x.Hi <= y.Lo : x.Hi < y.Lo) return {0, 0, 0};
    return {0, 1, 0};
}

Interval Equality(const Interval &x, const Interval &y, bool eq) {
    if (x.IsEmpty() || y.IsEmpty()) return Interval::Empty();
    const double same = eq ? 1 : 0;
    if (x.Lo == x.Hi && x.Hi == y.Lo && y.Lo == y.Hi) return {same, same, 0};
    if (x.Hi < y.Lo || x.Lo > y.Hi) return {1 - same, 1 - same, 0};
    return {0, 1, 0};
}

} // namespace

Interval::Interval(double a, double b, int bits) : Lsb(bits == INT_MIN ? DefaultLsb : bits) {
    if (std::isnan(a) || std::isnan(b)) {
        Lo = Hi = std::numeric_limits<double>::quiet_NaN();
    } else {
        Lo = std::min(a, b);
        Hi = std::max(a, b);
    }
}

bool Interval::IsEmpty() const { return std::isnan(Lo) || std::isnan(Hi); }

bool operator==(const Interval &a, const Interval &b) { return (a.IsEmpty() && b.IsEmpty()) || (a.Lo == b.Lo && a.Hi == b.Hi); }

Interval Reunion(const Interval &i, const Interval &j) {
    if (i.IsEmpty()) return j;
    if (j.IsEmpty()) return i;
    return {std::min(i.Lo, j.Lo), std::max(i.Hi, j.Hi), std::min(i.Lsb, j.Lsb)};
}

Interval Intersection(const Interval &i, const Interval &j) {
    if (i.IsEmpty()) return i;
    if (j.IsEmpty()) return j;
    const double l = std::max(i.Lo, j.Lo), h = std::min(i.Hi, j.Hi);
    if (l > h) return Interval::Empty();
    return {l, h, std::min(i.Lsb, j.Lsb)};
}

// 32-bit set width, and zero is exact.
Interval Singleton(double x) {
    if (x == 0) return {0, 0, 0};
    return {x, x, WrapAdd(ToInt(std::floor(std::log2(std::abs(x)))), -32)};
}

namespace ivl {

Interval IntNum(int64_t x) { return {double(x), double(x), 0}; }
Interval RealNum(double x) { return Singleton(x); }

Interval Slider(const Interval &lo, const Interval &hi, const Interval &step) {
    if (lo.IsEmpty() || hi.IsEmpty() || step.IsEmpty()) return Interval::Empty();
    int lsb = std::min(step.Lsb, lo.Lsb);
    if (step.Lo > 0) lsb = std::min(lsb, ToInt(std::log2(step.Lo)));
    return {lo.Lo, hi.Hi, lsb};
}

Interval IntCast(const Interval &x) {
    if (x.IsEmpty()) return Interval::Empty();
    return {double(SaturatedIntCast(x.Lo)), double(SaturatedIntCast(x.Hi)), 0};
}

Interval FloatCast(const Interval &x) { return {x.Lo, x.Hi, std::min(x.Lsb, -1)}; }

// The integer path catches wraparound, and only when *both* operands claim integer lsb.
Interval Add(const Interval &x, const Interval &y) {
    if (x.IsEmpty() || y.IsEmpty()) return Interval::Empty();
    const int lsb = std::min(x.Lsb, y.Lsb);
    if (x.Lsb >= 0 && y.Lsb >= 0) {
        const double lo = x.Lo + y.Lo, hi = x.Hi + y.Hi;
        constexpr double Min = double(INT_MIN), Max = double(INT_MAX);
        if (lo <= Min - 1 && hi >= Min) return {Min, Max, lsb};
        if (lo <= Max && hi >= Max + 1) return {Min, Max, lsb};
        return {double(WrapAdd(ToInt(x.Lo), ToInt(y.Lo))), double(WrapAdd(ToInt(x.Hi), ToInt(y.Hi))), lsb};
    }
    return {x.Lo + y.Lo, x.Hi + y.Hi, lsb};
}

Interval Sub(const Interval &x, const Interval &y) {
    if (x.IsEmpty() || y.IsEmpty()) return Interval::Empty();
    return {x.Lo - y.Hi, x.Hi - y.Lo, std::min(x.Lsb, y.Lsb)};
}

Interval Mul(const Interval &x, const Interval &y) {
    if (x.IsEmpty() || y.IsEmpty()) return Interval::Empty();
    const double a = SpecialMult(x.Lo, y.Lo), b = SpecialMult(x.Lo, y.Hi);
    const double c = SpecialMult(x.Hi, y.Lo), d = SpecialMult(x.Hi, y.Hi);
    if (x.Lsb >= 0 && y.Lsb >= 0 && std::max(std::abs(x.Lo), std::abs(x.Hi)) * std::max(std::abs(y.Lo), std::abs(y.Hi)) >= double(INT_MAX))
        return {double(INT_MIN), double(INT_MAX), WrapAdd(x.Lsb, y.Lsb)};
    return {std::min({a, b, c, d}), std::max({a, b, c, d}), WrapAdd(x.Lsb, y.Lsb)};
}

Interval Inv(const Interval &x) {
    if (x.IsEmpty()) return Interval::Empty();
    const int sign = SignMaxValAbs(x);
    double v = MaxValAbs(x);
    if (std::isinf(v)) v = sign == -1 ? double(INT_MAX) : double(INT_MIN);
    const int precision = Precision([](double d) { return 1.0 / d; }, v, sign * std::pow(2, x.Lsb), Taylor(x.Lsb - 2 * std::log2(std::abs(v))));
    if (x.Hi < 0 || x.Lo >= 0) return {1.0 / x.Hi, 1.0 / x.Lo, precision};
    if (x.Hi == 0 && x.Lo < 0) return {-Inf, 1.0 / x.Lo, precision};
    if (x.Lo == 0 && x.Hi > 0) return {1.0 / x.Hi, Inf, precision};
    return {-Inf, Inf, precision};
}

Interval Div(const Interval &x, const Interval &y) { return Mul(x, Inv(y)); }
Interval Neg(const Interval &x) {
    if (x.IsEmpty()) return Interval::Empty();
    return {-x.Hi, -x.Lo, x.Lsb};
}

Interval Mod(const Interval &x, const Interval &y) {
    const auto [xn, xp] = Split(x, false);
    const auto [yn, yp] = Split(y, true);
    const Interval xnyn = Neg(PositiveFMod(Neg(xn), Neg(yn)));
    const Interval xnyp = Neg(PositiveFMod(Neg(xn), yp));
    const Interval xpyn = PositiveFMod(xp, Neg(yn));
    const Interval xpyp = PositiveFMod(xp, yp);
    Interval bb = Reunion(
        Reunion(Singleton(std::fmod(x.Hi, y.Hi)), Singleton(std::fmod(x.Lo, y.Hi))), Reunion(Singleton(std::fmod(x.Hi, y.Lo)), Singleton(std::fmod(x.Lo, y.Lo)))
    );
    bb = Interval{bb.Lo, bb.Hi, std::min(x.Lsb, y.Lsb)};
    return Reunion(Reunion(Reunion(Reunion(bb, xnyn), xnyp), xpyn), xpyp);
}

Interval Gt(const Interval &x, const Interval &y) { return Compare(x, y, true); }
Interval Ge(const Interval &x, const Interval &y) { return Compare(x, y, false); }

Interval Lt(const Interval &x, const Interval &y) { return Gt(y, x); }
Interval Le(const Interval &x, const Interval &y) { return Ge(y, x); }

Interval Eq(const Interval &x, const Interval &y) { return Equality(x, y, true); }
Interval Ne(const Interval &x, const Interval &y) { return Equality(x, y, false); }

Interval And(const Interval &x, const Interval &y) {
    if (x.IsEmpty() || y.IsEmpty()) return Interval::Empty();
    const SItv z = AndS(AsSigned(x), AsSigned(y));
    return {double(z.Lo), double(z.Hi), std::min(x.Lsb, y.Lsb)};
}

Interval Or(const Interval &x, const Interval &y) {
    if (x.IsEmpty() || y.IsEmpty()) return Interval::Empty();
    const SItv z = OrS(AsSigned(x), AsSigned(y));
    return {double(z.Lo), double(z.Hi), std::min(x.Lsb, y.Lsb)};
}

Interval Xor(const Interval &x, const Interval &y) {
    if (x.IsEmpty() || y.IsEmpty()) return Interval::Empty();
    const SItv sx = AsSigned(x), sy = AsSigned(y);
    const SItv z = XorS(sx, sy);
    int precision = std::min(x.Lsb, y.Lsb);
    if (sx.Lo == sx.Hi && sy.Lo == sy.Hi) {
        int v = sx.Lo ^ sy.Lo;
        precision = 0;
        while ((v & 1) == 0 && v != 0) {
            v /= 2;
            ++precision;
        }
    }
    if (sx.Lo == sx.Hi) precision = y.Lsb;
    if (sy.Hi == sy.Lo) precision = x.Lsb;
    return {double(z.Lo), double(z.Hi), precision};
}

Interval Lsh(const Interval &x, const Interval &k) {
    if (x.IsEmpty() || k.IsEmpty()) return Interval::Empty();
    const Interval z = Mul(x, Interval{std::pow(2, k.Lo), std::pow(2, k.Hi)});
    return {z.Lo, z.Hi, WrapAdd(x.Lsb, ToInt(k.Lo))};
}

Interval Rsh(const Interval &x, const Interval &k) {
    if (x.IsEmpty() || k.IsEmpty()) return Interval::Empty();
    const Interval z = Mul(x, Interval{std::pow(2, -k.Hi), std::pow(2, -k.Lo)});
    return {z.Lo, z.Hi, WrapAdd(x.Lsb, -ToInt(k.Hi))};
}

Interval Abs(const Interval &x) {
    if (x.IsEmpty()) return Interval::Empty();
    if (x.Lo >= 0) return x;
    if (x.Lsb >= 0 && x.Lo <= double(INT_MIN)) {
        const double lo = x.Hi >= 0 ? 0 : std::min(std::abs(x.Hi), double(INT_MAX));
        return {lo, double(INT_MAX), x.Lsb};
    }
    if (x.Hi <= 0) return {-x.Hi, -x.Lo, x.Lsb};
    return {0, std::max(std::abs(x.Lo), std::abs(x.Hi)), x.Lsb};
}

Interval Min(const Interval &x, const Interval &y) {
    if (x.IsEmpty() || y.IsEmpty()) return Interval::Empty();
    return {std::min(x.Lo, y.Lo), std::min(x.Hi, y.Hi), std::min(x.Lsb, y.Lsb)};
}

Interval Max(const Interval &x, const Interval &y) {
    if (x.IsEmpty() || y.IsEmpty()) return Interval::Empty();
    return {std::max(x.Lo, y.Lo), std::max(x.Hi, y.Hi), std::min(x.Lsb, y.Lsb)};
}

// lsb -1 and not 0: the results are integral but still floats, and 0 would read as int.
Interval Floor(const Interval &x) { return Rounded(std::floor, x, -1); }
Interval Ceil(const Interval &x) { return Rounded(std::ceil, x, -1); }
Interval Rint(const Interval &x) { return Rounded(std::rint, x, std::max(0, x.Lsb)); }
Interval Round(const Interval &x) { return Rounded(std::round, x, std::max(0, x.Lsb)); }

Interval Sqrt(const Interval &x) {
    const Interval i = Intersection(Interval{0, Inf, 0}, x);
    if (i.IsEmpty()) return Interval::Empty();
    const int precision = Precision(std::sqrt, i.Hi, -std::pow(2, i.Lsb), i.Hi == 0 ? Taylor(i.Lsb / 2.0) : Taylor(i.Lsb - std::log2(i.Hi) - 1.0));
    return {std::sqrt(i.Lo), std::sqrt(i.Hi), precision};
}

Interval Exp(const Interval &x) {
    if (x.IsEmpty()) return Interval::Empty();
    const double delta = std::exp(std::pow(2, x.Lsb)) - 1;
    const int p1 = Taylor(x.Lo * std::numbers::log2e);
    const int p2 = delta == 0 ? x.Lsb : Taylor(std::log2(delta));
    return {std::exp(x.Lo), std::exp(x.Hi), WrapAdd(p1, p2)};
}

Interval Log(const Interval &x) {
    const Interval i = Intersection(x, Interval{0, Inf, 0});
    if (i.IsEmpty()) return Interval::Empty();
    const int precision = Precision(std::log, i.Hi, -std::pow(2, i.Lsb), Taylor(i.Lsb - std::log2(std::abs(i.Hi))));
    return {std::log(i.Lo), std::log(i.Hi), precision};
}

// Precision from the raw high bound before clamping, where `Log` intersects first.
Interval Log10(const Interval &x) {
    if (x.IsEmpty()) return Interval::Empty();
    const int precision = Precision(std::log10, x.Hi, -std::pow(2, x.Lsb), Taylor(x.Lsb - std::log2(std::abs(x.Hi)) - std::log2(std::numbers::ln10)));
    const Interval i = Intersection(Interval{0, Inf}, x);
    if (i.IsEmpty()) return Interval::Empty();
    return {std::log10(i.Lo), std::log10(i.Hi), precision};
}

Interval Pow(const Interval &x, const Interval &y) {
    Interval z = Interval::Empty();
    const Interval xp = Intersection(x, Interval{std::nexttoward(0.0, 1.0), Inf, 0});
    const Interval xn = Intersection(x, Interval{-Inf, std::nexttoward(0.0, -1.0), 0});
    if (y.HasZero()) z = Reunion(z, Interval(1, 1, 0));
    if (x.HasZero()) z = Reunion(z, Interval(0, 0, 0));
    if (!xp.IsEmpty()) z = Reunion(z, Exp(Mul(y, Log(xp))));
    if (!xn.IsEmpty()) {
        const int y0 = std::max(0, SaturatedIntCast(y.Lo));
        const int y1 = std::max(0, SaturatedIntCast(y.Hi));
        Interval w = IPow(xn, y0);
        if (y1 > y0) {
            w = Reunion(w, IPow(xn, y0 + 1));
            w = Reunion(w, IPow(xn, y1 - 1));
            w = Reunion(w, IPow(xn, y1));
        }
        z = Reunion(z, w);
    }
    return z;
}

Interval Sin(const Interval &x) {
    if (x.IsEmpty()) return Interval::Empty();
    const int wide = Precision(std::sin, 0.5, std::pow(2, x.Lsb), WrapAdd(Twice(x.Lsb), -1));
    if (x.Size() >= 2 * M_PI) return {-1, 1, wide};

    double l = std::fmod(x.Lo, 2 * M_PI);
    if (l < 0) l += 2 * M_PI;
    const Interval i(l, l + x.Size(), x.Lsb);
    double lo = std::min(std::sin(i.Lo), std::sin(i.Hi));
    double hi = std::max(std::sin(i.Lo), std::sin(i.Hi));
    if (i.Has(M_PI_2) || i.Has(5 * M_PI_2)) hi = 1;
    if (i.Has(3 * M_PI_2) || i.Has(7 * M_PI_2)) lo = -1;

    double v = M_PI_2;
    if (i.Hi < M_PI_2) {
        v = x.Hi;
    } else if ((i.Lo > M_PI_2 && i.Hi < 3 * M_PI_2) || (i.Lo > 3 * M_PI_2 && i.Hi < 2.5 * M_PI)) {
        const double dhi = std::ceil(i.Hi / M_PI + 0.5) - i.Hi / M_PI;
        const double dlo = i.Lo / M_PI - std::floor(i.Lo / M_PI - 0.5);
        v = dlo > dhi ? x.Hi : x.Lo;
    }
    return {
        lo, hi,
        Precision(std::sin, v, std::pow(2, x.Lsb), v != 0.5 * M_PI ? WrapAdd(x.Lsb, Taylor(std::log2(std::abs(std::cos(v))))) : WrapAdd(Twice(x.Lsb), -1))
    };
}

Interval Cos(const Interval &x) {
    if (x.IsEmpty()) return Interval::Empty();
    const int wide = Precision(std::cos, 0, std::pow(2, x.Lsb), WrapAdd(Twice(x.Lsb), -1));
    if (x.Size() >= 2 * M_PI) return {-1, 1, wide};

    double l = std::fmod(x.Lo, 2 * M_PI);
    if (l < 0) l += 2 * M_PI;
    const Interval i(l, l + x.Size(), x.Lsb);
    double lo = std::min(std::cos(i.Lo), std::cos(i.Hi));
    double hi = std::max(std::cos(i.Lo), std::cos(i.Hi));
    if (i.Has(0) || i.Has(2 * M_PI)) hi = 1;
    if (i.Has(M_PI) || i.Has(3 * M_PI)) lo = -1;

    double v = 0;
    if (i.Hi < M_PI || (i.Lo > M_PI && i.Hi < 2 * M_PI)) {
        const double dhi = std::ceil(x.Hi / M_PI) - x.Hi / M_PI;
        const double dlo = x.Lo / M_PI - std::floor(x.Lo / M_PI);
        v = dhi < dlo ? x.Hi : x.Lo;
    }
    return {lo, hi, Precision(std::cos, v, std::pow(2, x.Lsb), v != 0 ? WrapAdd(x.Lsb, Taylor(std::log2(std::abs(std::sin(v))))) : WrapAdd(Twice(x.Lsb), -1))};
}

Interval Tan(const Interval &x) {
    if (x.IsEmpty()) return Interval::Empty();
    if (x.Size() >= M_PI) return {-Inf, Inf, Precision(std::tan, 0, std::pow(2, x.Lsb), x.Lsb)};
    const Interval i(std::fmod(x.Lo, M_PI), std::fmod(x.Lo, M_PI) + x.Size(), x.Lsb);
    double v = 0;
    int sign = 1;
    if (i.Lo > 0) {
        v = i.Lo;
    } else if (i.Hi < 0) {
        v = i.Hi;
        sign = -1;
    }
    const int precision = Precision(std::tan, v, sign * std::pow(2, x.Lsb), Taylor(x.Lsb - 2.0 * std::log2(std::cos(v))));
    if (i.Has(-M_PI_2) || i.Has(M_PI_2)) return {-Inf, Inf, precision};
    return {std::min(std::tan(i.Lo), std::tan(i.Hi)), std::max(std::tan(i.Lo), std::tan(i.Hi)), precision};
}

Interval Asin(const Interval &x) {
    const Interval i = Intersection(Interval{-1, 1, 0}, x);
    if (i.IsEmpty()) return Interval::Empty();
    const auto [v, sign] = NearZero(i);
    const int precision = Precision(std::asin, v, sign * std::pow(2, i.Lsb), Taylor(x.Lsb - std::log2(1 - v * v) / 2));
    return {std::asin(i.Lo), std::asin(i.Hi), precision};
}

Interval Acos(const Interval &x) {
    const Interval i = Intersection(Interval{-1, 1, 0}, x);
    if (i.IsEmpty()) return Interval::Empty();
    const auto [v, sign] = NearZero(i);
    const int precision = Precision(std::acos, v, sign * std::pow(2, i.Lsb), Taylor(i.Lsb - std::log2(1 - v * v) / 2));
    return {std::acos(i.Hi), std::acos(i.Lo), precision};
}

Interval Atan(const Interval &x) {
    const double v = MaxValAbs(x);
    return Increasing(std::atan, x, Taylor(x.Lsb - std::log2(1 + v * v)));
}

Interval Atan2(const Interval &y, const Interval &x) {
    if (x.IsEmpty() || y.IsEmpty()) return Interval::Empty();
    double lo = -M_PI, hi = M_PI;
    if (y.Lo <= 0 && x.HasZero()) {
        const Interval dp = Div(y, Interval{0, x.Hi, x.Lsb});
        const Interval dn = Div(y, Interval{x.Lo, 0, x.Lsb});
        const int pp = ExactPrecisionUnary(std::atan, MaxValAbs(dp), SignMaxValAbs(dp) * std::pow(2, dp.Lsb));
        const int pn = ExactPrecisionUnary(std::atan, MaxValAbs(dn), SignMaxValAbs(dn) * std::pow(2, dn.Lsb));
        return {lo, hi, std::min(pp, pn)};
    }
    const Interval d = Div(y, x);
    const int precision = ExactPrecisionUnary(std::atan, MaxValAbs(d), SignMaxValAbs(d) * std::pow(2, d.Lsb));

    if (y.Lo >= 0) {
        hi = x.Lo <= 0 ? std::atan2(y.Lo, x.Lo) : std::atan2(y.Hi, x.Lo);
    } else if (x.Hi >= 0) {
        hi = y.Hi >= 0 ? std::atan2(y.Hi, x.Lo) : std::atan2(y.Hi, x.Hi);
    } else {
        hi = std::atan2(y.Lo, x.Hi);
    }

    if (y.Hi <= 0) {
        lo = x.Lo <= 0 ? std::atan2(y.Hi, x.Lo) : std::atan2(y.Lo, x.Lo);
    } else if (x.Hi >= 0) {
        lo = y.Lo >= 0 ? std::atan2(y.Lo, x.Hi) : std::atan2(y.Lo, x.Lo);
    } else {
        lo = std::atan2(y.Hi, x.Hi);
    }
    return {lo, hi, precision};
}

Interval Sinh(const Interval &x) {
    if (x.IsEmpty()) return Interval::Empty();
    const auto [v, sign] = NearZero(x);
    const int precision = Precision(std::sinh, v, sign * std::pow(2, x.Lsb), Taylor(x.Lsb + std::log2(std::cosh(v))));
    return {std::sinh(x.Lo), std::sinh(x.Hi), precision};
}

Interval Cosh(const Interval &x) {
    if (x.IsEmpty()) return Interval::Empty();
    if (x.HasZero()) return {1, std::max(std::cosh(x.Lo), std::cosh(x.Hi)), Precision(std::cosh, 0, std::pow(2, x.Lsb), WrapAdd(Twice(x.Lsb), -1))};
    double v = 0;
    int sign = 1;
    if (x.Lo > 0) {
        v = x.Lo;
    } else if (x.Hi < 0) {
        v = x.Hi;
        sign = -1;
    }
    const int precision = Precision(std::cosh, v, sign * std::pow(2, x.Lsb), Taylor(x.Lsb + std::log2(std::abs(std::sinh(v)))));
    return {std::min(std::cosh(x.Lo), std::cosh(x.Hi)), std::max(std::cosh(x.Lo), std::cosh(x.Hi)), precision};
}

Interval Tanh(const Interval &x) {
    const double v = MaxValAbs(x);
    return Increasing(std::tanh, x, Taylor(x.Lsb - 2.0 * std::log2(std::cosh(v))));
}

Interval Asinh(const Interval &x) {
    const double v = MaxValAbs(x);
    return Increasing(std::asinh, x, Taylor(x.Lsb - std::log2(1 + v * v) / 2));
}

Interval Acosh(const Interval &x) {
    const Interval i = Intersection(Interval{1, Inf}, x); // default lsb, unlike its neighbours
    if (i.IsEmpty()) return Interval::Empty();
    const int precision = Precision(std::acosh, x.Hi, -std::pow(2, x.Lsb), Taylor(x.Lsb - std::log2(x.Hi * x.Hi - 1) / 2));
    return {std::acosh(i.Lo), std::acosh(i.Hi), precision};
}

Interval Atanh(const Interval &x) {
    const Interval i = Intersection(Interval{std::nexttoward(-1, 0), std::nexttoward(1, 0), 0}, x);
    if (i.IsEmpty()) return Interval::Empty();
    const double v = MinValAbs(x);
    const int sign = SignMinValAbs(x);
    const int precision = Precision(std::atanh, v, sign * std::pow(2, x.Lsb), Taylor(x.Lsb - std::log2(1 - v * v)));
    return {std::atanh(i.Lo), std::atanh(i.Hi), precision};
}

Interval Remainder(const Interval &) { return {}; }

} // namespace ivl
} // namespace faustlens
