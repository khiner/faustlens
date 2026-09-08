// Map foreign names and signatures to native pointers, with instance-provided fSamplingFreq and count.
#pragma once

#include "signal/Plan.h"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace faustlens {

union Scalar {
    double D;
    int32_t I;
};

struct Symbol {
    ForeignKind Kind = ForeignKind::Function;
    Nature Result = Nature::Real;
    std::vector<Nature> Args;

    enum class Runtime : uint8_t { None, SampleRate, BlockSize };
    Runtime Provides = Runtime::None;

    // Read constant and variable values through Addr.
    void *Fn = nullptr;
    const void *Addr = nullptr;
};

struct Registry {
    std::map<std::string, Symbol> ByKey;

    static const Registry &Builtin();

    void AddFunction(const std::string &name, Nature result, std::vector<Nature> args, void *fn);
    void AddVariable(const std::string &name, Nature result, const void *addr);
    void AddRuntime(const std::string &name, ForeignKind, Nature result, Symbol::Runtime);

    // Return null for an unresolved symbol.
    const Symbol *Find(const ForeignDesc &) const;
};

// Check CanCall before invoking a scalar thunk of up to two arguments.
// Resolve Runtime symbols through the instance.
bool CanCall(const Symbol &);
Scalar Call(const Symbol &, std::span<const Scalar> args);

} // namespace faustlens
