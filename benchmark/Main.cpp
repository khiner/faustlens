#include "Bench.h"
#include "Cases.h"
#include "Instance.h"
#include "Live.h"
#include "runtime/Interp.h"
#include "runtime/Native.h"

#include <algorithm>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sys/resource.h>
#include <thread>

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
    const std::string source = *file;
    Session session;
    session.SetBuffer(path, source);
    app::Live live;
    live.Execution = Backend::Interp;
    const auto cold = live.Reload(session, path);
    if (!cold.Compiled) {
        std::cerr << cold.Why << '\n';
        return 1;
    }
    const auto artifact = live.Current;
    std::unique_ptr<Native> native;
    std::vector<double> compile;
    size_t code_bytes = 0;
    if (backend == "native") {
        for (int i = 0; i < 11; ++i) {
            const auto start = Clock::now();
            auto built = Native::Compile(artifact->Plan, artifact->Ui);
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
    Instance *instance = native ? static_cast<Instance *>(native.get()) : artifact->Dsp.get();
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
    audio::EnableFlushToZero();
    const Rendering rendering = Render(use_reference ? ref : candidate, block);
    std::vector<double> edits, edit_to_output;
    auto &host = live.Host;
    host.Chunk = block;
    host.DeviceIn = instance->Inputs();
    host.DeviceOut = instance->Outputs();
    host.SampleRate = 48000;
    host.Current = host.MakeVoice(*live.Current->Dsp).release();
    host.Running = true;
    std::vector<float> device_in(size_t(block) * host.DeviceIn, 0.125f), device_out(size_t(block) * host.DeviceOut);
    std::atomic<const Instance *> observed = nullptr;
    std::jthread callback([&](std::stop_token stop) {
        auto next = Clock::now();
        const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(double(block) / 48000));
        while (!stop.stop_requested()) {
            host.Process(device_in.data(), device_out.data(), block);
            observed.store(host.Current->Dsp, std::memory_order_release);
            next += period;
            std::this_thread::sleep_until(next);
        }
    });
    for (int i = 0; i < 9; ++i) {
        std::string changed = source;
        const size_t gain = changed.find("0.5,");
        if (gain == std::string::npos) return 3;
        changed.replace(gain, 4, i % 2 ? "0.51," : "0.52,");
        const auto start = Clock::now();
        session.SetBuffer(path, changed);
        live.Collect();
        auto prepared = app::Live::Build(session, path, live.Current, {}, 48000, live.Sound, native ? Backend::Native : Backend::Interp);
        if (!prepared.Next || prepared.Status.Unchanged) return 3;
        const auto edited = live.Accept(prepared);
        if (!edited.Swapped) return 3;
        edits.push_back(Ms(start));
        while (observed.load(std::memory_order_acquire) != live.Current->Dsp.get()) {
            if (Ms(start) > 1000) return 4;
            std::this_thread::yield();
        }
        edit_to_output.push_back(Ms(start));
    }
    callback.request_stop();
    callback.join();
    host.Stop();
    live.Collect();
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    size_t instructions = 0;
    for (const auto &band : artifact->Plan.Bands) instructions += band.size();
    std::cout << std::setprecision(17) << "{\"case\":" << std::quoted(name) << ",\"backend\":" << std::quoted(backend) << ",\"block\":" << block
              << ",\"instructions\":" << instructions << ",\"registers\":" << artifact->Plan.Regs << ",\"persistent_values\":" << instance->Values.size()
              << ",\"state_bytes\":" << instance->State.size() * sizeof(Scalar) << ",\"code_bytes\":" << code_bytes
              << ",\"cold_prepare_ms\":" << cold.Timings.Total << ",\"edit_prepare_p50_ms\":" << Quantile(edits, 0.5)
              << ",\"edit_prepare_p95_ms\":" << Quantile(edits, 0.95) << ",\"edit_to_output_p50_ms\":" << Quantile(edit_to_output, 0.5)
              << ",\"edit_to_output_p95_ms\":" << Quantile(edit_to_output, 0.95) << ",\"native_compile_first_ms\":" << (compile.empty() ? 0 : compile.front())
              << ",\"native_compile_p50_ms\":" << (compile.empty() ? 0 : Quantile(compile, 0.5))
              << ",\"native_compile_p95_ms\":" << (compile.empty() ? 0 : Quantile(compile, 0.95))
              << ",\"render_p50_ns_per_frame\":" << Quantile(rendering.NsPerFrame, 0.5)
              << ",\"render_p95_ns_per_frame\":" << Quantile(rendering.NsPerFrame, 0.95) << ",\"max_error\":" << accuracy.Max
              << ",\"rms_error\":" << accuracy.Rms() << ",\"reference_peak\":" << accuracy.Peak << ",\"nonfinite\":" << accuracy.Nonfinite
              << ",\"prepare_peak_rss_bytes\":" << compilation_usage.ru_maxrss << ",\"process_peak_rss_bytes\":" << usage.ru_maxrss
              << ",\"checksum\":" << rendering.Checksum << "}\n";
}
