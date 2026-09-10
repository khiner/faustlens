#include "runtime/Native.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace faustlens {
namespace {

int32_t SoundLength(const Instance *dsp, uint32_t field, uint32_t part) { return dsp->Sound[field]->Length[std::min(part, Soundfile::Parts - 1)]; }

int32_t SoundRate(const Instance *dsp, uint32_t field, uint32_t part) { return dsp->Sound[field]->Rate[std::min(part, Soundfile::Parts - 1)]; }

double SoundRead(const Instance *dsp, uint32_t field, uint32_t channel, uint32_t part, uint32_t frame) {
    const Soundfile &sf = *dsp->Sound[field];
    const uint32_t at = uint32_t(sf.Offset[std::min(part, Soundfile::Parts - 1)]) + frame;
    return sf.Channel[std::min(channel, uint32_t(sf.Channel.size()) - 1)][std::min<size_t>(at, sf.Owned[0].size() - 1)];
}

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

void Bind(std::vector<uint32_t> &words, const arm64::Relocation &r, uintptr_t address) {
    const uint32_t reg = words[r.Word] & 31;
    for (uint32_t k = 0; k < 4; ++k) words[r.Word + k] = arm64::Relocation::AddressWord(reg, k, address);
}

void ReadRuntime(std::vector<uint32_t> &words, const arm64::Relocation &r, const Symbol *symbol, Nature nature) {
    uint32_t at = r.Begin;
    if (!symbol) {
        words[at++] = 0xd2800009; // mov x9, #0
        words[at++] = 0x9e670120; // fmov d0, x9
    } else if (symbol->Provides == Symbol::Runtime::SampleRate) {
        words[at++] = 0xfd4000c0; // ldr d0, [x6]
        if (nature == Nature::Int) words[at++] = 0x1e780009; // fcvtzs w9, d0
    } else {
        words[at++] = 0xb94008c9; // ldr w9, [x6, #8]
        if (nature == Nature::Real) words[at++] = 0x1e620120; // scvtf d0, w9
    }
    words[at] = 0x14000000 | (r.End - at);
}

} // namespace

std::expected<std::shared_ptr<const NativeCode>, std::string>
NativeCode::Publish(std::shared_ptr<const arm64::Program> program, const faustlens::Registry &registry) {
    if (!program) return std::unexpected("missing ARM64 program");
    try {
        auto words = program->Words;
        for (const auto &r : program->Relocations) {
            uintptr_t address = 0;
            switch (r.Kind) {
                case arm64::Binding::Math: address = MathAddress(Ext(r.Index)); break;
                case arm64::Binding::Sound:
                    address = r.Index == uint32_t(Op::SoundfileLength) ? reinterpret_cast<uintptr_t>(&SoundLength) :
                        r.Index == uint32_t(Op::SoundfileRate)         ? reinterpret_cast<uintptr_t>(&SoundRate) :
                                                                         reinterpret_cast<uintptr_t>(&SoundRead);
                    break;
                case arm64::Binding::Foreign: {
                    const auto &desc = program->Plan.Foreign.at(r.Index);
                    const Symbol *symbol = registry.Find(desc);
                    if (symbol && !CanCall(*symbol)) symbol = nullptr;
                    if (!symbol || symbol->Provides != Symbol::Runtime::None) {
                        ReadRuntime(words, r, symbol, desc.Result);
                        continue;
                    }
                    address = reinterpret_cast<uintptr_t>(desc.Kind == ForeignKind::Function ? symbol->Fn : symbol->Addr);
                    break;
                }
            }
            Bind(words, r, address);
        }
        auto executable = Executable::Publish(words);
        if (!executable) return std::unexpected(executable.error());
        auto code = std::shared_ptr<NativeCode>(new NativeCode(std::move(program), registry, std::move(*executable)));
        const auto *base = static_cast<const uint32_t *>(code->Executable_->Address());
        for (size_t b = 0; b < code->Entries.size(); ++b) code->Entries[b] = std::bit_cast<Entry>(base + code->Program_->Entries[b]);
        return code;
    } catch (const std::runtime_error &e) { return std::unexpected(e.what()); }
}

Native::Native(std::shared_ptr<const NativeCode> code)
    : Instance(code->Program_->Plan, code->Program_->Ui, code->Registry_, &code->Program_->Layout), Scratch(Plan.Regs), Code(std::move(code)) {}

std::unique_ptr<Native> Native::Create(std::shared_ptr<const NativeCode> code) {
    if (!code) throw std::invalid_argument("missing native code");
    return std::unique_ptr<Native>(new Native(std::move(code)));
}

std::expected<std::unique_ptr<Native>, std::string> Native::Compile(const faustlens::Plan &p, const UiNode &ui, const faustlens::Registry &reg) {
    return arm64::Program::Compile(p, ui).and_then([&](auto program) { return NativeCode::Publish(std::move(program), reg); }).transform([](auto code) {
        return Create(std::move(code));
    });
}

void Native::Execute(bool initialize, int32_t frames, const double *const *in, double *const *out) {
    struct Parameters {
        double SampleRate;
        int32_t Frames;
        Scalar *Scratch;
        const Instance *Owner;
    };
    static_assert(offsetof(Parameters, Frames) == 8 && offsetof(Parameters, Scratch) == 16 && offsetof(Parameters, Owner) == 24);
    const Parameters parameters{SampleRate, Frames, Scratch.data(), this};
    Code->Entries[initialize ? 0 : 1](Values.data(), State.data(), in, out, frames, &parameters);
}

} // namespace faustlens
