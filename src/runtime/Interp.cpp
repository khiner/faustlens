#include "runtime/Interp.h"

#include "eval/Fold.h" // `ToInt`, the saturating conversion the folder settles

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <type_traits>

namespace faustlens {

namespace {

// `Instr::op` is a byte so the pseudo-opcodes below can share the dispatch.
constexpr uint8_t OpByte(Op o) { return uint8_t(o); }

// Not an optimization: repeated multiplication differs from `std::pow` in the last bits.
constexpr uint8_t IPow = OpByte(Op::Count_);

// A UI-bound field crosses the thread boundary both ways, control in and bargraph out,
// so both ends are atomic. Relaxed: nothing depends on when a control lands.
constexpr uint8_t LoadUi = OpByte(Op::Count_) + 1;
constexpr uint8_t StoreUi = OpByte(Op::Count_) + 2;

constexpr std::memory_order Relaxed = std::memory_order_relaxed;

std::atomic_ref<double> UiAt(Scalar &s) { return std::atomic_ref<double>(s.D); }

// Integer arithmetic must wrap (`noise` is built on it), and `uint32_t` avoids the UB.
int32_t Wrap(uint32_t v) { return IntOf(v); }

} // namespace

Interp::Interp(const faustlens::Plan &p, const UiNode &ui, const faustlens::Registry &reg) : Plan(p), Registry(reg) {
    for (size_t b = 0; b < Bands.size(); ++b) Prepare(Bands[b], p.Bands[b]);

    Regs.assign(p.Regs, Scalar{});
    RegNature.assign(p.Regs, Nature::Real);
    for (const Code &c : Bands)
        for (const Instr &i : c.In)
            if (i.Dst != NoReg) RegNature[i.Dst] = i.Nature;

    FieldAt.resize(p.Fields.size());
    uint32_t at = 0;
    for (size_t f = 0; f < p.Fields.size(); ++f) {
        FieldAt[f] = at;
        at += std::max<uint32_t>(1, p.Fields[f].Extent);
    }
    State.assign(at, Scalar{});

    InitWritesField.assign(p.Fields.size(), 0);
    InitWritesReg.assign(p.Regs, 0);
    for (const Instr &i : Band(Band::Init).In) {
        if (Op(i.Op) == Op::StoreField && i.Imm < p.Fields.size()) InitWritesField[i.Imm] = 1;
        if (i.Dst != NoReg) InitWritesReg[i.Dst] = 1;
    }

    // An unresolved symbol is a diagnostic and reads back zero rather than failing the compile.
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

    Specialize();
}

// `pow` at a constant integer exponent, per the reference's `isIntPowArg`: an int
// literal at eight or under, a real literal integral in `[0, 8]`. After all bands, a
// literal and its `pow` landing in different ones.
void Interp::Specialize() {
    std::vector<const Instr *> def(Plan.Regs, nullptr);
    for (const Code &c : Bands)
        for (const Instr &i : c.In)
            if (i.Dst != NoReg) def[i.Dst] = &i;

    for (Code &c : Bands)
        for (Instr &i : c.In) {
            if (Op(i.Op) != Op::Extended || Ext(i.Form) != Ext::Pow || i.ArgCount != 2) continue;
            const Instr *e = def[Plan.Operands[i.Args + 1]];
            if (!e) continue;
            int32_t k = 0;
            if (Op(e->Op) == Op::ConstInt) {
                k = IntOf(e->Imm);
                if (k > 8) continue;
            } else if (Op(e->Op) == Op::ConstReal) {
                const double v = RealOf(e->Imm, e->Aux);
                double whole;
                if (std::modf(v, &whole) != 0.0 || v < 0 || v > 8) continue;
                k = int32_t(v);
            } else {
                continue;
            }
            i.Op = IPow;
            i.Aux = BitsOf(k);
            i.ArgCount = 1;
        }

    // Zones read and write `.d`, so an integer widget field stays on the ordinary path.
    for (Code &c : Bands)
        for (Instr &i : c.In) {
            const Op op = Op(i.Op);
            if (op != Op::LoadField && op != Op::StoreField) continue;
            if (i.Imm >= Plan.Fields.size()) continue;
            const Field &f = Plan.Fields[i.Imm];
            if (f.Kind != FieldKind::Widget || f.Nature != Nature::Real || f.Extent != 1) continue;
            i.Op = op == Op::LoadField ? LoadUi : StoreUi;
        }
}

void Interp::Prepare(Code &c, std::span<const Instr> src) {
    // The band is copied, not viewed: `Code` outlives the Plan band it is prepared from.
    c.In.assign(src.begin(), src.end());
    c.Jump.assign(src.size(), 0);
    std::vector<uint32_t> open;
    for (uint32_t pc = 0; pc < c.In.size(); ++pc) {
        switch (Op(c.In[pc].Op)) {
            case Op::LoopBegin:
            case Op::GuardBegin: open.push_back(pc); break;
            case Op::LoopEnd:
            case Op::GuardEnd:
                if (!open.empty()) {
                    c.Jump[open.back()] = pc + 1;
                    c.Jump[pc] = open.back();
                    open.pop_back();
                }
                break;
            case Op::BitCast: Diagnostics.emplace_back("no interpretation for a bitcast"); break;
            default: break;
        }
    }
}

void Interp::Constants(double rate) {
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
    Run(Band(Band::Init), nullptr, nullptr, 0);
}

void Interp::ResetControls() {
    for (const Zone &z : Zones)
        for (const uint32_t f : z.Fields) UiAt(State[FieldAt[f]]).store(z.Init, Relaxed);
}

void Interp::Clear() {
    // Not what `constants` computed, hence the init-writes test on top of the kind test.
    for (uint32_t f = 0; f < Plan.Fields.size(); ++f) {
        const Field &fd = Plan.Fields[f];
        if (fd.Kind != FieldKind::Delay && fd.Kind != FieldKind::Perm) continue;
        if (InitWritesField[f]) continue;
        for (uint32_t k = 0; k < fd.Extent; ++k) State[FieldAt[f] + k] = Scalar{};
    }
    for (uint32_t r = 0; r < Regs.size(); ++r)
        if (!InitWritesReg[r]) Regs[r] = Scalar{};
}

void Interp::Init(double rate) {
    Constants(rate);
    ResetControls();
    Clear();
}

void Interp::LoadSoundfiles(SoundfileReader *reader) {
    for (uint32_t f = 0; f < Plan.Fields.size(); ++f) {
        const Field &fd = Plan.Fields[f];
        if (fd.Kind != FieldKind::Soundfile || fd.Desc >= Plan.Soundfiles.size()) continue;
        uint32_t unresolved = 0;
        Sound[f] = LoadSoundfile(Plan.Soundfiles[fd.Desc], reader, unresolved);
        if (unresolved && reader) Diagnostics.push_back(std::format("{} unreadable file(s) for soundfile {}", unresolved, fd.Desc));
    }
}

void Interp::SetControl(uint32_t label, double value) {
    for (const Zone &z : Zones)
        if (z.Label == label)
            for (const uint32_t f : z.Fields) UiAt(State[FieldAt[f]]).store(value, Relaxed);
}

double Interp::Control(uint32_t label) const {
    for (const Zone &z : Zones)
        if (z.Label == label && !z.Fields.empty())
            // The `const` is the method's, not the storage's: the audio thread writes
            // bargraphs here while we read.
            return UiAt(const_cast<Scalar &>(State[FieldAt[z.Fields[0]]])).load(Relaxed);
    return 0;
}

std::vector<uint32_t> Interp::ControlsOfKind(UiKind k) const {
    std::vector<uint32_t> out;
    for (const Zone &z : Zones)
        if (z.Kind == k) out.push_back(z.Label);
    return out;
}

void Interp::Compute(int32_t n, const double *const *in, double *const *out) {
    Frames = n;
    Run(Band(Band::Control), in, out, 0);
    for (int32_t f = 0; f < Frames; ++f) Run(Band(Band::Sample), in, out, f);
}

void Interp::Run(const Code &c, const double *const *in, double *const *out, int32_t frame) {
    Scalar *R = Regs.data();
    const Reg *ops = Plan.Operands.data();
    const Instr *code = c.In.data();
    const size_t n = c.In.size();

    // A register's nature is fixed by its writer, so reading the other nature converts.
    const auto D = [&](Reg r) { return RegNature[r] == Nature::Int ? double(R[r].I) : R[r].D; };
    const auto I = [&](Reg r) { return RegNature[r] == Nature::Int ? R[r].I : ToInt(R[r].D); };
    const auto Write = [&](const Instr &n, auto v) {
        if (n.Nature != Nature::Int) R[n.Dst].D = double(v);
        else if constexpr (std::is_integral_v<decltype(v)>) R[n.Dst].I = v;
        else R[n.Dst].I = ToInt(v);
    };
    // The index is already proven in range. The clamp keeps a bug from becoming a memory bug.
    const auto Slot = [&](uint32_t field, uint32_t at) {
        const uint32_t extent = std::max<uint32_t>(1, Plan.Fields[field].Extent);
        return FieldAt[field] + std::min(at, extent - 1);
    };

    for (size_t pc = 0; pc < n;) {
        const Instr &i = code[pc];
        const Reg *a = ops + i.Args;
        switch (i.Op) {
            case OpByte(Op::ConstInt): R[i.Dst].I = IntOf(i.Imm); break;
            case OpByte(Op::ConstReal): R[i.Dst].D = RealOf(i.Imm, i.Aux); break;
            case OpByte(Op::Input): R[i.Dst].D = in && in[i.Imm] ? in[i.Imm][frame] : 0.0; break;
            case OpByte(Op::Output):
                if (out && out[i.Imm]) out[i.Imm][frame] = D(a[0]);
                break;

            case OpByte(Op::BinOp): {
                const BinOpCode b = BinOpCode(i.Form);
                if (RegNature[a[0]] == Nature::Int) {
                    const int32_t x = R[a[0]].I, y = R[a[1]].I;
                    int32_t v = 0;
                    switch (b) {
                        case BinOpCode::Add: v = Wrap(uint32_t(x) + uint32_t(y)); break;
                        case BinOpCode::Sub: v = Wrap(uint32_t(x) - uint32_t(y)); break;
                        case BinOpCode::Mul: v = Wrap(uint32_t(x) * uint32_t(y)); break;
                        // Corners C++ leaves undefined, answered as AArch64 does: div/rem by
                        // zero or by `-1` at `INT_MIN`, and a shift count modulo the width.
                        case BinOpCode::Div: v = y == 0 ? 0 : (y == -1 ? Wrap(-uint32_t(x)) : x / y); break;
                        case BinOpCode::Rem: v = y == 0 ? x : (y == -1 ? 0 : x % y); break;
                        case BinOpCode::LeftShift: v = Wrap(uint32_t(x) << (uint32_t(y) & 31)); break;
                        case BinOpCode::RightShift: v = x >> (uint32_t(y) & 31); break;
                        case BinOpCode::LRightShift: v = Wrap(uint32_t(x) >> (uint32_t(y) & 31)); break;
                        case BinOpCode::GT: v = x > y; break;
                        case BinOpCode::LT: v = x < y; break;
                        case BinOpCode::GE: v = x >= y; break;
                        case BinOpCode::LE: v = x <= y; break;
                        case BinOpCode::EQ: v = x == y; break;
                        case BinOpCode::NE: v = x != y; break;
                        case BinOpCode::AND: v = x & y; break;
                        case BinOpCode::OR: v = x | y; break;
                        case BinOpCode::XOR: v = x ^ y; break;
                        default: break;
                    }
                    Write(i, v);
                } else {
                    const double x = R[a[0]].D, y = D(a[1]);
                    switch (b) {
                        case BinOpCode::Add: R[i.Dst].D = x + y; break;
                        case BinOpCode::Sub: R[i.Dst].D = x - y; break;
                        case BinOpCode::Mul: R[i.Dst].D = x * y; break;
                        case BinOpCode::Div: R[i.Dst].D = x / y; break;
                        case BinOpCode::Rem: R[i.Dst].D = std::fmod(x, y); break;
                        case BinOpCode::GT: R[i.Dst].I = x > y; break;
                        case BinOpCode::LT: R[i.Dst].I = x < y; break;
                        case BinOpCode::GE: R[i.Dst].I = x >= y; break;
                        case BinOpCode::LE: R[i.Dst].I = x <= y; break;
                        case BinOpCode::EQ: R[i.Dst].I = x == y; break;
                        case BinOpCode::NE: R[i.Dst].I = x != y; break;
                        // Promotion casts the bitwise codes to int, so they never arrive here.
                        default: R[i.Dst].I = 0; break;
                    }
                }
                break;
            }

            case OpByte(Op::Extended): {
                const Ext e = Ext(i.Form);
                if (RegNature[a[0]] == Nature::Int && (e == Ext::Abs || e == Ext::Min || e == Ext::Max)) {
                    const int32_t x = R[a[0]].I;
                    const int32_t v = e == Ext::Abs ? (x == INT32_MIN ? x : (x < 0 ? -x : x)) : e == Ext::Min ? std::min(x, R[a[1]].I) : std::max(x, R[a[1]].I);
                    Write(i, v);
                    break;
                }
                const double x = D(a[0]);
                const double y = i.ArgCount > 1 ? D(a[1]) : 0.0;
                double v = 0;
                switch (e) {
                    case Ext::Abs: v = std::fabs(x); break;
                    case Ext::Acos: v = std::acos(x); break;
                    case Ext::Acosh: v = std::acosh(x); break;
                    case Ext::Asin: v = std::asin(x); break;
                    case Ext::Asinh: v = std::asinh(x); break;
                    case Ext::Atan: v = std::atan(x); break;
                    case Ext::Atan2: v = std::atan2(x, y); break;
                    case Ext::Atanh: v = std::atanh(x); break;
                    case Ext::Ceil: v = std::ceil(x); break;
                    case Ext::Cos: v = std::cos(x); break;
                    case Ext::Cosh: v = std::cosh(x); break;
                    case Ext::Exp: v = std::exp(x); break;
                    case Ext::Floor: v = std::floor(x); break;
                    case Ext::Fmod: v = std::fmod(x, y); break;
                    case Ext::Log: v = std::log(x); break;
                    case Ext::Log10: v = std::log10(x); break;
                    case Ext::Max: v = std::max(x, y); break;
                    case Ext::Min: v = std::min(x, y); break;
                    case Ext::Pow: v = std::pow(x, y); break;
                    case Ext::Remainder: v = std::remainder(x, y); break;
                    case Ext::Rint: v = std::rint(x); break;
                    case Ext::Round: v = std::round(x); break;
                    case Ext::Sin: v = std::sin(x); break;
                    case Ext::Sinh: v = std::sinh(x); break;
                    case Ext::Sqrt: v = std::sqrt(x); break;
                    case Ext::Tan: v = std::tan(x); break;
                    case Ext::Tanh: v = std::tanh(x); break;
                    default: break;
                }
                Write(i, v);
                break;
            }

            case IPow: {
                const int32_t k = IntOf(i.Aux);
                if (RegNature[a[0]] == Nature::Int) {
                    const int32_t x = R[a[0]].I;
                    int32_t v = k == 0 ? 1 : x;
                    for (int32_t s = 0; s + 1 < k; ++s) v = Wrap(uint32_t(v) * uint32_t(x));
                    Write(i, v);
                } else {
                    const double x = R[a[0]].D;
                    double v = k == 0 ? 1.0 : x;
                    for (int32_t s = 0; s + 1 < k; ++s) v = v * x;
                    Write(i, v);
                }
                break;
            }

            case OpByte(Op::IntCast): R[i.Dst].I = I(a[0]); break;
            case OpByte(Op::FloatCast): R[i.Dst].D = D(a[0]); break;
            case OpByte(Op::BitCast): R[i.Dst] = Scalar{}; break;

            case OpByte(Op::Select2): R[i.Dst] = R[a[0]].I ? R[a[2]] : R[a[1]]; break;
            case OpByte(Op::Select3): R[i.Dst] = R[a[0]].I == 0 ? R[a[1]] : (R[a[0]].I == 1 ? R[a[2]] : R[a[3]]); break;

            case OpByte(Op::LoadField): R[i.Dst] = State[Slot(i.Imm, i.ArgCount ? uint32_t(I(a[0])) : 0)]; break;
            case LoadUi: R[i.Dst].D = UiAt(State[FieldAt[i.Imm]]).load(Relaxed); break;
            case StoreUi: UiAt(State[FieldAt[i.Imm]]).store(D(a[i.ArgCount - 1]), Relaxed); break;
            case OpByte(Op::StoreField): {
                const Reg v = a[i.ArgCount - 1];
                Scalar &at_slot = State[Slot(i.Imm, i.ArgCount > 1 ? uint32_t(I(a[0])) : 0)];
                if (Plan.Fields[i.Imm].Nature == Nature::Int) at_slot.I = I(v);
                else at_slot.D = D(v);
                break;
            }

            case OpByte(Op::SoundfileLength):
            case OpByte(Op::SoundfileRate): {
                const Soundfile &sf = *Sound[i.Imm];
                const uint32_t part = std::min<uint32_t>(uint32_t(I(a[0])), Soundfile::Parts - 1);
                R[i.Dst].I = i.Op == OpByte(Op::SoundfileLength) ? sf.Length[part] : sf.Rate[part];
                break;
            }
            case OpByte(Op::SoundfileRead): {
                const Soundfile &sf = *Sound[i.Imm];
                const uint32_t chan = std::min<uint32_t>(uint32_t(I(a[0])), uint32_t(sf.Channel.size()) - 1);
                const uint32_t part = std::min<uint32_t>(uint32_t(I(a[1])), Soundfile::Parts - 1);
                // The graph has already clamped `i` to the part's length.
                const uint32_t at_sample = uint32_t(sf.Offset[part]) + uint32_t(I(a[2]));
                R[i.Dst].D = sf.Channel[chan][std::min<size_t>(at_sample, sf.Owned[0].size() - 1)];
                break;
            }

            case OpByte(Op::FConst):
            case OpByte(Op::FVar):
            case OpByte(Op::FFun): {
                const faustlens::Symbol *s = Symbol[i.Imm];
                if (!s) {
                    R[i.Dst] = Scalar{};
                    break;
                }
                if (s->Provides == Symbol::Runtime::SampleRate) {
                    Write(i, SampleRate);
                    break;
                }
                if (s->Provides == Symbol::Runtime::BlockSize) {
                    Write(i, Frames);
                    break;
                }
                Scalar args[2]{};
                for (uint32_t k = 0; k < i.ArgCount && k < 2; ++k) {
                    if (s->Args[k] == Nature::Int) args[k].I = I(a[k]);
                    else args[k].D = D(a[k]);
                }
                R[i.Dst] = Call(*s, {args, i.ArgCount});
                break;
            }

            case OpByte(Op::LoopBegin):
                if (i.Imm == 0) {
                    pc = c.Jump[pc];
                    continue;
                }
                R[i.Dst].I = 0;
                break;
            case OpByte(Op::LoopEnd): {
                const uint32_t begin = c.Jump[pc];
                Scalar &k = R[code[begin].Dst];
                if (uint32_t(++k.I) < code[begin].Imm) {
                    pc = begin + 1;
                    continue;
                }
                break;
            }
            case OpByte(Op::GuardBegin):
                if (R[a[0]].I == 0) {
                    pc = c.Jump[pc];
                    continue;
                }
                break;
            case OpByte(Op::GuardEnd): break;
            default: break;
        }
        ++pc;
    }
}

} // namespace faustlens
