// A register machine over the Plan, plus the instance lifecycle.
#pragma once

#include "runtime/Registry.h"
#include "runtime/Soundfile.h"
#include "signal/Plan.h"
#include "signal/Ui.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace faustlens {

struct Interp {
    struct Code {
        std::vector<Instr> In;
        // For a `Begin`, one past its matching `End`. For an `End`, its `Begin`.
        std::vector<uint32_t> Jump;
    };

    struct Zone {
        uint32_t Label = 0;
        UiKind Kind = UiKind::Button;
        double Init = 0;
        std::vector<uint32_t> Fields;
    };

    const Plan &Plan;
    const Registry &Registry;
    std::array<Code, 3> Bands;
    std::vector<Scalar> Regs;
    std::vector<Nature> RegNature;
    std::vector<Scalar> State;
    std::vector<uint32_t> FieldAt;
    // `Clear` runs after `Constants`, so what the init band wrote must survive it.
    std::vector<uint8_t> InitWritesField, InitWritesReg;
    std::vector<Zone> Zones;
    std::vector<const faustlens::Symbol *> Symbol; // per foreign descriptor
    std::vector<std::shared_ptr<const Soundfile>> Sound; // per field
    std::vector<std::string> Diagnostics;
    double SampleRate = 44100;
    int32_t Frames = 0; // this block's `count`

    // The Plan and the registry must outlive the instance. `ui` is read only here.
    Interp(const faustlens::Plan &, const UiNode &ui, const faustlens::Registry & = Registry::Builtin());

    void Constants(double sample_rate);
    void ResetControls();
    void Clear();
    void Init(double sample_rate);

    // A missing input channel may be null and reads as silence.
    void Compute(int32_t frames, const double *const *in, double *const *out);

    // Call before `Init`. Leaves no soundfile field null.
    void LoadSoundfiles(SoundfileReader *);

    // A label path may name more than one field and a write reaches all of them.
    void SetControl(uint32_t label, double value);
    double Control(uint32_t label) const;
    std::vector<uint32_t> ControlsOfKind(UiKind) const;

    std::span<Scalar> FieldState(uint32_t f) { return {State.data() + FieldAt[f], Plan.Fields[f].Extent}; }
    std::span<const Scalar> FieldState(uint32_t f) const { return {State.data() + FieldAt[f], Plan.Fields[f].Extent}; }

    int32_t Inputs() const { return Plan.Inputs; }
    int32_t Outputs() const { return Plan.Outputs; }

    Code &Band(Band b) { return Bands[size_t(b)]; }

    void Prepare(Code &, std::span<const Instr>);
    void Specialize();
    void Run(const Code &, const double *const *in, double *const *out, int32_t frame);
};

} // namespace faustlens
