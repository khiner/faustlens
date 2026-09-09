#include "conformance/Sweep.h"
#include "runtime/Interp.h"

#include "doctest.h"

#include <bit>
#include <cmath>

using namespace faustlens;
using namespace faustlens::test;

namespace {

struct Fixture {
    Program Program;
    faustlens::Plan Plan;
    UiNode Ui;
    Fixture(const char *source) : Program("/instance.dsp", source) {
        REQUIRE(Program.Ok);
        const auto plan = Program.Lower();
        REQUIRE(plan);
        Plan = *plan;
        Ui = Program.Ui("instance");
    }
};

template<class R> R ForeignZero() { return R(7); }
template<class R, class A> R ForeignOne(A a) { return R(7 + 3 * double(a)); }
template<class R, class A, class B> R ForeignTwo(A a, B b) { return R(7 + 3 * double(a) + 5 * double(b)); }

template<class R, class... A> void Foreign(void *fn) {
    const auto nature = []<class T>() { return std::is_integral_v<T> ? Nature::Int : Nature::Real; };
    const auto type = [](Nature n) { return n == Nature::Int ? "int" : "float"; };
    const Nature result = nature.template operator()<R>();
    const std::vector<Nature> args{nature.template operator()<A>()...};
    Registry registry;
    registry.AddFunction("probe", result, args, fn);
    std::string signature;
    for (Nature arg : args) signature += std::string(signature.empty() ? "" : ",") + type(arg);
    const std::string source = std::format("process = ffunction({} probe({}), \"math.h\", \"\");", type(result), signature);
    INFO(source);
    Fixture f(source.c_str());
    auto dsp = std::make_unique<Interp>(f.Plan, f.Ui, registry);
    dsp->Init(48000);
    double a[] = {2, -3, 4}, b[] = {-1, 5, 6}, output[3];
    const double *in[] = {a, b};
    double *out[] = {output};
    dsp->Compute(3, in, out);
    for (size_t k = 0; k < 3; ++k) CHECK(output[k] == 7 + (args.size() > 0 ? 3 * a[k] : 0) + (args.size() > 1 ? 5 * b[k] : 0));
}

} // namespace

TEST_CASE("instance controls and lifecycle preserve DSP state") {
    Fixture f("process = _ * hslider(\"gain\",0.5,0,1,0.01) : mem;");
    auto dsp = std::make_unique<Interp>(f.Plan, f.Ui);
    dsp->Init(48000);
    const auto labels = dsp->ControlsOfKind(UiKind::HSlider);
    REQUIRE(labels.size() == 1);
    double input[] = {1, 2, 3}, output[3]{};
    const double *ip[] = {input};
    double *op[] = {output};
    dsp->Compute(3, ip, op);
    CHECK(output[0] == 0);
    CHECK(output[1] == 0.5);
    CHECK(output[2] == 1);
    dsp->SetControl(labels[0], 0.25);
    dsp->Clear();
    CHECK(dsp->Control(labels[0]) == 0.25);
    dsp->Compute(3, ip, op);
    CHECK(output[0] == 0);
    CHECK(output[1] == 0.25);
    CHECK(output[2] == 0.5);
    dsp->ResetControls();
    CHECK(dsp->Control(labels[0]) == 0.5);
    dsp->Compute(0, nullptr, nullptr);
    dsp->Compute(1, nullptr, op);
    CHECK(output[0] == 0.75);
    dsp->Compute(1, nullptr, op);
    CHECK(output[0] == 0);
    dsp->Init(96000);
    CHECK(dsp->SampleRate == 96000);
    dsp->Compute(1, ip, op);
    CHECK(output[0] == 0);
}

TEST_CASE("instances expose sample rate and actual block size to every band") {
    Fixture f("process = fconstant(int fSamplingFreq, \"math.h\"), fvariable(int count, \"math.h\");");
    auto dsp = std::make_unique<Interp>(f.Plan, f.Ui);
    for (int rate : {44100, 96000}) {
        dsp->Init(rate);
        double sr[17], count[17];
        double *out[] = {sr, count};
        for (int frames : {1, 0, 17, 3}) {
            dsp->Compute(frames, nullptr, out);
            CHECK(dsp->Frames == frames);
            for (int i = 0; i < frames; ++i) {
                CHECK(sr[i] == rate);
                CHECK(count[i] == frames);
            }
        }
    }
}

TEST_CASE("instances preserve separate rounding of multiply and add") {
    Fixture f("process = (_ * _) - 1.0;");
    auto dsp = std::make_unique<Interp>(f.Plan, f.Ui);
    dsp->Init(48000);
    double x = 1.0 + 0x1p-27, y = 1.0 - 0x1p-27, result = 1;
    const double *in[] = {&x, &y};
    double *out[] = {&result};
    dsp->Compute(1, in, out);
    CHECK(std::bit_cast<uint64_t>(result) == uint64_t(0));
}

TEST_CASE("instances pass typed foreign arguments and results through the host ABI") {
    const auto check = []<class R>() {
        Foreign<R>(reinterpret_cast<void *>(&ForeignZero<R>));
        Foreign<R, double>(reinterpret_cast<void *>(&ForeignOne<R, double>));
        Foreign<R, int32_t>(reinterpret_cast<void *>(&ForeignOne<R, int32_t>));
        Foreign<R, double, double>(reinterpret_cast<void *>(&ForeignTwo<R, double, double>));
        Foreign<R, double, int32_t>(reinterpret_cast<void *>(&ForeignTwo<R, double, int32_t>));
        Foreign<R, int32_t, double>(reinterpret_cast<void *>(&ForeignTwo<R, int32_t, double>));
        Foreign<R, int32_t, int32_t>(reinterpret_cast<void *>(&ForeignTwo<R, int32_t, int32_t>));
    };
    check.template operator()<double>();
    check.template operator()<int32_t>();
}

TEST_CASE("instances sample foreign variables once per block and report missing bindings") {
    Fixture f("process = fvariable(float realvar, \"math.h\"), fvariable(int intvar, \"math.h\"), fvariable(float missing, \"math.h\");");
    Registry registry;
    double real = -0.25;
    int32_t integer = -9;
    registry.AddVariable("realvar", Nature::Real, &real);
    registry.AddVariable("intvar", Nature::Int, &integer);
    auto dsp = std::make_unique<Interp>(f.Plan, f.Ui, registry);
    REQUIRE(dsp->Diagnostics.size() == 1);
    dsp->Init(48000);
    for (int k = 0; k < 3; ++k) {
        double r[3], i[3], missing[3];
        double *out[] = {r, i, missing};
        dsp->Compute(3, nullptr, out);
        for (int f = 0; f < 3; ++f) {
            CHECK(r[f] == real);
            CHECK(i[f] == integer);
            CHECK(missing[f] == 0);
        }
        real += 0.5;
        integer += 4;
    }
}
