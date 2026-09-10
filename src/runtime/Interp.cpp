#include "runtime/Interp.h"
#include "runtime/Math.h"

#include "eval/Fold.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <type_traits>

namespace faustlens {

namespace {

constexpr uint8_t OpByte(Op o) { return uint8_t(o); }

constexpr uint8_t IPow = OpByte(Op::Count_);

// Use relaxed atomics for control and bargraph scalars shared with the UI thread.
constexpr uint8_t LoadUi = OpByte(Op::Count_) + 1;
constexpr uint8_t StoreUi = OpByte(Op::Count_) + 2;

constexpr std::memory_order Relaxed = std::memory_order_relaxed;

std::atomic_ref<double> UiAt(Scalar &s) { return std::atomic_ref<double>(s.D); }

int32_t Wrap(uint32_t v) { return IntOf(v); }

} // namespace

Interp::Interp(const faustlens::Plan &p, const UiNode &ui, const faustlens::Registry &reg) : Instance(p, ui, reg), Regs(p.Regs) {
    for (size_t b = 0; b < Bands.size(); ++b) Prepare(Bands[b], p.Bands[b]);
    Specialize();
}

void Interp::Specialize() {
    std::vector<const Instr *> def(Plan.Regs, nullptr);
    for (const Code &c : Bands)
        for (const Instr &i : c.In)
            if (i.Dst != NoReg) def[i.Dst] = &i;

    for (Code &c : Bands)
        for (Instr &i : c.In) {
            if (Op(i.Op) != Op::Extended || Ext(i.Form) != Ext::Pow || i.ArgCount != 2) continue;
            const auto k = PowerExponent(def[Plan.Operands[i.Args + 1]]);
            if (!k) continue;
            i.Op = IPow;
            i.Aux = BitsOf(*k);
            i.ArgCount = 1;
        }

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

void Interp::Execute(bool initialize, int32_t frames, const double *const *in, double *const *out) {
    for (size_t k = 0; k < Values.size(); ++k) Regs[Registers.Persistent[k]] = Values[k];
    if (!initialize) Run(Band(faustlens::Band::Control), in, out, 0);
    const auto &code = Band(initialize ? faustlens::Band::Init : faustlens::Band::Sample);
    for (int32_t f = 0; f < frames; ++f) Run(code, in, out, f);
    for (size_t k = 0; k < Values.size(); ++k) Values[k] = Regs[Registers.Persistent[k]];
}

void Interp::Run(const Code &c, const double *const *in, double *const *out, int32_t frame) {
    Scalar *R = Regs.data();
    const Reg *ops = Plan.Operands.data();
    const Instr *code = c.In.data();
    const size_t n = c.In.size();

    const auto D = [&](Reg r) { return Registers.Types[r] == Nature::Int ? double(R[r].I) : R[r].D; };
    const auto I = [&](Reg r) { return Registers.Types[r] == Nature::Int ? R[r].I : ToInt(R[r].D); };
    const auto Write = [&](const Instr &n, auto v) {
        if (n.Nature != Nature::Int) R[n.Dst].D = double(v);
        else if constexpr (std::is_integral_v<decltype(v)>) R[n.Dst].I = v;
        else R[n.Dst].I = ToInt(v);
    };
    // Clamp as a memory-safety check after interval validation.
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
                if (Registers.Types[a[0]] == Nature::Int) {
                    Write(i, IntegerBinary(b, R[a[0]].I, R[a[1]].I));
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
                        default: R[i.Dst].I = 0; break;
                    }
                }
                break;
            }

            case OpByte(Op::Extended): {
                const Ext e = Ext(i.Form);
                if (Registers.Types[a[0]] == Nature::Int && (e == Ext::Abs || e == Ext::Min || e == Ext::Max)) {
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
                if (Registers.Types[a[0]] == Nature::Int) {
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
