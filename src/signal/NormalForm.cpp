#include "signal/NormalForm.h"

#include "signal/Simplify.h"

#include <cmath>
#include <map>

namespace faustlens {
namespace {

bool IsGEZero(const Signals &s, SigId a) { return IsNum(s, a) && NumOf(s, a) >= 0; }

// Absolute values across the int/real split, so `2` and `-2.0` match.
bool SameMagnitude(const Signals &s, SigId a, SigId b) {
    if (!IsNum(s, a) || !IsNum(s, b)) return false;
    return std::fabs(NumOf(s, a)) == std::fabs(NumOf(s, b));
}

// Through `SimpBinOp`, which folds exactly on two literals, integer division included.
SigId AddNums(Signals &s, SigId a, SigId b) { return SimpBinOp(s, BinOpCode::Add, a, b); }
SigId SubNums(Signals &s, SigId a, SigId b) { return SimpBinOp(s, BinOpCode::Sub, a, b); }
SigId MulNums(Signals &s, SigId a, SigId b) { return SimpBinOp(s, BinOpCode::Mul, a, b); }
SigId DivNums(Signals &s, SigId a, SigId b) { return SimpBinOp(s, BinOpCode::Div, a, b); }
SigId MinusNum(Signals &s, SigId a) { return MulNums(s, s.MakeInt(-1), a); }

// Everything here is built raw, the simplification rules having already run.
SigId Mul(Signals &s, SigId x, SigId y) { return s.MakeBin(BinOpCode::Mul, x, y); }
SigId Div(Signals &s, SigId x, SigId y) { return s.MakeBin(BinOpCode::Div, x, y); }
SigId Add(Signals &s, SigId x, SigId y) { return s.MakeBin(BinOpCode::Add, x, y); }
SigId Sub(Signals &s, SigId x, SigId y) { return s.MakeBin(BinOpCode::Sub, x, y); }

SigId Pow(Signals &s, SigId x, int32_t n) { return s.Make(SigKind::Extended, uint8_t(Ext::Pow), 0, 0, {x, s.MakeInt(n)}); }
bool IsPow(const Signals &s, SigId t, SigId &x, int32_t &n) {
    if (s.KindOf(t) != SigKind::Extended) return false;
    if (Ext(s.Get(t).Form) != Ext::Pow || s.Get(t).ChildCount != 2) return false;
    if (!s.IsInt(s.Child(t, 1))) return false;
    x = s.Child(t, 0);
    n = s.IntValue(s.Child(t, 1));
    return true;
}

int Order(const Signals &s, SigId a) {
    const int o = s.Order(a);
    return o < 0 ? 0 : (o > 3 ? 3 : o);
}

// `k * x^n * y^m * ...`, factors ordered by `SigId`, the arena's creation order.
struct MTerm {
    Signals *Sigs;
    SigId Coef;
    std::map<SigId, int> Factors;

    MTerm(Signals &s, int32_t k) : Sigs(&s), Coef(s.MakeInt(k)) {}
    static MTerm Of(Signals &s, SigId t) {
        MTerm m(s, 1);
        m.Scale(t, 1);
        return m;
    }

    bool IsNotZero() const { return !IsZeroNum(*Sigs, Coef); }
    bool IsNegative() const { return !IsGEZero(*Sigs, Coef); }

    // A coefficient of one or minus one is free, which stops factoring a sign out forever.
    int Complexity() const {
        int c = (IsOneNum(*Sigs, Coef) || IsMinusOne(*Sigs, Coef)) ? 0 : 1;
        for (const auto &[f, q] : Factors) c += (1 + Order(*Sigs, f)) * std::abs(q);
        return c;
    }

    // Precondition: only terms of the same signature are ever combined.
    void Combine(const MTerm &m, bool negate) {
        if (IsZeroNum(*Sigs, m.Coef)) {
        } else if (IsZeroNum(*Sigs, Coef)) {
            Coef = negate ? MinusNum(*Sigs, m.Coef) : m.Coef;
            Factors = m.Factors;
        } else {
            Coef = negate ? SubNums(*Sigs, Coef, m.Coef) : AddNums(*Sigs, Coef, m.Coef);
        }
        Cleanup();
    }

    void MulBy(const MTerm &m) {
        Coef = MulNums(*Sigs, Coef, m.Coef);
        for (const auto &[f, q] : m.Factors) Factors[f] += q;
        Cleanup();
    }
    void DivBy(const MTerm &m) {
        Coef = DivNums(*Sigs, Coef, m.Coef);
        for (const auto &[f, q] : m.Factors) Factors[f] -= q;
        Cleanup();
    }

