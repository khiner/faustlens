#pragma once

#include "controls/Store.h"
#include "signal/Plan.h"
#include "signal/Ui.h"

#include <cstdint>
#include <optional>

namespace faustlens {

struct Instance;

namespace controls {

struct Report {
    bool Ended = false; // ImGui gesture completion
    std::optional<uint32_t> Traced;
};

// Update both the running instance and persistent control values.
Report Draw(const Plan &, const UiNode &, Instance &dsp, Values &store);

} // namespace controls
} // namespace faustlens
