// Read forward for the parser's binding powers, backward for the printer's parens.
#pragma once

#include "syntax/Term.h"
#include "syntax/Token.h"

#include <cstdint>

namespace faustlens {

enum class Assoc : uint8_t { Left, Right, Postfix };

// A suffix operator takes a braced block rather than a right operand.
enum class OpShape : uint8_t { Infix, Postfix, Suffix };

struct OpRow {
    uint8_t Level = 0; // 1..15, lowest binds loosest. 0 means "not an operator"
    Assoc Assoc = Assoc::Left;
    OpShape Shape = OpShape::Infix;
    Kind Kind = Kind::Count_;
    bool Spaced = false; // diagram-shaped operators space, arithmetic is tight
    Tok Tok = Tok::Count_;
};

inline constexpr uint8_t PrimitiveLevel = 16;

// A same-level operand is taken on the associativity-favoured side only.
constexpr uint16_t LeftBp(const OpRow &r) { return uint16_t(2 * r.Level + (r.Assoc == Assoc::Right ? 1 : 0)); }
constexpr uint16_t RightBp(const OpRow &r) { return uint16_t(2 * r.Level + (r.Assoc == Assoc::Left ? 1 : 0)); }

uint8_t PrecOf(const Terms &, ValueId);

// All agree but `BinOp`, whose operator is a payload recoverable only from the term.
const OpRow &RowOf(Tok);
const OpRow &RowOf(Kind);
const OpRow &RowOf(const Terms &, ValueId);

// Argument admits `:`, `<:`, `:>`, `~` and below, but not `,`, `with`, `letrec`.
enum class Level : uint8_t { Expression = 1, Argument = 2 };

bool ExcludedFromArgument(const Terms &, ValueId);

} // namespace faustlens
