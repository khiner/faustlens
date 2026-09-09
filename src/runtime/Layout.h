#pragma once

#include "signal/Plan.h"

namespace faustlens {

// Retain values read by another band or defined conditionally across invocations.
struct RegisterLayout {
    std::vector<Reg> Slot, Persistent;
    std::vector<Nature> Types;
    std::vector<uint8_t> Init;
    explicit RegisterLayout(const Plan &);
};

struct InstanceLayout {
    RegisterLayout Registers;
    std::vector<uint32_t> FieldAt;
    std::vector<uint8_t> InitWritesField;
    uint32_t StateSize = 0;
    explicit InstanceLayout(const Plan &);
};

} // namespace faustlens
