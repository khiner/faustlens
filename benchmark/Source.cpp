#include "Source.h"

using namespace corpus;

namespace {

void Trace(Reference dsp, int block, std::ofstream &file) {
    Buffers data(dsp.Inputs, dsp.Outputs, block, 0.25);
    dsp.Init(dsp.Object, SampleRate);
    dsp.Control(dsp.Object, 1);
    for (int done = 0; done < SampleRate; done += block) {
        const int frames = std::min(block, SampleRate - done);
        dsp.Compute(dsp.Object, frames, data.Ip.data(), data.Op.data());
        for (const auto &channel : data.Out) file.write(reinterpret_cast<const char *>(channel.data()), frames * sizeof(double));
    }
    if (!file) throw std::runtime_error("cannot write output trace");
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: SOURCE_BENCH source.dsp native|llvm-scalar|llvm-vector TRACE_FILE");
        PrepareThread();
        const auto path = std::filesystem::canonical(argv[1]);
        const std::string backend = argv[2], name = path.stem().string();
        std::ifstream file(path);
        if (!file) throw std::runtime_error("cannot read source");
        const std::string source{std::istreambuf_iterator<char>(file), {}};
        auto compiled = corpus::Compile(path, source, backend);
        auto stage = Clock::now();
        ReferenceOwner dsp(corpus::Create(compiled));
        Reference candidate = dsp.Dsp;
        const double creation = Ms(stage);
        stage = Clock::now();
        candidate.Init(candidate.Object, SampleRate);
        const double initialization = Ms(stage);
        std::ofstream trace(argv[3], std::ios::binary);
        if (!trace) throw std::runtime_error("cannot open output trace");
        for (int block : Blocks) Trace(candidate, block, trace);
        trace.close();
        uint64_t fpcr;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));
        std::vector<Rendering> rendering;
        for (int block : Blocks) rendering.push_back(Render(candidate, block, 0.25));
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        std::cout << std::setprecision(17) << "{\"case\":" << std::quoted(name) << ",\"backend\":" << std::quoted(backend)
                  << ",\"compile_ms\":" << compiled.Elapsed << ",\"compile_peak_rss_bytes\":" << compiled.PeakRss
                  << ",\"process_peak_rss_bytes\":" << usage.ru_maxrss << ",\"create_ms\":" << creation << ",\"init_ms\":" << initialization
                  << ",\"inputs\":" << candidate.Inputs << ",\"outputs\":" << candidate.Outputs;
        std::cout << ",\"render\":[";
        for (size_t b = 0; b < rendering.size(); ++b) {
            if (b) std::cout << ',';
            std::cout << "{\"block\":" << Blocks[b] << ",\"p50_ns_per_frame\":" << Quantile(rendering[b].NsPerFrame, 0.5)
                      << ",\"p95_ns_per_frame\":" << Quantile(rendering[b].NsPerFrame, 0.95) << ",\"ns_per_frame\":[";
            for (size_t i = 0; i < rendering[b].NsPerFrame.size(); ++i) {
                if (i) std::cout << ',';
                std::cout << rendering[b].NsPerFrame[i];
            }
            std::cout << "]}";
        }
        std::cout << ']';
#ifdef BENCH_LLVM
        std::cout << ",\"faust_version\":" << std::quoted(getCLibFaustVersion()) << ",\"target\":" << std::quoted(getDSPMachineTarget())
                  << ",\"options\":" << std::quoted(compiled.Code->getCompileOptions());
#else
        size_t instructions = 0;
        for (const auto &band : compiled.Code->Program().Plan.Bands) instructions += band.size();
        std::cout << ",\"frontend_ms\":" << compiled.Frontend << ",\"emit_ms\":" << compiled.Emission << ",\"publish_ms\":" << compiled.Publication
                  << ",\"instructions\":" << instructions << ",\"code_bytes\":" << compiled.Code->CodeBytes();
#endif
        std::cout << "}\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
