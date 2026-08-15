// Foreign symbols: (name, signature) to a native pointer, plus the runtime symbols
// `fSamplingFreq` and `count` the instance answers.
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

    // `addr` is the address of a `fconstant`'s or `fvariable`'s value, read rather than called.
    void *Fn = nullptr;
    const void *Addr = nullptr;
};

struct Registry {
    std::map<std::string, Symbol> ByKey;

    static const Registry &Builtin();

    void AddFunction(const std::string &name, Nature result, std::vector<Nature> args, void *fn);
    void AddVariable(const std::string &name, Nature result, const void *addr);
    void AddRuntime(const std::string &name, ForeignKind, Nature result, Symbol::Runtime);

    // Null where nothing answers, which poisons the subgraph rather than failing the compile.
    const Symbol *Find(const ForeignDesc &) const;
};

// Thunk shapes cover arity zero to two over `int`/`double`, so check `CanCall` first.
// A `Runtime` symbol is the instance's to answer.
bool CanCall(const Symbol &);
Scalar Call(const Symbol &, std::span<const Scalar> args);

} // namespace faustlens
