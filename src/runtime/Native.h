#pragma once

#include "runtime/Executable.h"
#include "runtime/Instance.h"

#include <array>

namespace faustlens {

class Native final : public Instance {
public:
    static std::expected<std::unique_ptr<Native>, std::string>
    Compile(const faustlens::Plan &, const UiNode &, const faustlens::Registry & = Registry::Builtin());
    size_t CodeBytes() const { return Code->Size(); }

private:
    using Instance::Instance;
    struct Parameters {
        double SampleRate;
        int32_t Frames;
        Scalar *Scratch;
        const Instance *Owner;
    };
    using Entry = void (*)(Scalar *values, Scalar *state, const double *const *in, double *const *out, int32_t frames, const Parameters *);
    std::vector<Scalar> Scratch;
    std::unique_ptr<Executable> Code;
    std::array<Entry, 3> Entries;
    void Execute(Band, int32_t, const double *const *, double *const *) override;
};

} // namespace faustlens
