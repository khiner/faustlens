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
        // Begin targets the instruction after End; End targets Begin.
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
    // Preserve init-band writes when Clear follows Constants.
    std::vector<uint8_t> InitWritesField, InitWritesReg;
    std::vector<Zone> Zones;
    std::vector<const faustlens::Symbol *> Symbol;
    std::vector<std::shared_ptr<const Soundfile>> Sound;
    std::vector<std::string> Diagnostics;
    double SampleRate = 44100;
    int32_t Frames = 0;

    // Plan and Registry must outlive the instance; ui is read during construction only.
    Interp(const faustlens::Plan &, const UiNode &ui, const faustlens::Registry & = Registry::Builtin());

    void Constants(double sample_rate);
    void ResetControls();
    void Clear();
    void Init(double sample_rate);

    // Treat null input channels as silence.
    void Compute(int32_t frames, const double *const *in, double *const *out);

    // Bind soundfiles before Init, substituting silence for missing files.
    void LoadSoundfiles(SoundfileReader *);

    // Write every field sharing the label path.
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
