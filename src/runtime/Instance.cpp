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

RegisterLayout::RegisterLayout(const Plan &p) : Slot(p.Regs, NoReg), Types(p.Regs, Nature::Real) {
    std::vector<int> defined(p.Regs, -1);
    std::vector<bool> retained(p.Regs, false), initialized(p.Regs, false);
    for (size_t b = 0; b < p.Bands.size(); ++b) {
        int guards = 0;
        for (const Instr &i : p.Bands[b]) {
            for (Reg r : p.Args(i))
                if (defined[r] >= 0 && defined[r] != int(b)) retained[r] = true;
            if (Op(i.Op) == Op::GuardBegin) ++guards;
            if (Op(i.Op) == Op::GuardEnd) --guards;
            if (i.Dst == NoReg) continue;
            defined[i.Dst] = int(b);
            Types[i.Dst] = i.Nature;
            initialized[i.Dst] = b == size_t(Band::Init);
            // Guarded definitions can retain their previous value across calls.
            if (guards) retained[i.Dst] = true;
        }
    }
    for (Reg r = 0; r < p.Regs; ++r)
        if (retained[r]) {
            Slot[r] = uint32_t(Persistent.size());
            Persistent.push_back(r);
            Init.push_back(initialized[r]);
        }
}

Instance::Instance(const faustlens::Plan &p, const UiNode &ui, const faustlens::Registry &reg)
    : Plan(p), Registry(reg), Registers(p), Values(Registers.Persistent.size()) {
    FieldAt.resize(p.Fields.size());
    uint32_t at = 0;
    for (size_t f = 0; f < p.Fields.size(); ++f) {
        FieldAt[f] = at;
        at += std::max<uint32_t>(1, p.Fields[f].Extent);
    }
    State.assign(at, Scalar{});

    InitWritesField.assign(p.Fields.size(), 0);
    for (const Instr &i : p.Band(Band::Init)) {
        if (Op(i.Op) == Op::StoreField && i.Imm < p.Fields.size()) InitWritesField[i.Imm] = 1;
    }

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
        if (fd.Kind != FieldKind::Table || fd.Desc == NoDesc) continue;
        const std::vector<double> &w = Plan.Waves[fd.Desc];
        for (uint32_t k = 0; k < fd.Extent && k < w.size(); ++k) {
            if (fd.Nature == Nature::Int) State[FieldAt[f] + k].I = ToInt(w[k]);
            else State[FieldAt[f] + k].D = w[k];
        }
    }
    Execute(Band::Init, 1, nullptr, nullptr);
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
    Execute(Band::Control, 1, in, out);
    Execute(Band::Sample, Frames, in, out);
}

} // namespace faustlens
