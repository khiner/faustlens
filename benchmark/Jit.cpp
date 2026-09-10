#include "Bench.h"
#include "Cases.h"

#include <cfenv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/resource.h>

#ifdef BENCH_LLVM
#define FAUSTFLOAT double
#include "faust/dsp/llvm-dsp.h"
#include "faust/gui/MapUI.h"
#else
#include "Instance.h"
#include "query/Query.h"
#include "runtime/Native.h"
#endif

namespace {

#ifdef BENCH_LLVM
using Factory = std::unique_ptr<llvm_dsp_factory, decltype(&deleteDSPFactory)>;

Factory Compile(const std::string &name, const std::string &source, bool vector) {
    const char *args[] = {"-double", vector ? "-vec" : "-scal", "-vs", "32", "-I", BENCH_LIBRARY_DIR, "-I", BENCH_SOURCE_DIR};
    std::string error;
    Factory factory(createDSPFactoryFromString(name, source, 8, args, "", error, -1), deleteDSPFactory);
    if (!factory) throw std::runtime_error(error.empty() ? "Faust LLVM compilation failed" : error);
    return factory;
}

void CheckEmptyCache() {
    if (!getAllDSPFactories().empty()) throw std::runtime_error("uncached compilation retained a Faust factory");
}

struct Instance {
    std::unique_ptr<llvm_dsp> Dsp;
    MapUI Ui;
    explicit Instance(const Factory &factory) : Dsp(factory->createDSPInstance()) {
        if (!Dsp) throw std::runtime_error("Faust LLVM instance creation failed");
        Dsp->buildUserInterface(&Ui);
    }
};

Reference Create(const Factory &factory) {
    auto *p = new Instance(factory);
    return {
        p,
        [](void *p) { delete static_cast<Instance *>(p); },
        [](void *p, int rate) { static_cast<Instance *>(p)->Dsp->init(rate); },
        [](void *p, int n, double **in, double **out) { static_cast<Instance *>(p)->Dsp->compute(n, in, out); },
        [](void *p, double value) { static_cast<Instance *>(p)->Ui.setParamValue("gain", value); },
        p->Dsp->getNumInputs(),
        p->Dsp->getNumOutputs()
    };
}
#else
using Factory = std::shared_ptr<const faustlens::NativeCode>;

Factory Compile(const std::string &name, const std::string &source, bool) {
    using namespace faustlens;
    const std::string path = std::string(BENCH_SOURCE_DIR) + "/" + name + ".dsp";
    Session session;
    session.SetBuffer(path, source);
    Signals signals;
    Graph graph(session, path, signals);
    if (!graph.Ok) throw std::runtime_error("FaustLens lowering failed");
    const auto emit = [&](Plan plan) { return arm64::Program::Compile(std::move(plan), graph.Ui(RootLabel(session.Metadata))); };
    auto code = graph.Lower().and_then(emit).and_then([](auto program) { return NativeCode::Publish(std::move(program)); });
    if (!code) throw std::runtime_error(code.error());
    return std::move(*code);
}

void CheckEmptyCache() {}

Reference Create(const Factory &factory) { return ReferenceOf(faustlens::Native::Create(factory).release()); }

#endif

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc < 4 || argc > 5) throw std::runtime_error("usage: JIT_BENCH CASE native|llvm-scalar|llvm-vector BLOCK [--cold]");
        PrepareThread();
        const std::string name = argv[1], backend = argv[2];
        const int block = std::stoi(argv[3]);
        const bool cold = argc == 5 && std::string_view(argv[4]) == "--cold";
        const auto chosen = std::ranges::find_if(Cases, [&](const Case &c) { return c.Name == name; });
        if (chosen == std::end(Cases) || block < 1 || block > 4096 || (argc == 5 && !cold)) throw std::runtime_error("invalid JIT benchmark arguments");
#ifdef BENCH_LLVM
        if (backend != "llvm-scalar" && backend != "llvm-vector") throw std::runtime_error("expected LLVM backend");
        if (setenv("FAUST_OPT", "FAUST_LLVM_NO_FM", 1)) throw std::runtime_error("cannot disable Faust LLVM fast math");
#else
        if (backend != "native") throw std::runtime_error("expected native backend");
