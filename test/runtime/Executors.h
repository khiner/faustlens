#pragma once

#include "runtime/Interp.h"
#include "runtime/Native.h"

#include "doctest.h"

#include <type_traits>

#if defined(__APPLE__) && defined(__aarch64__)
#define FAUSTLENS_TEST_EXECUTORS faustlens::Interp, faustlens::Native
#else
#define FAUSTLENS_TEST_EXECUTORS faustlens::Interp
#endif

namespace faustlens::test {

template<class Backend> std::unique_ptr<Instance> MakeExecutor(const Plan &p, const UiNode &ui, const Registry &registry = Registry::Builtin()) {
    if constexpr (std::is_same_v<Backend, Interp>) return std::make_unique<Interp>(p, ui, registry);
    else {
        auto native = Native::Compile(p, ui, registry);
        INFO((native ? "native" : native.error()));
        REQUIRE(native.has_value());
        return std::move(*native);
    }
}

} // namespace faustlens::test
