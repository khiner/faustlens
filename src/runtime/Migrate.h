// Match state by content hash, then by shape and source proximity.
// Exclude tables, waveforms, soundfiles, and UI values.
#pragma once

#include "signal/Plan.h"

#include <cstdint>
#include <span>
#include <vector>

namespace faustlens {

struct Interp;

// Use a distinct sentinel for fields without source locations.
inline constexpr uint32_t Nowhere = 0xFFFFFFFFu;

struct Migration {
    int Exact = 0;
    int Shaped = 0;
    int Fresh = 0;
    int Resized = 0;
};

struct StateTransfer {
    Migration Counts;
    std::vector<std::pair<uint32_t, uint32_t>> Fields; // old field index, new field index
};

// Match fields off the audio thread.
StateTransfer MatchState(const Plan &, std::span<const uint32_t> old_at, const Plan &, std::span<const uint32_t> new_at);
// Copy preselected fields without allocation.
void TransferState(const StateTransfer &, const Interp &from, Interp &to);

// Copy matched state using per-field source offsets captured at compile time.
Migration
Migrate(const Plan &old_plan, const Interp &from, std::span<const uint32_t> old_at, const Plan &new_plan, Interp &to, std::span<const uint32_t> new_at);

} // namespace faustlens
