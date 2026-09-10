#include "runtime/Instance.h"

#include "eval/Fold.h"

#include <algorithm>
#include <atomic>
#include <format>

namespace faustlens {
namespace {
constexpr std::memory_order Relaxed = std::memory_order_relaxed;
std::atomic_ref<double> UiAt(Scalar &s) { return std::atomic_ref<double>(s.D); }
} // namespace

Instance::Instance(const faustlens::Plan &p, const UiNode &ui, const faustlens::Registry &reg, const InstanceLayout *layout)
    : OwnedLayout(layout ? nullptr : std::make_unique<InstanceLayout>(p)), Layout(layout ? *layout : *OwnedLayout), Plan(p), Registry(reg),
      Values(Registers.Persistent.size()), State(Layout.StateSize) {
    Symbol.assign(p.Foreign.size(), nullptr);
    for (size_t i = 0; i < p.Foreign.size(); ++i) {
        const ForeignDesc &d = p.Foreign[i];
        const faustlens::Symbol *s = Registry.Find(d);
        if (s && CanCall(*s)) {
            Symbol[i] = s;
            continue;
        }
        Diagnostics.push_back(s ? "no thunk shape for " + d.Name : "no registered symbol for " + d.Name);
    }

    Sound.assign(p.Fields.size(), nullptr);
    LoadSoundfiles(nullptr);

    ForEachWidget(ui, [&](const UiNode &w) {
        const uint32_t label = w.WidgetLabel;
        if (std::ranges::any_of(Zones, [&](const Zone &z) { return z.Label == label; })) return true;
        Zone z;
        z.Label = label;
        z.Kind = w.Kind;
        z.Init = w.Init;
        for (uint32_t f = 0; f < p.Fields.size(); ++f)
            if (p.Fields[f].Kind == FieldKind::Widget && p.Fields[f].Label == label) z.Fields.push_back(f);
        Zones.push_back(std::move(z));
        return true;
    });
}

void Instance::Constants(double rate) {
    SampleRate = rate;
    for (uint32_t f = 0; f < Plan.Fields.size(); ++f) {
        const Field &fd = Plan.Fields[f];
        if ((fd.Kind == FieldKind::Delay || fd.Kind == FieldKind::Perm) && InitWritesField[f]) std::ranges::fill(FieldState(f), Scalar{});
        if (fd.Kind != FieldKind::Table || fd.Desc == NoDesc) continue;
        const std::vector<double> &w = Plan.Waves[fd.Desc];
        for (uint32_t k = 0; k < fd.Extent && k < w.size(); ++k) {
            if (fd.Nature == Nature::Int) State[FieldAt[f] + k].I = ToInt(w[k]);
            else State[FieldAt[f] + k].D = w[k];
        }
    }
    Execute(true, 1, nullptr, nullptr);
}

void Instance::ResetControls() {
    for (const Zone &z : Zones)
        for (const uint32_t f : z.Fields) UiAt(State[FieldAt[f]]).store(z.Init, Relaxed);
}

void Instance::Clear() {
    for (uint32_t f = 0; f < Plan.Fields.size(); ++f) {
        const Field &fd = Plan.Fields[f];
        if (fd.Kind != FieldKind::Delay && fd.Kind != FieldKind::Perm) continue;
        if (InitWritesField[f]) continue;
        for (uint32_t k = 0; k < fd.Extent; ++k) State[FieldAt[f] + k] = Scalar{};
    }
    for (uint32_t k = 0; k < Values.size(); ++k)
        if (!Registers.Init[k]) Values[k] = Scalar{};
}

void Instance::Init(double rate) {
    Constants(rate);
    ResetControls();
    Clear();
}

void Instance::LoadSoundfiles(SoundfileReader *reader) {
    for (uint32_t f = 0; f < Plan.Fields.size(); ++f) {
        const Field &fd = Plan.Fields[f];
        if (fd.Kind != FieldKind::Soundfile || fd.Desc >= Plan.Soundfiles.size()) continue;
        uint32_t unresolved = 0;
        Sound[f] = LoadSoundfile(Plan.Soundfiles[fd.Desc], reader, unresolved);
        if (unresolved && reader) Diagnostics.push_back(std::format("{} unreadable file(s) for soundfile {}", unresolved, fd.Desc));
    }
}

void Instance::SetControl(uint32_t label, double value) {
    for (const Zone &z : Zones)
        if (z.Label == label)
            for (const uint32_t f : z.Fields) UiAt(State[FieldAt[f]]).store(value, Relaxed);
}

double Instance::Control(uint32_t label) const {
    for (const Zone &z : Zones)
        if (z.Label == label && !z.Fields.empty())
            // The audio thread can update bargraphs during this read.
            return UiAt(const_cast<Scalar &>(State[FieldAt[z.Fields[0]]])).load(Relaxed);
    return 0;
}

std::vector<uint32_t> Instance::ControlsOfKind(UiKind k) const {
    std::vector<uint32_t> out;
    for (const Zone &z : Zones)
        if (z.Kind == k) out.push_back(z.Label);
    return out;
}

void Instance::Compute(int32_t n, const double *const *in, double *const *out) {
    Frames = std::max(n, 0);
    Execute(false, Frames, in, out);
}

} // namespace faustlens