    // Every factor of `n` is a factor here, to at least the same power and sign.
    bool HasDivisor(const MTerm &n) const {
        if (n.Factors.empty()) return SameMagnitude(*Sigs, Coef, n.Coef);
        for (const auto &[f, v] : n.Factors) {
            const auto it = Factors.find(f);
            if (it == Factors.end()) return false;
            const int u = it->second;
            if (!(v == 0 || u / v > 0)) return false;
        }
        return true;
    }

    static MTerm Gcd(const MTerm &a, const MTerm &b) {
        Signals &s = *a.Sigs;
        MTerm r(s, 1);
        if (SameMagnitude(s, a.Coef, b.Coef)) r.Coef = a.Coef;
        for (const auto &[f, v1] : a.Factors) {
            const auto it = b.Factors.find(f);
            if (it == b.Factors.end()) continue;
            const int v2 = it->second;
            const int c = v1 > 0 && v2 > 0 ? std::min(v1, v2) : v1 < 0 && v2 < 0 ? std::max(v1, v2) : 0;
            if (c != 0) r.Factors[f] = c;
        }
        return r;
    }

    // One quotient per signal order, coefficient in order 0. `signature` drops it, so
    // `2*x` and `3*x` share one `ATerm` entry.
    SigId Tree(bool signature = false, bool negative = false) const {
        Signals &s = *Sigs;
        if (Factors.empty() || IsZeroNum(s, Coef)) {
            if (signature) return s.MakeInt(1);
            return negative ? MinusNum(s, Coef) : Coef;
        }

        SigId a[4], b[4];
        for (int order = 0; order < 4; ++order) {
            a[order] = b[order] = NoSig;
            for (const auto &[f, q] : Factors) {
                if (q == 0 || Order(s, f) != order) continue;
                if (q > 0) MulLeft(s, a[order], PowTerm(s, f, q));
                else MulLeft(s, b[order], PowTerm(s, f, -q));
            }
        }

        if (signature) a[0] = NoSig;
        else if (negative) a[0] = IsMinusOne(s, Coef) ? NoSig : MinusNum(s, Coef);
        else if (IsOneNum(s, Coef)) a[0] = NoSig;
        else a[0] = Coef;

        SigId r = NoSig;
        for (int order = 0; order < 4; ++order) {
            if (a[order] != NoSig && b[order] != NoSig) MulLeft(s, r, Div(s, a[order], b[order]));
            else if (a[order] != NoSig) MulLeft(s, r, a[order]);
            else if (b[order] != NoSig) DivLeft(s, r, b[order]);
        }
        return r == NoSig ? s.MakeInt(1) : r;
    }

    // A zero divisor stays unfolded and visible.
    void Scale(SigId t, int sign) {
        SigId x;
        int32_t n;
        if (IsNum(*Sigs, t)) {
            Coef = sign > 0 ? MulNums(*Sigs, Coef, t) : DivNums(*Sigs, Coef, t);
        } else if (IsBinOp(*Sigs, t, BinOpCode::Mul)) {
            Scale(Sigs->Child(t, 0), sign);
            Scale(Sigs->Child(t, 1), sign);
        } else if (IsBinOp(*Sigs, t, BinOpCode::Div)) {
            Scale(Sigs->Child(t, 0), sign);
            Scale(Sigs->Child(t, 1), -sign);
        } else if (IsPow(*Sigs, t, x, n)) {
            Factors[x] += sign * n;
        } else {
            Factors[t] += sign;
        }
    }

    void Cleanup() {
        if (IsZeroNum(*Sigs, Coef)) {
            Factors.clear();
            return;
        }
        std::erase_if(Factors, [](const auto &f) { return f.second == 0; });
    }

