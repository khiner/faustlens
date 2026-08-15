// Every control move is written twice: to the instance, which makes a sound,
// and to the store, which outlives it.
#pragma once

#include "controls/Store.h"
#include "signal/Plan.h"
#include "signal/Ui.h"

#include <cstdint>
#include <optional>

namespace faustlens {

struct Interp;

namespace controls {

struct Report {
    bool Ended = false; // a gesture ended, in ImGui's sense not the mouse's
    std::optional<uint32_t> Traced; // the interned label right-clicked
};

// `plan` turns a widget's interned id into the path text keying `store`.
Report Draw(const Plan &, const UiNode &, Interp &dsp, Values &store);

} // namespace controls
} // namespace faustlens
