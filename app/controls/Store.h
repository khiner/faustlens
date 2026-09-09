#pragma once

#include "signal/Plan.h"
#include "signal/Ui.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace faustlens {

struct Instance;

namespace controls {

enum class Restorable : uint8_t { No, Continuous, Toggle };
Restorable RestorableAs(UiKind);

struct Value {
    double V = 0;
    Restorable As = Restorable::No;

    bool operator==(const Value &) const = default;
};

// Values persist by label path, including controls absent from the current program.
using Values = std::map<std::string, Value, std::less<>>;

void Record(Values &, std::string_view path, const UiNode &, double v);

// Restore values from the same control class within current bounds, or use the declared initial value.
void Apply(const Values &, const Plan &, const UiNode &, Instance &);

} // namespace controls
} // namespace faustlens
