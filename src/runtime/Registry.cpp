#include "runtime/Registry.h"

#include <cmath>
#include <format>
#include <span>

namespace faustlens {

namespace {

std::string Key(ForeignKind kind, const std::string &name, Nature result, std::span<const Nature> args) {
    std::string k = std::format("{} {}(", int(kind), name);
    for (size_t i = 0; i < args.size(); ++i) k += std::format("{}{}", i ? "," : "", args[i] == Nature::Int ? "i" : "r");
    return k + ")" + (result == Nature::Int ? "i" : "r");
}

double Acosh(double x) { return std::acosh(x); }
double Asinh(double x) { return std::asinh(x); }
double Atanh(double x) { return std::atanh(x); }
double Cosh(double x) { return std::cosh(x); }
double Sinh(double x) { return std::sinh(x); }
double Tanh(double x) { return std::tanh(x); }
double CopySign(double x, double y) { return std::copysign(x, y); }
// Declared `int` rather than `bool`, matching `math.lib`'s `ffunction`.
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
        default: return R{}; // arity three or more: `CanCall` said no
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

const Registry &Registry::Builtin() {
    static const Registry r = [] {
        Registry b;
        b.AddRuntime("fSamplingFreq", ForeignKind::Constant, Nature::Int, Symbol::Runtime::SampleRate);
        b.AddRuntime("count", ForeignKind::Variable, Nature::Int, Symbol::Runtime::BlockSize);
        const std::vector<Nature> r1{Nature::Real}, r2{Nature::Real, Nature::Real};
        b.AddFunction("acosh", Nature::Real, r1, reinterpret_cast<void *>(&Acosh));
        b.AddFunction("asinh", Nature::Real, r1, reinterpret_cast<void *>(&Asinh));
        b.AddFunction("atanh", Nature::Real, r1, reinterpret_cast<void *>(&Atanh));
        b.AddFunction("cosh", Nature::Real, r1, reinterpret_cast<void *>(&Cosh));
        b.AddFunction("sinh", Nature::Real, r1, reinterpret_cast<void *>(&Sinh));
        b.AddFunction("tanh", Nature::Real, r1, reinterpret_cast<void *>(&Tanh));
        b.AddFunction("copysign", Nature::Real, r2, reinterpret_cast<void *>(&CopySign));
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
