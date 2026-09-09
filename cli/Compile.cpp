#include "arm64/Program.h"
#include "query/Query.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace faustlens;

int main(int argc, char **argv) {
    constexpr auto usage = "Usage: faustlens-compile [-I directory] source.dsp -o output.f64";
    try {
        Session session;
        std::filesystem::path input, output;
        for (int k = 1; k < argc; ++k) {
            const std::string_view arg = argv[k];
            if (arg == "--help" || arg == "-h") {
                std::cout << usage << '\n';
                return 0;
            }
            if ((arg == "-o" || arg == "-I") && k + 1 < argc) {
                if (arg == "-o") output = argv[++k];
                else session.AddSearchPath(argv[++k]);
            } else if (arg.starts_with('-') || !input.empty()) throw std::runtime_error("Invalid argument: " + std::string(arg));
            else input = arg;
        }
        if (input.empty() || output.empty()) throw std::runtime_error(usage);
        input = std::filesystem::weakly_canonical(input);
        if (input == std::filesystem::weakly_canonical(output) || (std::filesystem::exists(output) && std::filesystem::equivalent(input, output)))
            throw std::runtime_error("Input and output paths must differ");
        Signals signals;
        Graph graph(session, input.string(), signals);
        for (const auto &d : session.Diagnostics()) std::cerr << CodeName(d.Code) << ": " << d.Payload << '\n';
        for (const auto &d : graph.Prop.Diags) std::cerr << CodeName(d.Code) << ": " << d.Payload << '\n';
        if (!graph.Ok) return 1;
        auto program = graph.Lower().and_then([&](Plan plan) { return arm64::Program::Compile(std::move(plan), graph.Ui(RootLabel(session.Metadata))); });
        if (!program) throw std::runtime_error(program.error());
        const auto bytes = (*program)->Encode();
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
        stream.close();
        if (!stream) throw std::runtime_error("Cannot write " + output.string());
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
