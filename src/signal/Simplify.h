// The local signal rewrites and the pass applying them. Not optional, and ordered after
// promotion: `x*0` folds to the *real* zero.
#pragma once

#include "signal/Signal.h"

#include <initializer_list>
#include <span>
#include <vector>

namespace faustlens {

// Each returns what the rules leave behind, often not the node asked for.
SigId SimpBinOp(Signals &, BinOpCode, SigId, SigId);
SigId SimpIntCast(Signals &, SigId);
SigId SimpFloatCast(Signals &, SigId);
SigId SimpSelect2(Signals &, SigId sel, SigId a, SigId b);
SigId SimpControl(Signals &, SigId, SigId cond);

// `s@0 -> s` unless `s` is a projection, `0@d -> 0`, `(k*s)@d -> k*(s@d)` and
// `(s/k)@d -> (s@d)/k` for `k` under order 2, `(s@n)@m -> s@(n+m)`.
SigId SimpDelay(Signals &, SigId, SigId delay);
SigId SimpDelay1(Signals &, SigId);

SigId SimpExtended(Signals &, Ext, std::span<const SigId> args);
inline SigId SimpExtended(Signals &s, Ext e, std::initializer_list<SigId> args) {
    return SimpExtended(s, e, std::span<const SigId>(args.begin(), args.size()));
}

bool IsNum(const Signals &, SigId);
bool IsBinOp(const Signals &, SigId, BinOpCode);

inline double NumOf(const Signals &s, SigId id) { return s.KindOf(id) == SigKind::Int ? double(s.IntValue(id)) : s.RealValue(id); }
inline bool IsZeroNum(const Signals &s, SigId id) { return IsNum(s, id) && NumOf(s, id) == 0; }
inline bool IsOneNum(const Signals &s, SigId id) { return IsNum(s, id) && NumOf(s, id) == 1; }
inline bool IsMinusOne(const Signals &s, SigId id) { return IsNum(s, id) && NumOf(s, id) == -1; }

// `add_normal_form` reaches only a binary operation no rule rewrote. `pow` normalizes
// either way.
std::vector<SigId> Simplify(Signals &, std::span<const SigId> roots, bool add_normal_form);

// Promote, simplify, promote, then clamp table accesses once sizes have folded.
std::vector<SigId> Normalize(Signals &, std::span<const SigId> roots, bool add_normal_form);

} // namespace faustlens
