#pragma once

#include "signal/Plan.h"

#include <cmath>
#include <optional>

namespace faustlens {

inline int32_t IntegerBinary(BinOpCode op, int32_t x, int32_t y) {
    switch (op) {
        case BinOpCode::Add: return IntOf(uint32_t(x) + uint32_t(y));
        case BinOpCode::Sub: return IntOf(uint32_t(x) - uint32_t(y));
        case BinOpCode::Mul: return IntOf(uint32_t(x) * uint32_t(y));
        // Match ARM64 division by zero and signed overflow.
        case BinOpCode::Div: return y == 0 ? 0 : (y == -1 ? IntOf(-uint32_t(x)) : x / y);
        case BinOpCode::Rem: return y == 0 ? x : (y == -1 ? 0 : x % y);
        case BinOpCode::LeftShift: return IntOf(uint32_t(x) << (uint32_t(y) & 31));
        case BinOpCode::RightShift: return x >> (uint32_t(y) & 31);
        case BinOpCode::LRightShift: return IntOf(uint32_t(x) >> (uint32_t(y) & 31));
        case BinOpCode::GT: return x > y;
        case BinOpCode::LT: return x < y;
        case BinOpCode::GE: return x >= y;
        case BinOpCode::LE: return x <= y;
        case BinOpCode::EQ: return x == y;
        case BinOpCode::NE: return x != y;
        case BinOpCode::AND: return x & y;
        case BinOpCode::OR: return x | y;
        case BinOpCode::XOR: return x ^ y;
        default: return 0;
    }
}

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
