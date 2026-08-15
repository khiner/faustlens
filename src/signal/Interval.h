// The interval domain, from the reference's `compiler/interval/`: a tighter bound
// loses parity as surely as a looser one. Defaults to `[lowest(), max()]`. `lsb` is
// fixed-point resolution, `>= 0` meaning integer.
#pragma once

#include <cstdint>
#include <limits>

namespace faustlens {

struct Interval {
    static constexpr int DefaultLsb = -24;

    double Lo = std::numeric_limits<double>::lowest();
    double Hi = std::numeric_limits<double>::max();
    int Lsb = DefaultLsb;

    Interval() = default;
    Interval(double a, double b, int lsb = DefaultLsb);
    explicit Interval(double v) : Interval(v, v) {}

    static Interval Empty() { return {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()}; }

    bool IsEmpty() const;
    double Size() const { return Hi - Lo; }
    bool Has(double x) const { return Lo <= x && Hi >= x; }
    bool HasZero() const { return Has(0.0); }
};

// Bounds only, no lsb.
bool operator==(const Interval &a, const Interval &b);

Interval Reunion(const Interval &, const Interval &);
Interval Intersection(const Interval &, const Interval &);
Interval Singleton(double);

namespace ivl {

Interval IntNum(int64_t);
Interval RealNum(double);
// Reads only `lo`'s low end and `hi`'s high end.
Interval Slider(const Interval &lo, const Interval &hi, const Interval &step);

Interval IntCast(const Interval &);
Interval FloatCast(const Interval &);

Interval Add(const Interval &, const Interval &);
Interval Sub(const Interval &, const Interval &);
Interval Mul(const Interval &, const Interval &);
Interval Div(const Interval &, const Interval &);
Interval Inv(const Interval &);
Interval Neg(const Interval &);
Interval Mod(const Interval &, const Interval &);

Interval Gt(const Interval &, const Interval &);
Interval Lt(const Interval &, const Interval &);
Interval Ge(const Interval &, const Interval &);
Interval Le(const Interval &, const Interval &);
Interval Eq(const Interval &, const Interval &);
Interval Ne(const Interval &, const Interval &);

Interval And(const Interval &, const Interval &);
Interval Or(const Interval &, const Interval &);
Interval Xor(const Interval &, const Interval &);
Interval Lsh(const Interval &, const Interval &);
Interval Rsh(const Interval &, const Interval &);

Interval Abs(const Interval &);
Interval Min(const Interval &, const Interval &);
Interval Max(const Interval &, const Interval &);
Interval Floor(const Interval &);
Interval Ceil(const Interval &);
Interval Rint(const Interval &);
Interval Round(const Interval &);
Interval Sqrt(const Interval &);
Interval Exp(const Interval &);
Interval Log(const Interval &);
Interval Log10(const Interval &);
Interval Pow(const Interval &, const Interval &);
Interval Sin(const Interval &);
Interval Cos(const Interval &);
Interval Tan(const Interval &);
Interval Asin(const Interval &);
Interval Acos(const Interval &);
Interval Atan(const Interval &);
Interval Atan2(const Interval &, const Interval &);
Interval Sinh(const Interval &);
Interval Cosh(const Interval &);
Interval Tanh(const Interval &);
Interval Asinh(const Interval &);
Interval Acosh(const Interval &);
Interval Atanh(const Interval &);
// A stub in the reference: always the default interval.
Interval Remainder(const Interval &);

} // namespace ivl
} // namespace faustlens
