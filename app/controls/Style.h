// Honours `[hidden]`, `[style:knob]`, `[scale:log|exp]`, `[unit:]` and
// `[tooltip]`, ignoring every other key.
#pragma once

#include "signal/Ui.h"

#include <cstdint>
#include <string>

namespace faustlens::controls {

// The curve is the program's, so it follows the reference converters.
enum class Scale : uint8_t { Linear, Log, Exp };

struct Style {
    bool Hidden = false;
    bool Knob = false;
    Scale Scale = Scale::Linear;
    std::string Unit, Tooltip;
};

Style StyleOf(const UiNode &);

// `t` is normalized to `[0, 1]`. Log endpoints floor at `DBL_EPSILON`, as in
// the reference's `ValueConverter.h`.
double ToValue(const UiNode &, Scale, double t);
double ToPosition(const UiNode &, Scale, double v);

// Applies the declared `min`, `max` and `step`, which the reference does not.
double Quantize(const UiNode &, double v);

// `440 Hz`, `-6.00 dB`, `1`. Decimals come from the declared `step`.
std::string Format(const UiNode &, const Style &, double v);

} // namespace faustlens::controls
