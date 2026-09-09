#pragma once

#include "signal/Plan.h"

#include <cmath>
#include <optional>

namespace faustlens {

inline std::optional<int32_t> PowerExponent(const Instr *e) {
    if (!e) return {};
    // Match reference specialization, including negative integer exponents.
    if (Op(e->Op) == Op::ConstInt) {
        const int32_t k = IntOf(e->Imm);
        if (k <= 8) return k;
    } else if (Op(e->Op) == Op::ConstReal) {
        const double v = RealOf(e->Imm, e->Aux);
        double whole;
        if (std::modf(v, &whole) == 0.0 && v >= 0 && v <= 8) return int32_t(v);
    }
    return {};
}

} // namespace faustlens
