#include "Live.h"
#include "conformance/Sweep.h"
#include "runtime/Executors.h"

#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>
#include <thread>

using namespace faustlens;
using namespace faustlens::test;

namespace {

bool Same(double a, double b) { return (std::isnan(a) && std::isnan(b)) || std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b); }

struct Fixture {
    Program Program;
    faustlens::Plan Plan;
    UiNode Ui;
    Fixture(const char *source) : Program("/native.dsp", source) {
        REQUIRE(Program.Ok);
        const auto plan = Program.Lower();
        REQUIRE(plan);
        Plan = *plan;
        Ui = Program.Ui("native");
    }
};

template<class R> R ForeignZero() { return R(7); }
template<class R, class A> R ForeignOne(A a) { return R(7 + 3 * double(a)); }
template<class R, class A, class B> R ForeignTwo(A a, B b) { return R(7 + 3 * double(a) + 5 * double(b)); }

template<class Backend, class R, class... A> void Foreign(void *fn) {
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
    auto dsp = MakeExecutor<Backend>(f.Plan, f.Ui, registry);
    dsp->Init(48000);
    double a[] = {2, -3, 4}, b[] = {-1, 5, 6}, output[3];
    const double *in[] = {a, b};
    double *out[] = {output};
    dsp->Compute(3, in, out);
    for (size_t k = 0; k < 3; ++k) CHECK(output[k] == 7 + (args.size() > 0 ? 3 * a[k] : 0) + (args.size() > 1 ? 5 * b[k] : 0));
}

} // namespace

