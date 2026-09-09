#include "runtime/Backend.h"
#include "runtime/Interp.h"
#include "runtime/Native.h"

namespace faustlens {

std::expected<std::unique_ptr<Instance>, std::string> CreateInstance(Backend backend, const Plan &p, const UiNode &ui, const Registry &registry) {
    if (backend == Backend::Interp) return std::make_unique<Interp>(p, ui, registry);
    return Native::Compile(p, ui, registry);
}

} // namespace faustlens
