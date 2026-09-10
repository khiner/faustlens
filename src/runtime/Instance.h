#pragma once

#include "runtime/Layout.h"
#include "runtime/Registry.h"
#include "runtime/Soundfile.h"
#include "signal/Ui.h"

#include <memory>

namespace faustlens {

struct Instance {
private:
    std::unique_ptr<const InstanceLayout> OwnedLayout;
    const InstanceLayout &Layout;

public:
    const RegisterLayout &Registers = Layout.Registers;
    const std::vector<uint32_t> &FieldAt = Layout.FieldAt;
    const std::vector<uint8_t> &InitWritesField = Layout.InitWritesField;

    struct Zone {
        uint32_t Label = 0;
        UiKind Kind = UiKind::Button;
        double Init = 0;
        std::vector<uint32_t> Fields;
    };

    const Plan &Plan;
    const Registry &Registry;
    std::vector<Scalar> Values, State;
    std::vector<Zone> Zones;
    std::vector<const faustlens::Symbol *> Symbol;
    std::vector<std::shared_ptr<const Soundfile>> Sound;
    std::vector<std::string> Diagnostics;
    double SampleRate = 44100;
    int32_t Frames = 0;

    // Plan, Registry, and a supplied layout must outlive the instance.
    // A supplied layout must describe this Plan.
    Instance(const faustlens::Plan &, const UiNode &, const faustlens::Registry & = Registry::Builtin(), const InstanceLayout * = nullptr);
    virtual ~Instance() = default;
    Instance(const Instance &) = delete;
    Instance &operator=(const Instance &) = delete;

    void Constants(double sample_rate);
    void ResetControls();
    void Clear();
    void Init(double sample_rate);
    // Null input channels produce silence, and controls execute even for an empty block.
    void Compute(int32_t frames, const double *const *in, double *const *out);
    void LoadSoundfiles(SoundfileReader *);
    void SetControl(uint32_t label, double value);
    double Control(uint32_t label) const;
    std::vector<uint32_t> ControlsOfKind(UiKind) const;

    std::span<Scalar> FieldState(uint32_t f) { return {State.data() + FieldAt[f], Plan.Fields[f].Extent}; }
    std::span<const Scalar> FieldState(uint32_t f) const { return {State.data() + FieldAt[f], Plan.Fields[f].Extent}; }
    int32_t Inputs() const { return Plan.Inputs; }
    int32_t Outputs() const { return Plan.Outputs; }

protected:
    virtual void Execute(bool initialize, int32_t frames, const double *const *in, double *const *out) = 0;
};

} // namespace faustlens
