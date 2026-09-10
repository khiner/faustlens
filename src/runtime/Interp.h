#pragma once

#include "runtime/Instance.h"

#include <array>

namespace faustlens {

struct Interp final : Instance {
    struct Code {
        std::vector<Instr> In;
        // Jump targets follow the matching end for begin instructions and point to the matching begin for end instructions.
        std::vector<uint32_t> Jump;
    };

    std::array<Code, 3> Bands;
    std::vector<Scalar> Regs;

    Interp(const faustlens::Plan &, const UiNode &, const faustlens::Registry & = Registry::Builtin());
    Code &Band(Band b) { return Bands[size_t(b)]; }

private:
    void Execute(bool initialize, int32_t, const double *const *, double *const *) override;
    void Prepare(Code &, std::span<const Instr>);
    void Specialize();
    void Run(const Code &, const double *const *, double *const *, int32_t frame);
};

} // namespace faustlens
