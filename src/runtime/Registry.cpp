#include "runtime/Registry.h"

#include <cmath>
#include <format>
#include <span>
#include <stdexcept>

namespace faustlens {

uintptr_t MathAddress(Ext form) {
    switch (form) {
        case Ext::Acos: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::acos));
        case Ext::Acosh: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::acosh));
        case Ext::Asin: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::asin));
        case Ext::Asinh: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::asinh));
        case Ext::Atan: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::atan));
        case Ext::Atan2: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double, double)>(std::atan2));
        case Ext::Atanh: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::atanh));
        case Ext::Cos: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::cos));
        case Ext::Cosh: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::cosh));
        case Ext::Exp: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::exp));
        case Ext::Fmod: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double, double)>(std::fmod));
        case Ext::Log: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::log));
        case Ext::Log10: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::log10));
        case Ext::Pow: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double, double)>(std::pow));
        case Ext::Remainder: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double, double)>(std::remainder));
        case Ext::Sin: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::sin));
        case Ext::Sinh: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::sinh));
        case Ext::Tan: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::tan));
        case Ext::Tanh: return reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(std::tanh));
        default: throw std::runtime_error("invalid native math operation");
    }
}

namespace {

std::string Key(ForeignKind kind, const std::string &name, Nature result, std::span<const Nature> args) {
    std::string k = std::format("{} {}(", int(kind), name);
    for (size_t i = 0; i < args.size(); ++i) k += std::format("{}{}", i ? "," : "", args[i] == Nature::Int ? "i" : "r");
    return k + ")" + (result == Nature::Int ? "i" : "r");
}

// Return int to match the math.lib foreign declaration.
int32_t IsNan(double x) { return std::isnan(x) ? 1 : 0; }
int32_t IsInf(double x) { return std::isinf(x) ? 1 : 0; }

template<class R> R Invoke(const Symbol &s, std::span<const Scalar> a) {
    switch (s.Args.size()) {
        case 0: return reinterpret_cast<R (*)()>(s.Fn)();
        case 1: return s.Args[0] == Nature::Real ? reinterpret_cast<R (*)(double)>(s.Fn)(a[0].D) : reinterpret_cast<R (*)(int32_t)>(s.Fn)(a[0].I);
        case 2:
            if (s.Args[0] == Nature::Real)
                return s.Args[1] == Nature::Real ? reinterpret_cast<R (*)(double, double)>(s.Fn)(a[0].D, a[1].D) :
                                                   reinterpret_cast<R (*)(double, int32_t)>(s.Fn)(a[0].D, a[1].I);
            return s.Args[1] == Nature::Real ? reinterpret_cast<R (*)(int32_t, double)>(s.Fn)(a[0].I, a[1].D) :
                                               reinterpret_cast<R (*)(int32_t, int32_t)>(s.Fn)(a[0].I, a[1].I);
        default: return R{};
    }
}

} // namespace

void Registry::AddFunction(const std::string &name, Nature result, std::vector<Nature> args, void *fn) {
    Symbol s{.Kind = ForeignKind::Function, .Result = result, .Args = std::move(args), .Fn = fn};
    ByKey[Key(s.Kind, name, s.Result, s.Args)] = std::move(s);
}

void Registry::AddVariable(const std::string &name, Nature result, const void *addr) {
    Symbol s{.Kind = ForeignKind::Variable, .Result = result, .Addr = addr};
    ByKey[Key(s.Kind, name, s.Result, s.Args)] = std::move(s);
}

void Registry::AddRuntime(const std::string &name, ForeignKind kind, Nature result, Symbol::Runtime runtime) {
    Symbol s{.Kind = kind, .Result = result, .Provides = runtime};
    ByKey[Key(s.Kind, name, s.Result, s.Args)] = std::move(s);
}

const Symbol *Registry::Find(const ForeignDesc &d) const {
    const auto it = ByKey.find(Key(d.Kind, d.Name, d.Result, d.Args));
    return it == ByKey.end() ? nullptr : &it->second;
}

std::string Registry::CheckMath(uint64_t required) const {
    if (!required) return {};
    for (const Ext op : BuiltinMath) {
        const uint64_t bit{uint64_t{1} << uint8_t(op)};
        if (!(required & bit)) continue;
        const ForeignDesc desc{ForeignKind::Function, std::string(ExtName(op)), Nature::Real, {Nature::Real}};
        const auto *symbol{Find(desc)};
        if (!symbol || symbol->Kind != desc.Kind || symbol->Result != desc.Result || symbol->Args != desc.Args || symbol->Provides != Symbol::Runtime::None ||
            reinterpret_cast<uintptr_t>(symbol->Fn) != MathAddress(op))
            return "math binding differs from compiled intrinsic: " + desc.Name;
        required &= ~bit;
    }
    return required ? "invalid compiled math binding requirement" : "";
}

const Registry &Registry::Builtin() {
    static const Registry r = [] {
        Registry b;
        b.AddRuntime("fSamplingFreq", ForeignKind::Constant, Nature::Int, Symbol::Runtime::SampleRate);
        b.AddRuntime("count", ForeignKind::Variable, Nature::Int, Symbol::Runtime::BlockSize);
        const std::vector<Nature> r1{Nature::Real}, r2{Nature::Real, Nature::Real};
        for (const Ext op : BuiltinMath) b.AddFunction(std::string(ExtName(op)), Nature::Real, r1, reinterpret_cast<void *>(MathAddress(op)));
        b.AddFunction("copysign", Nature::Real, r2, reinterpret_cast<void *>(static_cast<double (*)(double, double)>(std::copysign)));
        b.AddFunction("isnan", Nature::Int, r1, reinterpret_cast<void *>(&IsNan));
        b.AddFunction("isinf", Nature::Int, r1, reinterpret_cast<void *>(&IsInf));
        return b;
    }();
    return r;
}

bool CanCall(const Symbol &s) {
    if (s.Provides != Symbol::Runtime::None) return true;
    if (s.Kind != ForeignKind::Function) return s.Addr != nullptr;
    return s.Fn != nullptr && s.Args.size() <= 2;
}

Scalar Call(const Symbol &s, std::span<const Scalar> a) {
    Scalar out{};
    if (s.Kind != ForeignKind::Function) {
        if (s.Addr) {
            if (s.Result == Nature::Int) out.I = *static_cast<const int32_t *>(s.Addr);
            else out.D = *static_cast<const double *>(s.Addr);
        }
        return out;
    }
    if (s.Result == Nature::Int) out.I = Invoke<int32_t>(s, a);
    else out.D = Invoke<double>(s, a);
    return out;
}

} // namespace faustlens
