#include "signal/Differentiate.h"
#include "Bench.h"
#include "Instance.h"
#include "query/Query.h"
#include "runtime/Native.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sys/resource.h>

using namespace faustlens;

namespace {
template<class Range, class Write> void Array(const Range &values, Write write) {
    std::cout << '[';
    bool first = true;
    for (const auto &value : values) {
        if (!first) std::cout << ',';
        first = false;
        write(value);
    }
    std::cout << ']';
}

std::shared_ptr<const NativeCode> Compile(Graph &graph) {
    auto plan = graph.Lower();
    if (!plan) throw std::runtime_error(plan.error());
    auto program = arm64::Program::Compile(std::move(*plan), graph.Ui("benchmark"));
    if (!program) throw std::runtime_error(program.error());
    auto code = NativeCode::Publish(*program);
    if (!code) throw std::runtime_error(code.error());
    return *code;
}

size_t CheckPrimal(const std::shared_ptr<const NativeCode> &plain, const std::shared_ptr<const NativeCode> &augmented) {
    auto a = Native::Create(plain), b = Native::Create(augmented);
    Buffers av(a->Inputs(), a->Outputs(), 64, 0.25), bv(b->Inputs(), b->Outputs(), 64, 0.25);
    a->Init(48000);
    b->Init(48000);
    size_t invalidTangents = 0;
    for (int block = 0; block < 750; ++block) {
        a->Compute(64, av.Ip.data(), av.Op.data());
        b->Compute(64, bv.Ip.data(), bv.Op.data());
        for (int c = 0; c < a->Outputs(); ++c)
            for (int n = 0; n < 64; ++n)
                if (BitsOf(av.Out[c][n]) != BitsOf(bv.Out[c][n])) throw std::runtime_error("augmented primal differs from plain output");
        for (size_t c = 0; c < bv.Out.size(); ++c)
            for (double sample : bv.Out[c])
                if (!std::isfinite(sample)) {
                    if (c < size_t(a->Outputs())) throw std::runtime_error("nonfinite primal at default controls");
                    ++invalidTangents;
                }
    }
    return invalidTangents;
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: faustlens_differentiate source.dsp plain|columns|jvp COUNT|all");
        PrepareThread();
        const auto path = std::filesystem::canonical(argv[1]);
        const std::string mode = argv[2];
        if (mode != "plain" && mode != "columns" && mode != "jvp") throw std::runtime_error("invalid mode");
        std::ifstream file(path);
        const std::string source{std::istreambuf_iterator<char>(file), {}};
        const auto start = Clock::now();
        Session session;
        session.AddSearchPath(path.parent_path());
        session.SetBuffer(path.string(), source);
        Signals signals;
        Graph graph(session, path.string(), signals);
        if (!graph.Ok) throw std::runtime_error("source compilation failed");
        const double frontend = Ms(start);
        const auto originalRoots = graph.Outs;
        DifferentiateRequest request;
        std::set<std::string> seen;
        for (const auto &w : graph.Prop.Ui) {
            if (w.Kind != UiKind::HSlider && w.Kind != UiKind::VSlider && w.Kind != UiKind::NumEntry) continue;
            const std::string label(signals.Str(w.Label));
            if (seen.insert(label).second) request.Controls.push_back(label);
        }
        const size_t available = request.Controls.size();
        const size_t count = std::string_view(argv[3]) == "all" ? available : std::stoul(argv[3]);
        if (count > available) throw std::runtime_error("requested more controls than declared");
        request.Controls.resize(mode == "plain" ? 0 : count);
        if (mode == "jvp") {
            request.ExplicitDirections = true;
            request.DirectionCount = 1;
            for (size_t j = 0; j < count; ++j) request.Directions.push_back((j & 1 ? -1.0 : 1.0) / double(j + 1));
        }
        const auto adStart = Clock::now();
        const auto derivative = Differentiate(signals, graph.Outs, request, graph.Prop.Ui);
        const double differentiate = Ms(adStart);
        std::cout << std::setprecision(17) << std::boolalpha << "{\"source\":" << std::quoted(path.string()) << ",\"mode\":" << std::quoted(mode)
                  << ",\"controls\":" << request.Controls.size() << ",\"available_controls\":" << available << ",\"directions\":" << derivative.DirectionCount
                  << ",\"diagnostics\":";
        Array(derivative.Diagnostics, [](const auto &d) {
            std::cout << "{\"kind\":" << int(d.Kind) << ",\"control\":" << d.Control << ",\"signal\":" << d.Signal << ",\"reason\":" << std::quoted(d.Reason)
                      << '}';
        });
        std::cout << ",\"control_info\":";
        Array(derivative.Controls, [](const auto &c) {
            std::cout << "{\"label\":" << std::quoted(c.Label) << ",\"present\":" << c.Present << ",\"structural\":" << c.Structural
                      << ",\"active\":" << c.Active << '}';
        });
        if (!derivative.Ok()) {
            std::cout << ",\"status\":\"unsupported\",\"error\":" << std::quoted(derivative.Error) << "}\n";
            return 0;
        }
        graph.Outs.insert(graph.Outs.end(), derivative.Tangents.begin(), derivative.Tangents.end());
        const auto lowerStart = Clock::now();
        auto code = Compile(graph);
        const double lowerCompile = Ms(lowerStart), totalCompile = Ms(start);
        size_t reachable = 0;
        Reachable(signals, derivative.Tangents, [&](SigId) {
            ++reachable;
            return true;
        });
        const auto createStart = Clock::now();
        auto dsp = Native::Create(code);
        const double creation = Ms(createStart);
        const auto initStart = Clock::now();
        dsp->Init(48000);
        const double initialization = Ms(initStart);
        if (!dsp->Diagnostics.empty()) throw std::runtime_error(dsp->Diagnostics.front());
        graph.Outs = originalRoots;
        auto plain = Compile(graph);
        const size_t invalidTangents = CheckPrimal(plain, code);
        if (invalidTangents) {
            std::cout << ",\"status\":\"invalid_derivative\",\"primal_bitwise_equal\":true,\"nonfinite_tangent_samples\":" << invalidTangents
                      << ",\"error\":\"nonfinite tangent at default controls under strict derivative-domain semantics\"}\n";
            return 0;
        }
        Reference ref = ReferenceOf(dsp.get());
        // Use source defaults for both validation and timing.
        ref.Control = [](void *, double) {};
        std::cout << ",\"status\":\"ok\",\"primal_bitwise_equal\":true,\"frontend_ms\":" << frontend << ",\"differentiate_ms\":" << differentiate
                  << ",\"lower_emit_publish_ms\":" << lowerCompile << ",\"compile_ms\":" << totalCompile << ",\"create_ms\":" << creation
                  << ",\"init_ms\":" << initialization << ",\"allocated_tangent_nodes\":" << derivative.AllocatedNodes
                  << ",\"tangent_reachable_nodes\":" << reachable << ",\"arena_nodes\":" << signals.Size()
                  << ",\"arena_node_storage_bytes\":" << signals.Nodes.capacity() * sizeof(SigNode)
                  << ",\"arena_child_storage_bytes\":" << signals.ChildPool.capacity() * sizeof(SigId) << ",\"code_bytes\":" << code->CodeBytes()
                  << ",\"state_bytes\":" << dsp->State.size() * sizeof(Scalar) << ",\"persistent_bytes\":" << dsp->Values.size() * sizeof(Scalar)
                  << ",\"scratch_bytes\":" << code->Program().Plan.Regs * sizeof(Scalar) << ",\"instructions_by_band\":";
        Array(code->Program().Plan.Bands, [](const auto &band) { std::cout << band.size(); });
        std::cout << ",\"render\":";
        Array(std::array{64, 256}, [&](int block) {
            auto rendering = Render(ref, block, 0.25);
            if (!std::isfinite(rendering.Checksum)) throw std::runtime_error("nonfinite timed rendering");
            std::cout << "{\"block\":" << block << ",\"p50_ns_per_frame\":" << Quantile(rendering.NsPerFrame, 0.5) << ",\"ns_per_frame\":";
            Array(rendering.NsPerFrame, [](double ns) { std::cout << ns; });
            std::cout << '}';
        });
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        std::cout << ",\"process_peak_rss_bytes\":" << usage.ru_maxrss << "}\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
