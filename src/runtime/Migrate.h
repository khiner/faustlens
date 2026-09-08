// State preservation across a reload: exact content hash, same shape by source
// proximity, then fresh. Tables, waveforms, soundfiles and UI are excluded.
#pragma once

#include "signal/Plan.h"

#include <cstdint>
#include <span>
#include <vector>

namespace faustlens {

struct Interp;

// The `*_at` offset of a field with no term of origin. Position zero would make it a
// nearest neighbour of everything.
inline constexpr uint32_t Nowhere = 0xFFFFFFFFu;

struct Migration {
    int Exact = 0;
    int Shaped = 0;
    int Fresh = 0;
    int Resized = 0; // matched, and the length rule applied
};

struct StateTransfer {
    Migration Counts;
    std::vector<std::pair<uint32_t, uint32_t>> Fields; // old, new
};

// Matching allocates off the audio thread. Applying only copies preselected fields.
StateTransfer MatchState(const Plan &, std::span<const uint32_t> old_at, const Plan &, std::span<const uint32_t> new_at);
void TransferState(const StateTransfer &, const Interp &from, Interp &to);

// Copies `from`'s state into `to`. `old_at`/`new_at` are per-field source offsets from
// build time.
Migration
Migrate(const Plan &old_plan, const Interp &from, std::span<const uint32_t> old_at, const Plan &new_plan, Interp &to, std::span<const uint32_t> new_at);

} // namespace faustlens
