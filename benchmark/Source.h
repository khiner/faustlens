#pragma once

#include "Bench.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <sys/resource.h>

#ifdef BENCH_LLVM
#define FAUSTFLOAT double
#include "faust/dsp/llvm-dsp.h"
#include "faust/gui/MapUI.h"
#include "faust/gui/SimpleParser.h"
#include "faust/gui/Soundfile.h"
#else
#include "Instance.h"
#include "query/Query.h"
#include "runtime/Native.h"
#endif

namespace corpus {

constexpr int SampleRate = 48000, SoundFrames = 4096;
constexpr int Blocks[] = {64, 256};
inline double SoundSample(int part, int frame) { return std::sin(part + 2 * std::acos(-1.0) * frame / SoundFrames); }

#ifdef BENCH_LLVM
struct Reader : SoundfileReader {
    bool checkFile(const std::string &) override { return true; }
    void getParamsFile(const std::string &, int &channels, int &length) override {
        channels = 2;
        length = SoundFrames;
    }
    void readFile(Soundfile *sound, const std::string &, int part, int &offset, int) override {
        sound->fLength[part] = SoundFrames;
        sound->fSR[part] = SampleRate;
        sound->fOffset[part] = offset;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < SoundFrames; ++i) static_cast<double **>(sound->fBuffers)[c][offset + i] = SoundSample(part, i);
        offset += SoundFrames;
    }
};

struct Controls : MapUI {
    std::vector<double *> Buttons;
    std::vector<std::unique_ptr<Soundfile>> Sounds;
    void addButton(const char *, double *zone) override { Buttons.push_back(zone); }
    void addSoundfile(const char *, const char *url, Soundfile **zone) override {
        std::vector<std::string> names;
        if (!parseMenuList2(url, names, true)) throw std::runtime_error("invalid soundfile URL list");
        Sounds.emplace_back(Reader().createSoundfile(names, MAX_CHAN, true));
        if (!Sounds.back()) throw std::runtime_error("soundfile fixture allocation failed");
        *zone = Sounds.back().get();
    }
};

struct Dsp {
    std::unique_ptr<llvm_dsp> Instance;
    Controls Ui;
    explicit Dsp(llvm_dsp_factory *factory) : Instance(factory->createDSPInstance()) {
        if (!Instance) throw std::runtime_error("LLVM instance creation failed");
        Instance->buildUserInterface(&Ui);
    }
    Reference Ref() {
        return {
            this,
            [](void *p) { delete static_cast<Dsp *>(p); },
            [](void *p, int rate) { static_cast<Dsp *>(p)->Instance->init(rate); },
            [](void *p, int n, double **in, double **out) { static_cast<Dsp *>(p)->Instance->compute(n, in, out); },
            [](void *p, double) {
                for (auto *zone : static_cast<Dsp *>(p)->Ui.Buttons) *zone = 1;
            },
            Instance->getNumInputs(),
            Instance->getNumOutputs()
        };
    }
};
#else
inline bool ReadSound(void *, const std::string &, uint32_t part, std::vector<std::vector<double>> &channels, int32_t &rate) {
    channels.assign(2, std::vector<double>(SoundFrames));
    rate = SampleRate;
    for (auto &channel : channels)
        for (int i{0}; i < SoundFrames; ++i) channel[i] = SoundSample(part, i);
    return true;
}
#endif

struct Compilation {
#ifdef BENCH_LLVM
    std::unique_ptr<llvm_dsp_factory, decltype(&deleteDSPFactory)> Code{nullptr, deleteDSPFactory};
#else
    std::shared_ptr<const faustlens::NativeCode> Code;
    double Frontend = 0, Emission = 0, Publication = 0;
#endif
    double Elapsed = 0;
    long PeakRss = 0;
};

inline Compilation Compile(const std::filesystem::path &path, const std::string &source, const std::string &backend) {
    const std::string directory{path.parent_path().string()};
    Compilation result;
#ifdef BENCH_LLVM
    const std::string name{path.stem().string()};
    if (backend != "llvm-scalar" && backend != "llvm-vector") throw std::runtime_error("expected LLVM backend");
    if (setenv("FAUST_OPT", "FAUST_LLVM_NO_FM", 1)) throw std::runtime_error("cannot disable Faust LLVM fast math");
    if (!getAllDSPFactories().empty()) throw std::runtime_error("expected an empty factory cache");
    const char *args[] = {"-double", backend == "llvm-vector" ? "-vec" : "-scal", "-vs", "32", "-I", directory.c_str(), "-I", BENCH_LIBRARY_DIR};
    std::string error;
    const auto start = Clock::now();
    std::unique_ptr<llvm_dsp_factory, decltype(&deleteDSPFactory)> factory(
        createDSPFactoryFromString(name, source, 8, args, "", error, -1), deleteDSPFactory
    );
    result.Elapsed = Ms(start);
    if (!factory) throw std::runtime_error(error.empty() ? "LLVM compilation failed" : error);
    result.Code = std::move(factory);
#else
    using namespace faustlens;
    if (backend != "native") throw std::runtime_error("expected native backend");
    const auto start = Clock::now();
    Session session;
    session.AddSearchPath(directory);
    session.SetBuffer(path.string(), source);
    Signals signals;
    Graph graph(session, path.string(), signals);
    if (!graph.Ok) {
        for (const auto &d : session.Diagnostics()) std::cerr << d.Payload << '\n';
        for (const auto &d : graph.Prop.Diags) std::cerr << d.Payload << '\n';
        throw std::runtime_error("source compilation failed");
    }
    auto plan = graph.Lower();
    if (!plan) throw std::runtime_error(plan.error());
    auto ui = graph.Ui(RootLabel(session.Metadata));
    result.Frontend = Ms(start);
    const auto emitStart = Clock::now();
    auto program = arm64::Program::Compile(std::move(*plan), std::move(ui));
    result.Emission = Ms(emitStart);
    if (!program) throw std::runtime_error(program.error());
    const auto publishStart = Clock::now();
    auto code = NativeCode::Publish(*program);
    result.Publication = Ms(publishStart);
    result.Elapsed = Ms(start);
    if (!code) throw std::runtime_error(code.error());
    result.Code = std::move(*code);
#endif
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    result.PeakRss = usage.ru_maxrss;
    return result;
}

inline Reference Create(const Compilation &compiled) {
#ifdef BENCH_LLVM
    auto *dsp = new Dsp(compiled.Code.get());
    Reference candidate = dsp->Ref();
#else
    using namespace faustlens;
    auto dsp = Native::Create(compiled.Code);
    if (!dsp) throw std::runtime_error("native instance creation failed");
    faustlens::SoundfileReader reader{nullptr, ReadSound};
    dsp->LoadSoundfiles(&reader);
    Reference candidate = ReferenceOf(dsp.release());
    candidate.Control = [](void *p, double) {
        auto &instance = *static_cast<Instance *>(p);
        for (const auto &zone : instance.Zones)
            if (zone.Kind == UiKind::Button) instance.SetControl(zone.Label, 1);
    };
#endif
    return candidate;
}

} // namespace corpus
