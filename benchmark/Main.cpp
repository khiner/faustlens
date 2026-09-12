#include "Bench.h"
#include "Cases.h"
#include "Instance.h"
#include "query/Query.h"
#include "runtime/Float.h"
#include "runtime/Interp.h"
#include "runtime/Native.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sys/resource.h>

using namespace faustlens;

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: faustlens_benchmark CASE interp|native|scalar|vector [BLOCK]\n";
        return 1;
    }
    PrepareThread();
    const std::string name = argv[1], backend = argv[2];
    const int block = argc > 3 ? std::stoi(argv[3]) : 64;
    const auto chosen = std::ranges::find_if(Cases, [&](const Case &c) { return c.Name == name; });
    if (chosen == std::end(Cases) || block < 1 || block > 4096 || (backend != "interp" && backend != "native" && backend != "scalar" && backend != "vector"))
        return 1;
    const std::string path = std::string(BENCH_SOURCE_DIR) + "/" + name + ".dsp";
    const auto file = ReadFile(path);
    if (!file) {
        std::cerr << file.error();
        return 1;
    }
    Session session;
    session.SetBuffer(path, *file);
    Signals signals;
    const auto begun{Clock::now()};
    Graph graph(session, path, signals);
    auto plan{graph.Lower()};
    if (!plan) {
        std::cerr << plan.error();
        return 1;
    }
    auto ui{graph.Ui(RootLabel(session.Metadata))};
    Interp interpreted{*plan, ui};
    interpreted.Init(48000);
    const double prepareMs{Ms(begun)};
    std::unique_ptr<Native> native;
    std::vector<double> compile;
    size_t code_bytes = 0;
    if (backend == "native") {
        for (int i = 0; i < 11; ++i) {
            const auto start = Clock::now();
            auto built = Native::Compile(*plan, ui);
            const double elapsed = Ms(start);
            if (!built) {
                std::cerr << built.error() << '\n';
                return 2;
            }
            compile.push_back(elapsed);
            native = std::move(*built);
        }
        code_bytes = native->CodeBytes();
    }
    Instance *instance{native ? static_cast<Instance *>(native.get()) : &interpreted};
    rusage compilation_usage{};
    getrusage(RUSAGE_SELF, &compilation_usage);
    ReferenceOwner reference{backend == "vector" ? chosen->Vector() : chosen->Scalar()};
    Reference &ref = reference.Dsp;
    if (ref.Inputs != instance->Inputs() || ref.Outputs != instance->Outputs()) return 2;
    const bool use_reference = backend == "scalar" || backend == "vector";
    const Reference candidate = ReferenceOf(instance);
    const Accuracy accuracy = Compare(candidate, ref, block);
    if (!accuracy.Ok()) {
        std::cerr << "output differs from reference: max error " << accuracy.Max << ", nonfinite samples " << accuracy.Nonfinite << '\n';
        return 2;
    }
    EnableFlushToZero();
    const Rendering rendering = Render(use_reference ? ref : candidate, block);
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    size_t instructions = 0;
    for (const auto &band : plan->Bands) instructions += band.size();
    std::cout << std::setprecision(17) << "{\"case\":" << std::quoted(name) << ",\"backend\":" << std::quoted(backend) << ",\"block\":" << block
              << ",\"instructions\":" << instructions << ",\"registers\":" << plan->Regs << ",\"persistent_values\":" << instance->Values.size()
              << ",\"state_bytes\":" << instance->State.size() * sizeof(Scalar) << ",\"code_bytes\":" << code_bytes << ",\"prepare_ms\":" << prepareMs
              << ",\"native_compile_first_ms\":" << (compile.empty() ? 0 : compile.front())
              << ",\"native_compile_p50_ms\":" << (compile.empty() ? 0 : Quantile(compile, 0.5))
              << ",\"native_compile_p95_ms\":" << (compile.empty() ? 0 : Quantile(compile, 0.95))
              << ",\"render_p50_ns_per_frame\":" << Quantile(rendering.NsPerFrame, 0.5)
              << ",\"render_p95_ns_per_frame\":" << Quantile(rendering.NsPerFrame, 0.95) << ",\"max_error\":" << accuracy.Max
              << ",\"rms_error\":" << accuracy.Rms() << ",\"reference_peak\":" << accuracy.Peak << ",\"nonfinite\":" << accuracy.Nonfinite
              << ",\"prepare_peak_rss_bytes\":" << compilation_usage.ru_maxrss << ",\"process_peak_rss_bytes\":" << usage.ru_maxrss
              << ",\"checksum\":" << rendering.Checksum << "}\n";
}