    static SigId PowTerm(Signals &s, SigId f, int q) { return q > 1 ? Pow(s, f, q) : f; }
    static void MulLeft(Signals &s, SigId &r, SigId x) { r = r == NoSig ? x : Mul(s, r, x); }
    static void DivLeft(Signals &s, SigId &r, SigId x) { r = Div(s, r == NoSig ? s.MakeReal(1.0) : r, x); }
};

// Orienting by creation order keeps the sum association deterministic.
SigId SimplifyingAdd(Signals &s, SigId x, SigId y) {
    if (IsNum(s, x) && IsNum(s, y)) return AddNums(s, x, y);
    if (IsZeroNum(s, x)) return y;
    if (IsZeroNum(s, y)) return x;
    return x <= y ? Add(s, x, y) : Add(s, y, x);
}

void AddSigned(Signals &s, bool p1, SigId v1, bool p2, SigId v2, bool &p3, SigId &v3) {
    if (IsZeroNum(s, v1)) {
        p3 = p2;
        v3 = v2;
        return;
    }
    if (IsZeroNum(s, v2)) {
        p3 = p1;
        v3 = v1;
        return;
    }
    if (p1 && p2) {
        p3 = true;
        v3 = Add(s, v1, v2);
        return;
    }
    if (p1) {
        p3 = true;
        v3 = Sub(s, v1, v2);
        return;
    }
    if (p2) {
        p3 = true;
        v3 = Sub(s, v2, v1);
        return;
    }
    p3 = false;
    v3 = Add(s, v1, v2);
}

struct ATerm {
    Signals *Sigs;
    std::map<SigId, MTerm> Terms;

    explicit ATerm(Signals &s) : Sigs(&s) {}

    void AddTree(SigId t, bool positive = true) {
        if (IsBinOp(*Sigs, t, BinOpCode::Add)) {
            AddTree(Sigs->Child(t, 0), positive);
            AddTree(Sigs->Child(t, 1), positive);
        } else if (IsBinOp(*Sigs, t, BinOpCode::Sub)) {
            AddTree(Sigs->Child(t, 0), positive);
            AddTree(Sigs->Child(t, 1), !positive);
        } else {
            Combine(MTerm::Of(*Sigs, t), !positive);
        }
    }

    void Combine(const MTerm &m, bool sub) {
        const SigId sig = m.Tree(true);
        const auto it = Terms.find(sig);
        if (it != Terms.end()) {
            it->second.Combine(m, sub);
        } else if (sub) {
            MTerm neg = m;
            neg.MulBy(MTerm(*Sigs, -1));
            Terms.emplace(sig, neg);
        } else {
            Terms.emplace(sig, m);
        }
    }

    MTerm GreatestDivisor() const {
        int best = 0;
        MTerm r(*Sigs, 1);
        for (auto p1 = Terms.begin(); p1 != Terms.end(); ++p1)
            for (auto p2 = std::next(p1); p2 != Terms.end(); ++p2) {
                const MTerm g = MTerm::Gcd(p1->second, p2->second);
                if (const int c = g.Complexity(); c > best) {
                    best = c;
                    r = g;
                }
            }
        return r;
    }

    ATerm Factorize(const MTerm &d) const {
        ATerm a(*Sigs), q(*Sigs);
        for (const auto &[sig, t] : Terms) {
            if (t.HasDivisor(d)) {
                MTerm quot = t;
                quot.DivBy(d);
                q.Combine(quot, false);
            } else {
                a.Combine(t, false);
            }
        }
        a.AddTree(Mul(*Sigs, d.Tree(), q.Tree()));
        return a;
    }

    // Bucketing by signal order keeps a sample-rate subexpression out of a control-rate sum.
    SigId Tree() const {
        Signals &s = *Sigs;
        SigId p[4], n[4];
        for (int order = 0; order < 4; ++order) p[order] = n[order] = s.MakeInt(0);

        for (const auto &[sig, m] : Terms) {
            if (m.IsNegative()) {
                const SigId t = m.Tree(false, true);
                n[Order(s, t)] = SimplifyingAdd(s, n[Order(s, t)], t);
            } else {
                const SigId t = m.Tree();
                p[Order(s, t)] = SimplifyingAdd(s, p[Order(s, t)], t);
            }
        }

        SigId sum = SubNums(s, p[0], n[0]);
        bool positive = true;
        for (int order = 3; order > 0; --order) {
            AddSigned(s, false, n[order], positive, sum, positive, sum);
            AddSigned(s, true, p[order], positive, sum, positive, sum);
        }
        return positive ? sum : Mul(s, s.MakeInt(-1), sum);
    }
};

} // namespace

SigId NormalizeAddTerm(Signals &s, SigId t) {
    ATerm a(s);
    a.AddTree(t);
    // Pulling one divisor out can expose another.
    for (MTerm d = a.GreatestDivisor(); d.IsNotZero() && d.Complexity() > 0; d = a.GreatestDivisor()) a = a.Factorize(d);
    return a.Tree();
}

} // namespace faustlens
