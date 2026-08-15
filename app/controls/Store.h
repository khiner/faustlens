// Control values held beside the session, so an edit cannot reset a moved slider. Keyed
// by label path text.
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

// A stored value applies only within its class: a stored 1 for a momentary
// button would be a button stuck down.
enum class Restorable : uint8_t { No, Continuous, Toggle };
Restorable RestorableAs(UiKind);

struct Value {
    double V = 0;
    Restorable As = Restorable::No;

    bool operator==(const Value &) const = default;
};

// Entries never expire within a session, since answering for a widget the
// current program lacks is the point.
using Values = std::map<std::string, Value, std::less<>>;

// A widget with no restorable class is not recorded.
void Record(Values &, std::string_view path, const UiNode &, double v);

// Every restorable widget written exactly once, from the store or its init, so it is
// safe on a playing instance. Bounds re-apply only to a stored value.
void Apply(const Values &, const Plan &, const UiNode &, Interp &);

} // namespace controls
} // namespace faustlens
