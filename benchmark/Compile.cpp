#include "runtime/Interp.h"
#include "runtime/Native.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>

using namespace faustlens;

int main(int argc, char **argv) {
    if (argc != 3) return 1;
    const uint32_t count = uint32_t(std::stoul(argv[1]));
    const std::string shape = argv[2];
    if (count < 4 || count % 2 || (shape != "chain" && shape != "wide" && shape != "math")) return 1;
    Plan plan;
    plan.Inputs = plan.Outputs = 1;
    const auto emit = [&](Band band, Op op, uint8_t form, std::initializer_list<Reg> args, bool result = true) {
        const Reg dst = result ? plan.Regs++ : NoReg;
        plan.Band(band).push_back({uint8_t(op), form, Nature::Real, dst, 0, 0, uint32_t(plan.Operands.size()), uint32_t(args.size())});
        plan.Operands.insert(plan.Operands.end(), args);
        return dst;
    };
    const Reg constant = emit(Band::Init, Op::ConstReal, 0, {});
    const auto bits = std::bit_cast<uint64_t>(0.999999999);
    plan.Band(Band::Init).back().Imm = uint32_t(bits);
    plan.Band(Band::Init).back().Aux = uint32_t(bits >> 32);
    const Reg input = emit(Band::Sample, Op::Input, 0, {});
    Reg value = input;
    if (shape == "wide") {
        std::vector<Reg> values;
        for (uint32_t k = 0; k < (count - 2) / 2; ++k) values.push_back(emit(Band::Sample, Op::BinOp, uint8_t(BinOpCode::Mul), {input, constant}));
        value = values[0];
        for (size_t k = 1; k < values.size(); ++k) value = emit(Band::Sample, Op::BinOp, uint8_t(BinOpCode::Add), {value, values[k]});
    } else {
        for (uint32_t k = 0; k < count - (shape == "math" ? 4 : 3); ++k)
            value = shape == "math" ? emit(Band::Sample, Op::Extended, uint8_t(Ext::Sin), {value}) :
                                      emit(Band::Sample, Op::BinOp, uint8_t(BinOpCode::Mul), {value, constant});
    }
    if (shape == "math") value = emit(Band::Sample, Op::BinOp, uint8_t(BinOpCode::Mul), {value, constant});
    emit(Band::Sample, Op::Output, 0, {value}, false);
    const UiNode ui;
    std::unique_ptr<Native> native;
    std::vector<double> elapsed, emission, publication, instantiation;
    using Clock = std::chrono::steady_clock;
    const auto ms = [](auto begin, auto end) { return std::chrono::duration<double, std::milli>(end - begin).count(); };
    for (int k = 0; k < 11; ++k) {
        const auto start = Clock::now();
        auto program = arm64::Program::Compile(plan, ui);
        const auto emitted = Clock::now();
        if (!program) {
            std::cerr << program.error() << '\n';
            return 2;
        }
        auto code = NativeCode::Publish(*program);
        const auto published = Clock::now();
        if (!code) {
            std::cerr << code.error() << '\n';
            return 2;
        }
        auto instance = Native::Create(*code);
        const auto instantiated = Clock::now();
        emission.push_back(ms(start, emitted));
        publication.push_back(ms(emitted, published));
        instantiation.push_back(ms(published, instantiated));
        elapsed.push_back(ms(start, instantiated));
        native = std::move(instance);
    }
    Interp interp(plan, ui);
    interp.Init(48000);
    native->Init(48000);
    double inputData[17], a[17], b[17];
    for (int k = 0; k < 17; ++k) inputData[k] = std::sin(double(k));
    const double *in[] = {inputData};
    double *outA[] = {a}, *outB[] = {b};
    interp.Compute(17, in, outA);
    native->Compute(17, in, outB);
    for (int k = 0; k < 17; ++k)
        if (std::bit_cast<uint64_t>(a[k]) != std::bit_cast<uint64_t>(b[k])) return 3;
    const double first = elapsed[0];
    for (auto *times : {&elapsed, &emission, &publication, &instantiation}) std::ranges::sort(*times);
    std::cout << std::setprecision(17) << "{\"shape\":" << std::quoted(shape) << ",\"instructions\":" << count << ",\"code_bytes\":" << native->CodeBytes()
              << ",\"first_ms\":" << first << ",\"p50_ms\":" << elapsed[5] << ",\"p95_ms\":" << elapsed[10] << ",\"emit_p95_ms\":" << emission[10]
              << ",\"publish_p95_ms\":" << publication[10] << ",\"instantiate_p95_ms\":" << instantiation[10] << ",\"exact\":true}\n";
}
