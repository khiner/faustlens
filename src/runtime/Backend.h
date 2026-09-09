#pragma once

#include "runtime/Instance.h"

namespace faustlens {

enum class Backend { Interp, Native };
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr Backend DefaultBackend = Backend::Native;
#else
inline constexpr Backend DefaultBackend = Backend::Interp;
#endif

std::expected<std::unique_ptr<Instance>, std::string> CreateInstance(Backend, const Plan &, const UiNode &, const Registry & = Registry::Builtin());

} // namespace faustlens