TEST_CASE_TEMPLATE("executors share control and lifecycle behavior", Backend, FAUSTLENS_TEST_EXECUTORS) {
    Fixture f("process = _ * hslider(\"gain\",0.5,0,1,0.01) : mem;");
    auto dsp = MakeExecutor<Backend>(f.Plan, f.Ui);
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

TEST_CASE_TEMPLATE("executors expose sample rate and actual block size to every band", Backend, FAUSTLENS_TEST_EXECUTORS) {
    Fixture f("process = fconstant(int fSamplingFreq, \"math.h\"), fvariable(int count, \"math.h\");");
    auto dsp = MakeExecutor<Backend>(f.Plan, f.Ui);
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

TEST_CASE_TEMPLATE("executors preserve separate rounding of multiply and add", Backend, FAUSTLENS_TEST_EXECUTORS) {
    Fixture f("process = (_ * _) - 1.0;");
    auto dsp = MakeExecutor<Backend>(f.Plan, f.Ui);
    dsp->Init(48000);
    double x = 1.0 + 0x1p-27, y = 1.0 - 0x1p-27, result = 1;
    const double *in[] = {&x, &y};
    double *out[] = {&result};
    dsp->Compute(1, in, out);
    CHECK(std::bit_cast<uint64_t>(result) == uint64_t(0));
}

TEST_CASE_TEMPLATE("executors pass typed foreign arguments and results through the host ABI", Backend, FAUSTLENS_TEST_EXECUTORS) {
    const auto check = []<class R>() {
        Foreign<Backend, R>(reinterpret_cast<void *>(&ForeignZero<R>));
        Foreign<Backend, R, double>(reinterpret_cast<void *>(&ForeignOne<R, double>));
        Foreign<Backend, R, int32_t>(reinterpret_cast<void *>(&ForeignOne<R, int32_t>));
        Foreign<Backend, R, double, double>(reinterpret_cast<void *>(&ForeignTwo<R, double, double>));
        Foreign<Backend, R, double, int32_t>(reinterpret_cast<void *>(&ForeignTwo<R, double, int32_t>));
        Foreign<Backend, R, int32_t, double>(reinterpret_cast<void *>(&ForeignTwo<R, int32_t, double>));
        Foreign<Backend, R, int32_t, int32_t>(reinterpret_cast<void *>(&ForeignTwo<R, int32_t, int32_t>));
    };
    check.template operator()<double>();
    check.template operator()<int32_t>();
}

TEST_CASE_TEMPLATE("executors sample foreign variables once per block and report missing bindings", Backend, FAUSTLENS_TEST_EXECUTORS) {
    Fixture f("process = fvariable(float realvar, \"math.h\"), fvariable(int intvar, \"math.h\"), fvariable(float missing, \"math.h\");");
    Registry registry;
    double real = -0.25;
    int32_t integer = -9;
    registry.AddVariable("realvar", Nature::Real, &real);
    registry.AddVariable("intvar", Nature::Int, &integer);
    auto dsp = MakeExecutor<Backend>(f.Plan, f.Ui, registry);
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

#if defined(__APPLE__) && defined(__aarch64__)
TEST_CASE("native arithmetic agrees at full precision under repeated blocks and resets") {
    fenv_t saved;
    std::fegetenv(&saved);
    struct Restore {
        fenv_t &Saved;
        ~Restore() { std::fesetenv(&Saved); }
    } restore{saved};
    for (int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO})
        for (bool flush : {false, true}) {
            std::fesetenv(FE_DFL_ENV);
            if (flush) audio::EnableFlushToZero();
            std::fesetround(rounding);
            INFO(rounding, flush);
            const char *sources[] = {
                "process = (_ * 1.0000000000000002) - _;",
                "process = sin(_) + _;",
                "process = _ < _;",
                "process = _ <= _;",
                "process = _ >= _;",
                "process = _ > _;",
                "process = _ == _;",
                "process = _ != _;",
                "process = _ / _;",
                "process = int(_);",
                "process = float(int(_));",
                "process = int(_) / int(_);",
                "process = int(_) % int(_);",
                "process = int(_) << int(_);",
                "process = int(_) >> int(_);",
                "process = _ % _;",
                "process = min(_, _);",
                "process = max(_, _);",
                "process = min(int(_), int(_));",
                "process = max(int(_), int(_));",
                "process = abs(int(_));",
                "process = select2(int(_), _, 0.5);",
                "process = select3(int(_), _, 0.5, -0.5);",
                "process = pow(_, 0);",
                "process = pow(_, 2);",
                "process = pow(_, 8);",
                "process = pow(_, _);",
                "process = abs(_);",
                "process = acos(_);",
                "process = asin(_);",
                "process = atan(_);",
                "process = ceil(_);",
                "process = cos(_);",
                "process = exp(_);",
                "process = floor(_);",
                "process = log(_);",
                "process = log10(_);",
                "process = rint(_);",
                "process = round(_);",
                "process = sqrt(_);",
                "process = tan(_);",
                "process = atan2(_, _);",
                "process = fmod(_, _);",
                "process = remainder(_, _);",
                "process = int(_) * int(_);",
                "process = int(_) + int(_);",
                "process = _ @ 8;",
                "process = _ @ 64;",
                "process = (+ : *(0.9999)) ~ _;",
                "process = prefix(0.25, _);",
                "process = control(_, int(_));",
                "process = _ * hslider(\"gain\",0.37,0,1,0.01);"
            };
            const double values[] = {
                0.,
                -0.,
                1.,
                -1.,
                0.1,
                100000.,
                2147483647.,
                -2147483648.,
                std::numeric_limits<double>::denorm_min(),
                std::numeric_limits<double>::infinity(),
                -std::numeric_limits<double>::infinity(),
                std::numeric_limits<double>::quiet_NaN()
            };
            for (const char *source : sources) {
                INFO(std::string(source));
                Fixture f(source);
                auto a = MakeExecutor<Interp>(f.Plan, f.Ui), b = MakeExecutor<Native>(f.Plan, f.Ui);
                a->Init(48000);
                b->Init(48000);
                std::vector<double> x(127), y(127), ao(127), bo(127);
                const double *ip[] = {x.data(), y.data()};
                double *ap[] = {ao.data()}, *bp[] = {bo.data()};
                for (int block = 0; block < 200; ++block) {
                    const int n = block % 127 + 1;
                    for (int j = 0; j < n; ++j) {
                        x[j] = block < 12 ? values[(block + j) % 12] : std::sin(0.01 * (block * 127 + j));
                        y[j] = block < 12 ? values[(block + j + 2) % 12] : std::cos(0.02 * (block * 127 + j));
                    }
                    if (block == 12) {
                        a->Init(44100);
                        b->Init(44100);
                    }
                    a->Compute(n, ip, ap);
                    b->Compute(n, ip, bp);
                    for (int j = 0; j < n; ++j) {
                        INFO(block, j, ao[j], bo[j]);
                        REQUIRE(Same(ao[j], bo[j]));
                    }
                }
                REQUIRE(a->State.size() == b->State.size());
                for (size_t k = 0; k < a->State.size(); ++k) CHECK(std::bit_cast<uint64_t>(a->State[k]) == std::bit_cast<uint64_t>(b->State[k]));
            }
        }
}

TEST_CASE("native storage forwarding preserves field conversion") {
    Fixture f("process = rwtable(4, 0.0, 0, _, 0);");
    for (auto &field : f.Plan.Fields) field.Nature = Nature::Int;
    for (auto &band : f.Plan.Bands)
        for (auto &i : band)
            if (Op(i.Op) == Op::LoadField) i.Nature = Nature::Int;
    auto dsp = MakeExecutor<Native>(f.Plan, f.Ui);
    dsp->Init(48000);
    double input[] = {1.75, -2.75, 3.5}, output[3];
    const double *in[] = {input};
    double *out[] = {output};
    dsp->Compute(3, in, out);
    CHECK(output[0] == 1);
    CHECK(output[1] == -2);
    CHECK(output[2] == 3);
}

TEST_CASE("native loop allocation retains values defined before a loop") {
    Fixture f("process = rdtable(8, atan(2.0 + float(+(1) ~ _)) + sin(3.0 + float(+(1) ~ _)), 7);");
    auto &init = f.Plan.Bands[0];
    const auto constant = std::ranges::find_if(init, [](const Instr &i) { return Op(i.Op) == Op::ConstReal && RealOf(i.Imm, i.Aux) == 2.0; });
    REQUIRE(constant != init.end());
    const Instr moved = *constant;
    init.erase(constant);
    init.insert(init.begin(), moved);
    auto interp = MakeExecutor<Interp>(f.Plan, f.Ui), native = MakeExecutor<Native>(f.Plan, f.Ui);
    interp->Init(48000);
    native->Init(48000);
    double a[3], b[3];
    double *outA[] = {a}, *outB[] = {b};
    interp->Compute(3, nullptr, outA);
    native->Compute(3, nullptr, outB);
    for (int k = 0; k < 3; ++k) CHECK(Same(a[k], b[k]));
    for (size_t k = 0; k < interp->State.size(); ++k) CHECK(Same(interp->State[k].D, native->State[k].D));
}

TEST_CASE("native code survives callback publication and retires with its artifact") {
    Session s;
    app::Live live;
    s.SetBuffer("/n.dsp", "process=(+ : *(0.9)) ~ _;");
    REQUIRE(live.Reload(s, "/n.dsp").Compiled);
    auto &host = live.Host;
    host.Chunk = 16;
    host.DeviceIn = host.DeviceOut = 1;
    host.SampleRate = 1000;
    host.Current = host.MakeVoice(*live.Current->Dsp).release();
    host.Running = true;
    float input[16] = {1}, out[16]{};
    host.Process(input, out, 4);
    std::weak_ptr<app::Artifact> old = live.Current;
    for (int generation = 0; generation < 8; ++generation) {
        s.SetBuffer("/n.dsp", std::format("process=(+ : *({})) ~ _;", 0.8 - 0.01 * generation));
        const Backend backend = generation % 3 == 2 ? Backend::Interp : Backend::Native;
        auto prepared = app::Live::Build(s, "/n.dsp", live.Current, {}, 1000, live.Sound, backend);
        REQUIRE(prepared.Next);
        CHECK(prepared.Next->Execution == backend);
        host.Process(nullptr, out, 4);
        const double before = out[3];
        REQUIRE(live.Accept(prepared).Swapped);
        CHECK_FALSE(old.expired());
        std::thread callback([&] { host.Process(nullptr, out, 6); });
        callback.join();
        CHECK(out[5] == doctest::Approx(before * std::pow(0.8 - 0.01 * generation, 6)).epsilon(1e-6));
        prepared.Base.reset();
        auto garbage = live.Collect();
        REQUIRE(garbage.size() == 1);
        CHECK_FALSE(old.expired());
        garbage.clear();
        CHECK(old.expired());
        old = live.Current;
    }
    host.Stop();
    live.Collect();
}

TEST_CASE("native publication preserves code executing on another thread") {
    Fixture f("process = _ * 0.5;");
    auto running = MakeExecutor<Native>(f.Plan, f.Ui);
    running->Init(48000);
    std::atomic<bool> valid = true;
    std::atomic<uint64_t> blocks = 0;
    std::jthread execution([&](std::stop_token stop) {
        double input[32], output[32];
        std::fill_n(input, 32, 1.0);
        const double *ip[] = {input};
        double *op[] = {output};
        while (!stop.stop_requested()) {
            running->Compute(32, ip, op);
            for (double value : output)
                if (value != 0.5) valid.store(false);
            blocks.fetch_add(1);
        }
    });
    while (blocks.load() == 0) std::this_thread::yield();
    for (int i = 0; i < 128; ++i) {
        auto replacement = MakeExecutor<Native>(f.Plan, f.Ui);
        replacement->Init(48000);
        double input = 2, output = 0;
        const double *ip[] = {&input};
        double *op[] = {&output};
        replacement->Compute(1, ip, op);
        REQUIRE(output == 1);
    }
    execution.request_stop();
    execution.join();
    CHECK(valid.load());
    CHECK(blocks.load() > 0);
}

TEST_CASE("unsupported native instructions produce a diagnostic") {
    Fixture f("process = _;");
    f.Plan.Bands[2][0].Op = uint8_t(Op::BitCast);
    const auto code = Native::Compile(f.Plan, f.Ui);
    REQUIRE_FALSE(code);
    CHECK(code.error().find("does not support") != std::string::npos);
}
#endif
