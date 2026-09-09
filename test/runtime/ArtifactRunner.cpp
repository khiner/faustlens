#include "runtime/Native.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace faustlens;

namespace {
double Probe(double value) { return value * 3 + 1; }
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 1;
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    Registry registry = Registry::Builtin();
    registry.AddFunction("probe", Nature::Real, {Nature::Real}, reinterpret_cast<void *>(&Probe));
    auto code = arm64::Program::Decode(bytes).and_then([&](auto program) { return NativeCode::Publish(std::move(program), registry); });
    if (!code) {
        std::cerr << code.error() << '\n';
        return 1;
    }
    auto dsp = Native::Create(*code);
    if (dsp->Inputs() != 3 || dsp->Outputs() != 8 || !dsp->Diagnostics.empty()) return 2;
    dsp->Init(48000);
    const double samples[] = {1, 2, 3};
    const double *in[] = {samples, samples, samples};
    double outputs[8][3];
    double *out[8];
    for (int k = 0; k < 8; ++k) out[k] = outputs[k];
    dsp->Compute(3, in, out);
    for (int k = 0; k < 3; ++k) {
        if (out[0][k] != k * 0.5 || out[1][k] != std::sin(samples[k]) || out[2][k] != Probe(samples[k])) return 3;
        if (out[3][k] != 48000 || out[4][k] != 3 || out[5][k] != 1024 || out[6][k] != 44100 || out[7][k] != 0) return 4;
    }
    std::cout << "PASS: artifact compiled and executed in separate processes\n";
}
