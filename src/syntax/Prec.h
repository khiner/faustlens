// Shared operator binding powers for parsing and printing.
#pragma once

#include "syntax/Term.h"
#include "syntax/Token.h"

#include <cstdint>

namespace faustlens {

enum class Assoc : uint8_t { Left, Right, Postfix };

// Suffix operators take a braced block.
enum class OpShape : uint8_t { Infix, Postfix, Suffix };

struct OpRow {
    uint8_t Level = 0; // 1..15 from lowest precedence; 0 for non-operators
    Assoc Assoc = Assoc::Left;
    OpShape Shape = OpShape::Infix;
    Kind Kind = Kind::Count_;
    bool Spaced = false; // space composition operators
    Tok Tok = Tok::Count_;
};

inline constexpr uint8_t PrimitiveLevel = 16;

// Allow equal precedence on the associative side.
constexpr uint16_t LeftBp(const OpRow &r) { return uint16_t(2 * r.Level + (r.Assoc == Assoc::Right ? 1 : 0)); }
constexpr uint16_t RightBp(const OpRow &r) { return uint16_t(2 * r.Level + (r.Assoc == Assoc::Left ? 1 : 0)); }

uint8_t PrecOf(const Terms &, ValueId);

// Read the operator from BinOp payloads.
const OpRow &RowOf(Tok);
const OpRow &RowOf(Kind);
const OpRow &RowOf(const Terms &, ValueId);

// Argument positions exclude comma, with, and letrec.
enum class Level : uint8_t { Expression = 1, Argument = 2 };

bool ExcludedFromArgument(const Terms &, ValueId);

} // namespace faustlens
