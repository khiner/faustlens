// Control edits update both the running instance and persistent session values.
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
    bool Ended = false; // ImGui gesture completion
    std::optional<uint32_t> Traced;
};

Report Draw(const Plan &, const UiNode &, Interp &dsp, Values &store);

} // namespace controls
} // namespace faustlens
