// Support hidden, style, scale, unit, and tooltip metadata.
#pragma once

#include "signal/Ui.h"

#include <cstdint>
#include <string>

namespace faustlens::controls {

enum class Scale : uint8_t { Linear, Log, Exp };

struct Style {
    bool Hidden = false;
    bool Knob = false;
    Scale Scale = Scale::Linear;
    std::string Unit, Tooltip;
};

Style StyleOf(const UiNode &);

// `t` is in [0, 1]; log endpoints floor at DBL_EPSILON as in reference ValueConverter.h.
double ToValue(const UiNode &, Scale, double t);
double ToPosition(const UiNode &, Scale, double v);

// Quantize to the declared bounds and step.
double Quantize(const UiNode &, double v);

// Format with the declared step precision and unit.
std::string Format(const UiNode &, const Style &, double v);

} // namespace faustlens::controls