#endif
        const bool vector = backend == "llvm-vector";
        std::ifstream file(std::string(BENCH_SOURCE_DIR) + "/" + name + ".dsp");
        if (!file) throw std::runtime_error("cannot read DSP source");
        const std::string source{std::istreambuf_iterator<char>(file), {}};
        std::optional<Factory> factory;
        std::vector<double> compilation, creation, initialization, cached;
        double first = 0;
        for (int i = 0; i < (cold ? 1 : 12); ++i) {
            factory.reset();
            CheckEmptyCache();
            const auto start = Clock::now();
            factory.emplace(Compile(name, source, vector));
            const double elapsed = Ms(start);
            if (i == 0) first = elapsed;
            else compilation.push_back(elapsed);
        }
#ifdef BENCH_LLVM
        if (!cold)
            for (int i = 0; i < 11; ++i) {
                const auto start = Clock::now();
                auto hit = Compile(name, source, vector);
                cached.push_back(Ms(start));
                if (hit.get() != factory->get()) throw std::runtime_error("Faust factory cache missed");
            }
#endif
        std::optional<ReferenceOwner> dsp;
        for (int i = 0; i < (cold ? 1 : 11); ++i) {
            dsp.reset();
            auto start = Clock::now();
            dsp.emplace(Create(*factory));
            creation.push_back(Ms(start));
            start = Clock::now();
            dsp->Dsp.Init(dsp->Dsp.Object, 48000);
            initialization.push_back(Ms(start));
        }
        Reference candidate = dsp->Dsp;
        if (cold) {
            Buffers data(candidate.Inputs, candidate.Outputs, block);
            const auto start = Clock::now();
            candidate.Compute(candidate.Object, block, data.Ip.data(), data.Op.data());
            const double compute = Ms(start);
            double checksum = 0;
            for (const auto &c : data.Out) checksum += std::accumulate(c.begin(), c.end(), 0.0);
            if (!std::isfinite(checksum)) throw std::runtime_error("nonfinite first block");
            std::cout << std::setprecision(17) << "{\"compile_ms\":" << first << ",\"create_ms\":" << creation[0] << ",\"init_ms\":" << initialization[0]
                      << ",\"first_block_ms\":" << compute << ",\"checksum\":" << checksum << "}\n"
                      << std::flush;
            return 0;
        }
        ReferenceOwner reference(chosen->Scalar());
        if (candidate.Inputs != reference.Dsp.Inputs || candidate.Outputs != reference.Dsp.Outputs)
            throw std::runtime_error("JIT channel counts differ from reference");
        const Accuracy accuracy = Compare(candidate, reference.Dsp, block);
        if (!accuracy.Ok()) throw std::runtime_error("JIT output differs from reference: " + std::to_string(accuracy.Max));
        uint64_t fpcr;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));
        const Rendering rendering = Render(candidate, block);
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        std::cout << std::setprecision(17) << "{\"case\":" << std::quoted(name) << ",\"backend\":" << std::quoted(backend) << ",\"block\":" << block
                  << ",\"compile_first_ms\":" << first << ",\"compile_p50_ms\":" << Quantile(compilation, 0.5)
                  << ",\"compile_p95_ms\":" << Quantile(compilation, 0.95) << ",\"create_p50_ms\":" << Quantile(creation, 0.5)
                  << ",\"create_p95_ms\":" << Quantile(creation, 0.95) << ",\"init_p50_ms\":" << Quantile(initialization, 0.5)
                  << ",\"init_p95_ms\":" << Quantile(initialization, 0.95)
                  << ",\"cache_hit_p50_ms\":" << (cached.empty() ? "null" : std::to_string(Quantile(cached, 0.5)))
                  << ",\"cache_hit_p95_ms\":" << (cached.empty() ? "null" : std::to_string(Quantile(cached, 0.95)))
                  << ",\"render_p50_ns_per_frame\":" << Quantile(rendering.NsPerFrame, 0.5)
                  << ",\"render_p95_ns_per_frame\":" << Quantile(rendering.NsPerFrame, 0.95) << ",\"max_error\":" << accuracy.Max
                  << ",\"rms_error\":" << accuracy.Rms() << ",\"reference_peak\":" << accuracy.Peak << ",\"nonfinite\":" << accuracy.Nonfinite
                  << ",\"process_peak_rss_bytes\":" << usage.ru_maxrss << ",\"checksum\":" << rendering.Checksum;
#ifdef BENCH_LLVM
        std::cout << ",\"faust_version\":" << std::quoted(getCLibFaustVersion()) << ",\"target\":" << std::quoted(getDSPMachineTarget())
                  << ",\"options\":" << std::quoted((*factory)->getCompileOptions());
#endif
        std::cout << "}\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
