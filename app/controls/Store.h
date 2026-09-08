// Store control values by label path across program edits.
#pragma once

#include "signal/Plan.h"
#include "signal/Ui.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace faustlens {

struct Interp;

namespace controls {

// Restore values only within the same control class.
enum class Restorable : uint8_t { No, Continuous, Toggle };
Restorable RestorableAs(UiKind);

struct Value {
    double V = 0;
    Restorable As = Restorable::No;

    bool operator==(const Value &) const = default;
};

// Retain values for controls absent from the current program.
using Values = std::map<std::string, Value, std::less<>>;

void Record(Values &, std::string_view path, const UiNode &, double v);

// Write each restorable widget once, using its stored value or initial value.
// Reapply bounds to stored values only.
void Apply(const Values &, const Plan &, const UiNode &, Interp &);

} // namespace controls
} // namespace faustlens
