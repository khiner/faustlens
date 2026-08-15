// Constant folding over Box: iteration counts, widget bounds, `route`'s arguments, `2, 3 : +`.
#pragma once

#include "box/Box.h"

#include <optional>
#include <span>
#include <vector>

namespace faustlens {

// Saturating double-to-int, NaN to 0.
int32_t ToInt(double);

struct Num {
    bool IsInt = true;
    int32_t I = 0;
    double D = 0;

    double AsDouble() const { return IsInt ? double(I) : D; }
    int32_t AsInt() const { return IsInt ? I : ToInt(D); }
    static Num Int(int32_t v) { return {true, v, 0}; }
    static Num Real(double v) { return {false, 0, v}; }
};

inline BoxId MakeNum(Boxes &b, const Num &v) { return v.IsInt ? b.MakeInt(v.I) : b.MakeReal(v.D); }

// Nothing where the primitive is not a function of its arguments alone.
std::optional<Num> ApplyPrim(Prim, std::span<const Num> args);

// `box` run with `inputs`, or nothing where any of it is not constant.
std::optional<std::vector<Num>> FoldOutputs(const Boxes &, BoxId, std::span<const Num> inputs);

std::optional<Num> FoldConstant(const Boxes &, BoxId);

} // namespace faustlens
