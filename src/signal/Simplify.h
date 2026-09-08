// Run simplification after promotion to preserve literal nature.
#pragma once

#include "signal/Signal.h"

#include <initializer_list>
#include <span>
#include <vector>

namespace faustlens {

// Return the rewritten node id.
SigId SimpBinOp(Signals &, BinOpCode, SigId, SigId);
SigId SimpIntCast(Signals &, SigId);
SigId SimpFloatCast(Signals &, SigId);
SigId SimpSelect2(Signals &, SigId sel, SigId a, SigId b);
SigId SimpControl(Signals &, SigId, SigId cond);

// Simplify constant and nested delays while preserving current-sample projections.
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

// Apply optional additive normalization to unchanged binary nodes; always normalize pow.
std::vector<SigId> Simplify(Signals &, std::span<const SigId> roots, bool add_normal_form);

// Promote, simplify, promote, then clamp table accesses.
std::vector<SigId> Normalize(Signals &, std::span<const SigId> roots, bool add_normal_form);

} // namespace faustlens
