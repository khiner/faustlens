#include "syntax/Prec.h"

#include <array>

namespace faustlens {
namespace {

constexpr std::array<OpRow, TokenKindCount> Ops = [] {
    std::array<OpRow, TokenKindCount> t{};
    auto set = [&t](Tok tok, uint8_t level, Assoc a, OpShape s, Kind k, bool spaced) { t[size_t(tok)] = {level, a, s, k, spaced, tok}; };
    set(Tok::With, 1, Assoc::Left, OpShape::Suffix, Kind::With, true);
    set(Tok::LetRec, 2, Assoc::Left, OpShape::Suffix, Kind::LetRec, true);
    set(Tok::Split, 3, Assoc::Right, OpShape::Infix, Kind::Split, true);
    set(Tok::Mix, 3, Assoc::Right, OpShape::Infix, Kind::Merge, true);
    set(Tok::Seq, 4, Assoc::Right, OpShape::Infix, Kind::Seq, true);
    set(Tok::Par, 5, Assoc::Right, OpShape::Infix, Kind::Par, false);
    set(Tok::Rec, 6, Assoc::Left, OpShape::Infix, Kind::RecComp, true);
    for (const Tok tok : {Tok::Lt, Tok::Le, Tok::Gt, Tok::Ge, Tok::Eq, Tok::Ne}) set(tok, 7, Assoc::Left, OpShape::Infix, Kind::BinOp, false);
    for (const Tok tok : {Tok::Add, Tok::Sub, Tok::Or}) set(tok, 8, Assoc::Left, OpShape::Infix, Kind::BinOp, false);
    for (const Tok tok : {Tok::Mul, Tok::Div, Tok::Mod, Tok::And, Tok::Xor, Tok::Lsh, Tok::Rsh}) set(tok, 9, Assoc::Left, OpShape::Infix, Kind::BinOp, false);
    set(Tok::PowOp, 10, Assoc::Left, OpShape::Infix, Kind::BinOp, false);
    set(Tok::FDelay, 11, Assoc::Left, OpShape::Infix, Kind::BinOp, false);
    set(Tok::Delay1, 12, Assoc::Left, OpShape::Postfix, Kind::Delay1, false);
    set(Tok::Dot, 13, Assoc::Left, OpShape::Postfix, Kind::Access, false);
    set(Tok::LPar, 14, Assoc::Left, OpShape::Postfix, Kind::Apply, false);
    set(Tok::LCroc, 15, Assoc::Left, OpShape::Postfix, Kind::ModifLocalDef, false);
    return t;
}();

constexpr std::array<OpRow, size_t(Kind::Count_)> KindOps = [] {
    std::array<OpRow, size_t(Kind::Count_)> t{};
    for (const OpRow &r : Ops) {
        if (r.Level != 0 && r.Kind != Kind::BinOp) t[size_t(r.Kind)] = r;
    }
    return t;
}();

} // namespace

const OpRow &RowOf(Tok t) { return Ops[size_t(t)]; }

const OpRow &RowOf(Kind k) { return KindOps[size_t(k)]; }

const OpRow &RowOf(const Terms &terms, ValueId v) {
    const Kind k = terms.KindOf(v);
    if (k == Kind::BinOp) return Ops[terms.Get(v).Payload];
    return RowOf(k);
}

uint8_t PrecOf(const Terms &terms, ValueId v) {
    const uint8_t level = RowOf(terms, v).Level;
    return level == 0 ? PrimitiveLevel : level;
}

bool ExcludedFromArgument(const Terms &terms, ValueId v) {
    const Kind k = terms.KindOf(v);
    return k == Kind::Par || k == Kind::With || k == Kind::LetRec;
}

} // namespace faustlens
