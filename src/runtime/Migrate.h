#pragma once

#include "signal/Plan.h"

#include <cstdint>
#include <span>
#include <vector>

namespace faustlens {

struct Instance;

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

// Match delay and scalar state off the audio thread with one source offset per Plan field.
StateTransfer MatchState(const Plan &, std::span<const uint32_t> old_at, const Plan &, std::span<const uint32_t> new_at);
// Copy preselected fields without allocation.
void TransferState(const StateTransfer &, const Instance &from, Instance &to);

// Copy matched state using per-field source offsets captured at compile time.
Migration
Migrate(const Plan &old_plan, const Instance &from, std::span<const uint32_t> old_at, const Plan &new_plan, Instance &to, std::span<const uint32_t> new_at);

} // namespace faustlens
