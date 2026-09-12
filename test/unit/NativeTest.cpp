#include "runtime/Float.h"
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

struct FloatingEnvironment {
    fenv_t Saved;
    FloatingEnvironment() { std::fegetenv(&Saved); }
    ~FloatingEnvironment() { std::fesetenv(&Saved); }
};

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

#if defined(__APPLE__) && defined(__aarch64__)
double ForeignClobber(double x) {
    asm volatile("mov x0, xzr\nmov x1, xzr\nmov x2, xzr\nmov x3, xzr\n"
                 "mov x4, xzr\nmov x5, xzr\nmov x6, xzr\nmov x7, xzr\n"
                 "mov x8, xzr\nmov x9, xzr\nmov x10, xzr\nmov x11, xzr\n"
                 "mov x12, xzr\nmov x13, #32\nmov x14, xzr\nmov x15, xzr\n"
                 "mov x16, xzr\nmov x17, xzr\n"
                 "movi v0.16b, #0\nmovi v1.16b, #0\nmovi v2.16b, #0\nmovi v3.16b, #0\n"
                 "movi v4.16b, #0\nmovi v5.16b, #0\nmovi v6.16b, #0\nmovi v7.16b, #0\n"
                 "movi v16.16b, #0\nmovi v17.16b, #0\nmovi v18.16b, #0\nmovi v19.16b, #0\n"
                 "movi v20.16b, #0\nmovi v21.16b, #0\nmovi v22.16b, #0\nmovi v23.16b, #0\n"
                 "movi v24.16b, #0\nmovi v25.16b, #0\nmovi v26.16b, #0\nmovi v27.16b, #0\n"
                 "movi v28.16b, #0\nmovi v29.16b, #0\nmovi v30.16b, #0\nmovi v31.16b, #0\n"
                 :
                 :
                 : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "v0", "v1", "v2", "v3",
                   "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
    return x + 0.25;
}
#endif

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

TEST_CASE_TEMPLATE("table generators restart when constants are recomputed", Backend, FAUSTLENS_TEST_EXECUTORS) {
    Fixture f("process(x) = rdtable(4, +(1)~_, int(x)%4), rdtable(4, +(0.5)~_, int(x)%4), +(1)~_;");
    auto dsp = MakeExecutor<Backend>(f.Plan, f.Ui);
    double input[] = {0, 1, 2, 3}, integers[4], reals[4], counter[4];
    const double *in[] = {input};
    double *out[] = {integers, reals, counter};
    for (int repeat = 0; repeat < 3; ++repeat) {
        dsp->Init(48000);
        dsp->Compute(4, in, out);
        for (int i = 0; i < 4; ++i) {
            CHECK(integers[i] == i + 1);
            CHECK(reals[i] == (i + 1) * 0.5);
            CHECK(counter[i] == i + 1);
        }
        dsp->Constants(96000);
        dsp->Compute(4, in, out);
        for (int i = 0; i < 4; ++i) {
            CHECK(integers[i] == i + 1);
            CHECK(reals[i] == (i + 1) * 0.5);
            CHECK(counter[i] == i + 5);
        }
    }
}

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
TEST_CASE("native blocks retain control results across empty blocks and sample calls") {
    for (const char *source :
         {"process(x)=sin(hslider(\"gain\",0.5,0,1,0.01))*x;", "process(x)=par(i,40,sin(hslider(\"gain\",0.5,0,1,0.01)+float(i))*sin(x)+x');",
          "process(x)=select2(1',x,x+sin(hslider(\"gain\",0.5,0,1,0.01)));"}) {
        Fixture f(source);
        for (bool samples : {false, true}) {
            auto plan = f.Plan;
            if (!samples) plan.Band(Band::Sample).clear();
            auto a = MakeExecutor<Interp>(plan, f.Ui), b = MakeExecutor<Native>(plan, f.Ui);
            a->Init(48000);
            b->Init(48000);
            for (int frames : {-3, 0, 1, 3, 0, 17}) {
                INFO(std::string_view(source), " samples=", samples, " frames=", frames);
                const auto gain = a->ControlsOfKind(UiKind::HSlider)[0];
                a->SetControl(gain, 0.25 + double(frames & 1) * 0.5);
                b->SetControl(gain, a->Control(gain));
                std::array<double, 17> input;
                input.fill(-0.125);
                const double *in[] = {input.data()};
                std::vector<std::array<double, 17>> ao(a->Outputs()), bo(a->Outputs());
                std::vector<double *> ap, bp;
                for (size_t k = 0; k < ao.size(); ++k) {
                    ao[k].fill(-7);
                    bo[k] = ao[k];
                    ap.push_back(ao[k].data());
                    bp.push_back(bo[k].data());
                }
                a->Compute(frames, in, ap.data());
                b->Compute(frames, in, bp.data());
                CHECK(ao == bo);
                for (const Instr &i : plan.Band(Band::Control))
                    if (i.Dst != NoReg && a->Registers.Slot[i.Dst] != NoReg) {
                        const uint32_t slot = a->Registers.Slot[i.Dst];
                        CHECK(Same(a->Values[slot].D, b->Values[slot].D));
                    }
                for (size_t k = 0; k < a->State.size(); ++k) CHECK(Same(a->State[k].D, b->State[k].D));
            }
        }
    }
}

TEST_CASE("native blocks preserve prior control values through guards and empty loops") {
    enum { Unconditional, Guarded, Looped, ReadBeforeWrite };
    for (Nature type : {Nature::Int, Nature::Real})
        for (int mode : {Unconditional, Guarded, Looped, ReadBeforeWrite})
            for (uint32_t bound : {0u, 2u}) {
                INFO(int(type), mode, bound);
                Plan plan;
                plan.Regs = 4;
                plan.Inputs = 1;
                plan.Outputs = 3;
                plan.Operands = {0, 1, 2, 3};
                plan.Fields.resize(3);
                plan.Fields[0].Nature = Nature::Int;
                plan.Fields[0].Kind = plan.Fields[1].Kind = FieldKind::Widget;
                plan.Fields[1].Nature = plan.Fields[2].Nature = type;
                const uint64_t initial = type == Nature::Int ? 7 : std::bit_cast<uint64_t>(7.0);
                plan.Band(Band::Init) = {
                    {uint8_t(type == Nature::Int ? Op::ConstInt : Op::ConstReal), 0, type, 0, uint32_t(initial), uint32_t(initial >> 32)},
                    {uint8_t(Op::ConstInt), 0, Nature::Int, 2, 9}
                };
                auto &control = plan.Band(Band::Control);
                control.push_back({uint8_t(Op::LoadField), 0, Nature::Int, 1, 0});
                if (mode == Guarded) control.push_back({uint8_t(Op::GuardBegin), 0, Nature::Int, NoReg, 0, 0, 1, 1});
                if (mode == Looped) control.push_back({uint8_t(Op::LoopBegin), 0, Nature::Int, 2, bound});
                if (mode == ReadBeforeWrite) control.push_back({uint8_t(Op::StoreField), 0, type, NoReg, 2, 0, 0, 1});
                control.push_back({uint8_t(Op::LoadField), 0, type, 0, 1});
                if (mode == Guarded) control.push_back({uint8_t(Op::GuardEnd), 0, Nature::Int, NoReg});
                if (mode == Looped) control.push_back({uint8_t(Op::LoopEnd), 0, Nature::Int, NoReg});
                plan.Band(Band::Sample) = {
                    {uint8_t(Op::Input), 0, Nature::Real, 3, 0},
                    {uint8_t(Op::Output), 0, Nature::Real, NoReg, 0, 0, 0, 1},
                    {uint8_t(Op::Output), 0, Nature::Real, NoReg, 1, 0, 2, 1},
                    {uint8_t(Op::Output), 0, Nature::Real, NoReg, 2, 0, 3, 1}
                };
                auto a = MakeExecutor<Interp>(plan, {}), b = MakeExecutor<Native>(plan, {});
                a->Init(48000);
                b->Init(48000);
                double input[] = {1, 2, 3, 4, 5, 6, 7};
                const double *in[] = {input};
                double ao[3][7]{}, bo[3][7]{};
                double *ap[] = {ao[0], ao[1], ao[2]}, *bp[] = {bo[0], bo[1], bo[2]};
                for (bool connected : {true, false})
                    for (int gate : {0, 1, 0})
                        for (int frames : {0, 1, 7, 0}) {
                            a->State[0].I = gate;
                            if (type == Nature::Int) a->State[1].I += 3;
                            else a->State[1].D += 0.25;
                            b->State = a->State;
                            a->Compute(frames, connected ? in : nullptr, ap);
                            b->Compute(frames, connected ? in : nullptr, bp);
                            for (int c = 0; c < 3; ++c)
                                for (int k = 0; k < frames; ++k) CHECK(Same(ao[c][k], bo[c][k]));
                            for (size_t k = 0; k < a->Values.size(); ++k) CHECK(Same(a->Values[k].D, b->Values[k].D));
                            CHECK(Same(a->State[2].D, b->State[2].D));
                        }
            }
}

TEST_CASE("native calls preserve live values across caller-saved register clobbers") {
    constexpr Reg count = 40;
    Plan plan;
    plan.Regs = plan.Outputs = 2 * count + 2;
    plan.Fields.resize(1);
    plan.Foreign = {
        {ForeignKind::Function, "clobber", Nature::Real, {Nature::Real}},
        {ForeignKind::Constant, "fSamplingFreq", Nature::Int, {}},
        {ForeignKind::Variable, "count", Nature::Int, {}}
    };
    for (Reg r = 0; r < plan.Regs; ++r) plan.Operands.push_back(r);
    for (Reg r = 0; r < count; ++r) {
        const uint64_t bits = std::bit_cast<uint64_t>(double(r) + 0.5);
        plan.Band(Band::Init).push_back({uint8_t(Op::ConstReal), 0, Nature::Real, r, uint32_t(bits), uint32_t(bits >> 32)});
        plan.Band(Band::Sample).push_back({uint8_t(Op::FFun), 0, Nature::Real, count + r, 0, 0, r, 1});
        plan.Band(Band::Sample).push_back({uint8_t(Op::StoreField), 0, Nature::Real, NoReg, 0, 0, count + r, 1});
    }
    plan.Band(Band::Sample).push_back({uint8_t(Op::FConst), 0, Nature::Int, 2 * count, 1});
    plan.Band(Band::Sample).push_back({uint8_t(Op::FVar), 0, Nature::Int, 2 * count + 1, 2});
    for (Reg r = 0; r < plan.Regs; ++r) plan.Band(Band::Sample).push_back({uint8_t(Op::Output), 0, Nature::Real, NoReg, r, 0, r, 1});
    Registry registry = Registry::Builtin();
    registry.AddFunction("clobber", Nature::Real, {Nature::Real}, reinterpret_cast<void *>(&ForeignClobber));
    auto native = MakeExecutor<Native>(plan, {}, registry);
    native->Init(48000);
    std::array<std::array<double, 17>, 2 * count + 2> output;
    std::array<double *, 2 * count + 2> channels;
    for (Reg r = 0; r < plan.Regs; ++r) channels[r] = output[r].data();
    for (int frames : {1, 17, 3}) {
        native->Compute(frames, nullptr, channels.data());
        for (int k = 0; k < frames; ++k) {
            for (Reg r = 0; r < 2 * count; ++r) CHECK(output[r][k] == double(r % count) + (r < count ? 0.5 : 0.75));
            CHECK(output[2 * count][k] == 48000);
            CHECK(output[2 * count + 1][k] == frames);
        }
        CHECK(native->State[0].D == count - 0.25);
    }
}

TEST_CASE("native cached math calls preserve block boundaries after volatile register clobbers") {
    Plan plan;
    plan.Regs = 2;
    plan.Outputs = 1;
    plan.Operands = {0, 1};
    plan.Foreign.push_back({ForeignKind::Function, "clobber", Nature::Real, {Nature::Real}});
    plan.Band(Band::Init) = {{uint8_t(Op::ConstReal), 0, Nature::Real, 0, 0, 0x3fe00000}};
    plan.Band(Band::Sample) = {
        {uint8_t(Op::Extended), uint8_t(Ext::Sin), Nature::Real, 1, 0, 0, 0, 1}, {uint8_t(Op::Output), 0, Nature::Real, NoReg, 0, 0, 1, 1}
    };
    auto compiled = arm64::Program::Compile(plan, {});
    REQUIRE(compiled);
    auto decoded = arm64::Program::Decode((*compiled)->Encode());
    REQUIRE(decoded);
    auto program = std::make_shared<arm64::Program>(**decoded);
    for (auto &relocation : program->Relocations)
        if (relocation.Kind == arm64::Binding::Math) {
            relocation.Kind = arm64::Binding::Foreign;
            relocation.Index = 0;
        }
    Registry registry;
    registry.AddFunction("clobber", Nature::Real, {Nature::Real}, reinterpret_cast<void *>(&ForeignClobber));
    auto code = NativeCode::Publish(program, registry);
    REQUIRE(code);
    auto native = Native::Create(*code);
    native->Init(48000);
    std::array<double, 64> output;
    double *channels[] = {output.data()};
    for (int frames : {0, 1, 2, 3, 4, 5, 7, 8, 9, 17, 31, 32, 33}) {
        output.fill(-1);
        native->Compute(frames, nullptr, channels);
        for (size_t k = 0; k < output.size(); ++k) CHECK(output[k] == (k < size_t(frames) ? 0.75 : -1));
    }
}

TEST_CASE("native arithmetic agrees at full precision under repeated blocks and resets") {
    FloatingEnvironment restore;
    for (int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO})
        for (bool flush : {false, true}) {
            std::fesetenv(FE_DFL_ENV);
            if (flush) EnableFlushToZero();
            std::fesetround(rounding);
            INFO(rounding, flush);
            const char *sources[] = {
                "process = (_ * 1.0000000000000002) - _;",
                "process = sin(_) + _;",
                "process(x) = sum(i,40,sin(x*float(i+1))/float(i+1));",
                "process(x) = sin(x)+cos(x)+atan(x)+exp(x)+log(x)+log10(x)+tan(x)+acos(x);",
                "process(x) = control(sin(x),x>0) + cos(x);",
                "process(x) = sin(x) + x' + cos(x);",
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
                "process = select2(int(_), int(_), 7);",
                "process = select3(int(_), int(_), 7, -7);",
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
                "process(x,y) = sin(x*y)-y;",
                "process(x,y) = x-sin(x*y);",
                "process(x,y) = atan2(sin(x*y),y);",
                "process(x,y) = atan2(y,sin(x*y));",
                "process(x) = sin(cos(x));",
                "process(x) = s*s with { s=sin(x); };",
                "process(x) = atan2(s,s) with { s=sin(x); };",
                "process(x) = s*s+s with { s=sin(x); };",
                "process(x) = control(sin(x*0.25)*2,x>0);",
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

TEST_CASE("native constants preserve exact bits across immediate encodings and spills") {
    FloatingEnvironment restore;
    std::vector<uint64_t> bits{0, 1ull << 63, 1, (1ull << 63) | 1, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000123};
    for (int exponent = -4; exponent <= 5; ++exponent)
        for (int mantissa = 16; mantissa < 32; ++mantissa)
            for (double sign : {-1., 1.}) {
                const double value = std::ldexp(sign * mantissa / 16, exponent);
                bits.push_back(std::bit_cast<uint64_t>(value));
                bits.push_back(std::bit_cast<uint64_t>(std::nextafter(value, 0.)));
            }
    for (Band band : {Band::Init, Band::Sample}) {
        Plan plan;
        plan.Regs = plan.Outputs = uint32_t(bits.size());
        for (Reg r = 0; r < plan.Regs; ++r) {
            plan.Band(band).push_back({uint8_t(Op::ConstReal), 0, Nature::Real, r, uint32_t(bits[r]), uint32_t(bits[r] >> 32)});
            plan.Operands.push_back(r);
        }
        for (Reg r = 0; r < plan.Regs; ++r) plan.Band(Band::Sample).push_back({uint8_t(Op::Output), 0, Nature::Real, NoReg, r, 0, r, 1});
        auto native = MakeExecutor<Native>(plan, {});
        std::vector<std::array<double, 2>> output(bits.size());
        std::vector<double *> pointers;
        for (auto &channel : output) pointers.push_back(channel.data());
        for (int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO})
            for (bool flush : {false, true}) {
                std::fesetenv(FE_DFL_ENV);
                std::fesetround(rounding);
                if (flush) EnableFlushToZero();
                native->Init(48000);
                native->Compute(2, nullptr, pointers.data());
                for (size_t r = 0; r < bits.size(); ++r)
                    for (double value : output[r]) CHECK(std::bit_cast<uint64_t>(value) == bits[r]);
            }
    }
}

TEST_CASE("native literal pools preserve short loops and large artifacts") {
    for (Band band : {Band::Control, Band::Sample})
        for (uint32_t count : {1u, 4u, 65530u, 140000u}) {
            Plan plan;
            plan.Regs = 1;
            plan.Operands = {0};
            std::vector<uint64_t> expected;
            for (uint32_t k = 0; k < count; ++k) {
                const uint64_t bits = 0x3fe123456789abcd + k;
                plan.Band(band).push_back({uint8_t(Op::ConstReal), 0, Nature::Real, 0, uint32_t(bits), uint32_t(bits >> 32)});
                if (k % 32768 == 0 || k + 1 == count) {
                    plan.Band(band).push_back({uint8_t(Op::Output), 0, Nature::Real, NoReg, uint32_t(plan.Outputs++), 0, 0, 1});
                    expected.push_back(bits);
                }
            }
            if (band == Band::Control) {
                plan.Band(Band::Sample).push_back({uint8_t(Op::Output), 0, Nature::Real, NoReg, uint32_t(plan.Outputs++), 0, 0, 1});
                expected.push_back(expected.back());
            }
            auto compiled = arm64::Program::Compile(std::move(plan), {});
            REQUIRE(compiled);
            auto decoded = arm64::Program::Decode((*compiled)->Encode());
            REQUIRE(decoded);
            auto code = NativeCode::Publish(*decoded);
            REQUIRE(code);
            auto native = Native::Create(*code);
            native->Init(48000);
            std::vector<std::array<double, 5>> output(expected.size());
            std::vector<double *> pointers;
            for (auto &channel : output) pointers.push_back(channel.data());
            native->Compute(5, nullptr, pointers.data());
            for (size_t k = 0; k < output.size(); ++k)
                for (size_t frame = 0; frame < output[k].size(); ++frame)
                    CHECK(std::bit_cast<uint64_t>(output[k][frame]) == (band == Band::Sample || k + 1 == output.size() || !frame ? expected[k] : 0));
        }
}

TEST_CASE("native constant powers preserve aliased and spilled bases") {
    for (Nature type : {Nature::Int, Nature::Real})
        for (Reg count : {1u, 40u})
            for (int exponent : {-2, 0, 1, 2, 3, 8}) {
                INFO(type, count, exponent);
                Plan plan;
                plan.Regs = count + 2;
                plan.Inputs = 1;
                plan.Outputs = count;
                plan.Operands = {0};
                plan.Band(Band::Init) = {{uint8_t(Op::ConstInt), 0, Nature::Int, count + 1, uint32_t(exponent)}};
                plan.Band(Band::Sample).push_back({uint8_t(Op::Input), 0, Nature::Real, 0});
                for (Reg r = 1; r <= count; ++r)
                    plan.Band(Band::Sample).push_back({uint8_t(type == Nature::Int ? Op::IntCast : Op::FloatCast), 0, type, r, 0, 0, 0, 1});
                for (Reg r = 1; r <= count; ++r) {
                    const uint32_t at = uint32_t(plan.Operands.size());
                    plan.Operands.insert(plan.Operands.end(), {r, count + 1});
                    plan.Band(Band::Sample).push_back({uint8_t(Op::Extended), uint8_t(Ext::Pow), type, r, 0, 0, at, 2});
                }
                for (Reg r = 1; r <= count; ++r) plan.Band(Band::Sample).push_back({uint8_t(Op::Output), 0, Nature::Real, NoReg, r - 1, 0, 2 * r - 1, 1});
                auto reference = MakeExecutor<Interp>(plan, {}), native = MakeExecutor<Native>(plan, {});
                reference->Init(48000);
                native->Init(48000);
                const double input[] = {-2.125, -0.0, 0.125, 1.0000000000000002, 100001., 2147483647.};
                const double *inputs[] = {input};
                std::vector<std::array<double, 6>> expected(count), actual(count);
                std::vector<double *> a(count), b(count);
                for (Reg r = 0; r < count; ++r) {
                    a[r] = expected[r].data();
                    b[r] = actual[r].data();
                }
                reference->Compute(6, inputs, a.data());
                native->Compute(6, inputs, b.data());
                for (Reg r = 0; r < count; ++r)
                    for (size_t k = 0; k < std::size(input); ++k) CHECK(Same(expected[r][k], actual[r][k]));
            }
}

TEST_CASE("native scaled integer conversions preserve boundaries and shared products") {
    FloatingEnvironment restore;
    uint64_t random = 0x923ace847152;
    for (unsigned scale = 1; scale <= 32; ++scale) {
        std::vector<double> input{
            0.,
            -0.,
            std::numeric_limits<double>::denorm_min(),
            -std::numeric_limits<double>::denorm_min(),
            std::numeric_limits<double>::max(),
            -std::numeric_limits<double>::max(),
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()
        };
        for (double integer : {-2147483649., -2147483648., -1., 0., 1., 2147483647., 2147483648.}) {
            const double boundary = std::ldexp(integer, -int(scale));
            input.insert(input.end(), {std::nextafter(boundary, -INFINITY), boundary, std::nextafter(boundary, INFINITY)});
        }
        for (int k = 0; k < 512; ++k) {
            random = random * 6364136223846793005ull + 1;
            input.push_back(std::bit_cast<double>(random));
        }
        for (bool shared : {false, true}) {
            const std::string source = std::format("process = _ * {}.0 {}", 1ull << scale, shared ? "<: int, _;" : ": int;");
            Fixture f(source.c_str());
            auto a = MakeExecutor<Interp>(f.Plan, f.Ui), b = MakeExecutor<Native>(f.Plan, f.Ui);
            std::vector<double> ao[2], bo[2];
            for (int c = 0; c < 2; ++c) {
                ao[c].resize(input.size());
                bo[c].resize(input.size());
            }
            const double *ip[] = {input.data()};
            double *ap[] = {ao[0].data(), ao[1].data()}, *bp[] = {bo[0].data(), bo[1].data()};
            for (int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO})
                for (bool flush : {false, true}) {
                    std::fesetenv(FE_DFL_ENV);
                    if (flush) EnableFlushToZero();
                    std::fesetround(rounding);
                    INFO(scale, shared, rounding, flush);
                    a->Init(48000);
                    b->Init(48000);
                    a->Compute(int(input.size()), ip, ap);
                    b->Compute(int(input.size()), ip, bp);
                    for (int c = 0; c < a->Outputs(); ++c)
                        for (size_t k = 0; k < input.size(); ++k) {
                            INFO(c, k, input[k], ao[c][k], bo[c][k]);
                            REQUIRE(Same(ao[c][k], bo[c][k]));
                        }
                }
        }
    }
}

TEST_CASE("native sample loops preserve block boundaries and restored state") {
    for (const char *source :
         {"process(x)=select2(1',x,x+2);", "process(x)=select3(2',x,x+1,x+2);", "process(x)=select2(1'-1,x,x+2);", "process(x)=select2(1',x,sin(x));",
          "process(x)=(waveform{0.1,0.2,0.3,0.4},int(select2(1',x,sin(x)))&3:rdtable);", "process(x)=control(select2(1',x,x+1),int(x)>0);",
          "process(x)=rwtable(16,0,int(x)&15,1,0);", "process=2147483647'+1,(-2147483648)'/(-1)',(-2147483648)'%(-1)',1'/0',1'%0';",
          "process(x)=int(x)&15,float(15),int(x)/15;", "process(x)=rdtable(4,pow(float(+(1)~_),3),int(x)&3);", "process(x)=par(i,32, x+float(i+1):mem);",
          "process(x)=par(i,32, sin(x+float(i+1)):mem);", "process(x)=x*float(int(hslider(\"k\",3,0,15,1))&15);",
          "import(\"stdfaust.lib\"); process=os.osc(440);"}) {
        INFO(source);
        Fixture f(source);
        auto a = MakeExecutor<Interp>(f.Plan, f.Ui), b = MakeExecutor<Native>(f.Plan, f.Ui);
        for (int32_t seed : {0, -1, INT32_MIN, INT32_MAX}) {
            a->Init(48000);
            for (size_t k = 0; k < f.Plan.Fields.size(); ++k) {
                const auto &field = f.Plan.Fields[k];
                if (field.Kind == FieldKind::Delay || field.Kind == FieldKind::Perm)
                    for (uint32_t j = 0; j < field.Extent; ++j) {
                        auto &value = a->State[a->FieldAt[k] + j];
                        if (field.Nature == Nature::Int) value.I = seed;
                        else value.D = seed ? 0.125 * double(k + j + 1) : j % 2 ? 0.0 : -0.0;
                    }
            }
            b->Init(48000);
            b->State = a->State;
            for (int frames : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 17, 1, 0, 64, 65}) {
                INFO(seed, frames);
                const size_t size = size_t(std::max(1, frames));
                std::vector<double> input(size), ao(size * a->Outputs()), bo(ao.size());
                for (int k = 0; k < frames; ++k) input[k] = k % 2 ? -0.25 * k : 0.25 * k;
                const double *ip[] = {input.data()};
                std::vector<double *> ap, bp;
                for (int c = 0; c < a->Outputs(); ++c) {
                    ap.push_back(ao.data() + size_t(c) * size);
                    bp.push_back(bo.data() + size_t(c) * size);
                }
                a->Compute(frames, ip, ap.data());
                b->Compute(frames, ip, bp.data());
                for (size_t k = 0; k < ao.size(); ++k) CHECK(Same(ao[k], bo[k]));
                for (size_t k = 0; k < a->State.size(); ++k) CHECK(Same(a->State[k].D, b->State[k].D));
            }
        }
    }
}

TEST_CASE("native shared state registers preserve intervening reads and live values") {
    enum { LateRead = 1, LateUse = 2, RepeatedStore = 4, MathCall = 8, RepeatedDefinition = 16 };
    for (Nature type : {Nature::Int, Nature::Real})
        for (uint32_t first = 0; first < 3; ++first)
            for (uint32_t mode = 0; mode < 32; ++mode) {
                INFO(int(type), first, mode);
                Plan plan;
                plan.Inputs = 1;
                plan.Outputs = 3;
                plan.Fields.resize(3);
                for (auto &field : plan.Fields) field.Nature = type;
                const auto emit = [&](Op op, std::initializer_list<Reg> args, uint32_t imm = 0, uint8_t form = 0) {
                    const Reg dst = op == Op::StoreField || op == Op::Output ? NoReg : plan.Regs++;
                    const Nature nature = op == Op::Input || op == Op::Extended ? Nature::Real : type;
                    plan.Band(Band::Sample).push_back({uint8_t(op), form, nature, dst, imm, 0, uint32_t(plan.Operands.size()), uint32_t(args.size())});
                    plan.Operands.insert(plan.Operands.end(), args);
                    return dst;
                };
                Reg input = emit(Op::Input, {});
                if (type == Nature::Int) input = emit(Op::IntCast, {input});
                const Reg earlier = mode & RepeatedDefinition ? emit(Op::BinOp, {input, input}, 0, uint8_t(BinOpCode::Sub)) : NoReg;
                const Reg prior = emit(Op::LoadField, {}, first);
                Reg old = NoReg;
                if (!(mode & LateRead)) old = emit(Op::LoadField, {}, (first + 1) % 3);
                Reg value = emit(Op::BinOp, {prior, input}, 0, uint8_t(BinOpCode::Add));
                if (earlier != NoReg) plan.Band(Band::Sample).back().Dst = value = earlier;
                if (mode & RepeatedStore) emit(Op::StoreField, {input}, first);
                emit(Op::StoreField, {value}, first);
                if (mode & LateRead) old = emit(Op::LoadField, {}, (first + 1) % 3);
                if (!(mode & LateUse)) emit(Op::Output, {old});
                const Reg result = mode & MathCall ? emit(Op::Extended, {value}, 0, uint8_t(Ext::Sin)) : value;
                emit(Op::StoreField, {value}, (first + 1) % 3);
                emit(Op::StoreField, {value}, (first + 2) % 3);
                if (mode & LateUse) emit(Op::Output, {old});
                emit(Op::Output, {result}, 1);
                emit(Op::Output, {prior}, 2);
                auto a = MakeExecutor<Interp>(plan, {}), b = MakeExecutor<Native>(plan, {});
                a->Init(48000);
                b->Init(48000);
                for (uint32_t k = 0; k < 3; ++k) {
                    if (type == Nature::Int) a->State[k].I = INT32_MAX - int32_t(k);
                    else a->State[k].D = 0.125 * (k + 1);
                }
                b->State = a->State;
                const double inputData[] = {1, -0.0, -3, 0.25, 7, -2, 0};
                const double *in[] = {inputData};
                double ao[3][7]{}, bo[3][7]{};
                double *ap[] = {ao[0], ao[1], ao[2]}, *bp[] = {bo[0], bo[1], bo[2]};
                for (int frames : {0, 1, 2, 7, 0, 1}) {
                    a->Compute(frames, in, ap);
                    b->Compute(frames, in, bp);
                    for (int c = 0; c < 3; ++c)
                        for (int k = 0; k < frames; ++k) CHECK(Same(ao[c][k], bo[c][k]));
                    for (size_t k = 0; k < a->State.size(); ++k) CHECK(Same(a->State[k].D, b->State[k].D));
                }
            }
}

TEST_CASE("native channel loops preserve nullable and aliased buffers") {
    enum { Separate, Shared, Offset };
    enum { MissingInputs = 1, MissingOutputs = 2, NullInputs = 4, NullOutputs = 8 };
    for (const char *source :
         {"process=0.5;", "process=_*0.5;", "process=sin(_*3)*0.5;", "process(x,y)=x+y;", "process=+~_;", "process(x,y)=sin(x),cos(y)+x';",
          "process(x,y)=control(x',int(y)>0),y+x;", "process=par(i,5, _ : +(i) : mem);", "process(x)=select2(1',x,x+1);",
          "process(x)=x*fvariable(int count, \"math.h\");", "process=_;"}) {
        INFO(source);
        Fixture f(source);
        if (std::string_view(source) == "process=_;") {
            auto &code = f.Plan.Band(Band::Sample);
            Instr input = *std::ranges::find_if(code, [](const Instr &i) { return Op(i.Op) == Op::Input; });
            Instr output = *std::ranges::find_if(code, [](const Instr &i) { return Op(i.Op) == Op::Output; });
            input.Dst = f.Plan.Regs++;
            output.Args = uint32_t(f.Plan.Operands.size());
            f.Plan.Operands.push_back(input.Dst);
            code.insert(code.end(), {input, output});
        }
        auto a = MakeExecutor<Interp>(f.Plan, f.Ui), b = MakeExecutor<Native>(f.Plan, f.Ui);
        const int channels = std::max(a->Inputs(), a->Outputs());
        for (int alias : {Separate, Shared, Offset}) {
            a->Init(48000);
            b->Init(48000);
            for (int mode = 0; mode < 16; ++mode)
                for (int frames : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 1, 0}) {
                    INFO(alias, mode, frames);
                    std::vector<std::vector<double>> av(2 * channels, std::vector<double>(68)), bv;
                    for (size_t c = 0; c < av.size(); ++c)
                        for (size_t k = 0; k < av[c].size(); ++k) av[c][k] = 0.125 * double(int(k % 7) - 3) + 0.0625 * c;
                    bv = av;
                    std::vector<const double *> ai, bi;
                    std::vector<double *> ao, bo;
                    for (int c = 0; c < a->Inputs(); ++c) {
                        const int index = alias ? (c + 1) % channels : c;
                        const bool missing = (mode & MissingInputs) && c % 2 == 0;
                        ai.push_back(missing ? nullptr : av[index].data() + 1);
                        bi.push_back(missing ? nullptr : bv[index].data() + 1);
                    }
                    for (int c = 0; c < a->Outputs(); ++c) {
                        const int index = (alias ? 0 : channels) + c;
                        const bool missing = (mode & MissingOutputs) && c % 2 == 0;
                        ao.push_back(missing ? nullptr : av[index].data() + (alias == Offset ? 2 : 1));
                        bo.push_back(missing ? nullptr : bv[index].data() + (alias == Offset ? 2 : 1));
                    }
                    const auto inputs = bi;
                    const auto outputs = bo;
                    a->Compute(frames, mode & NullInputs ? nullptr : ai.data(), mode & NullOutputs ? nullptr : ao.data());
                    b->Compute(frames, mode & NullInputs ? nullptr : bi.data(), mode & NullOutputs ? nullptr : bo.data());
                    CHECK(bi == inputs);
                    CHECK(bo == outputs);
                    for (size_t c = 0; c < av.size(); ++c)
                        for (size_t k = 0; k < av[c].size(); ++k) CHECK(Same(av[c][k], bv[c][k]));
                    for (size_t k = 0; k < a->State.size(); ++k) CHECK(Same(a->State[k].D, b->State[k].D));
                }
        }
    }
}

TEST_CASE("native repeated sample bodies preserve guarded output") {
    for (uint32_t condition : {0u, 1u}) {
        Fixture f("process=1.25;");
        const Reg cond = f.Plan.Regs++;
        f.Plan.Band(Band::Init).push_back({uint8_t(Op::ConstInt), 0, Nature::Int, cond, condition});
        Instr guard{uint8_t(Op::GuardBegin), 0, Nature::Int, NoReg, 0, 0, uint32_t(f.Plan.Operands.size()), 1};
        f.Plan.Operands.push_back(cond);
        auto &sample = f.Plan.Band(Band::Sample);
        sample.insert(sample.begin(), guard);
        sample.push_back({uint8_t(Op::GuardEnd)});
        auto a = MakeExecutor<Interp>(f.Plan, f.Ui), b = MakeExecutor<Native>(f.Plan, f.Ui);
        a->Init(48000);
        b->Init(48000);
        for (int frames : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 31, 32, 33, 63, 64, 65}) {
            std::array<double, 67> av, bv;
            av.fill(-7);
            bv = av;
            double *ap[] = {av.data() + 1}, *bp[] = {bv.data() + 1};
            a->Compute(frames, nullptr, ap);
            b->Compute(frames, nullptr, bp);
            CHECK(av == bv);
        }
    }
}

TEST_CASE("native constant integer arithmetic preserves signed results and state") {
    std::vector<uint32_t> constants{0, 1, UINT32_MAX, 3, 4095, 4097, 0x00fff000, 0x01000000, 0x12345678, 0x80000001};
    for (uint32_t shift = 1; shift < 32; ++shift) {
        constants.push_back(1u << shift);
        constants.push_back(0u - (1u << shift));
    }
    std::vector<double> input{INT32_MIN, INT32_MIN + 1, INT32_MAX, -4097, -4096, -33, -17, -3, -1, 0, 1, 3, 17, 33, 4095, 4096};
    uint32_t random = 42;
    for (int k = 0; k < 32; ++k) {
        random = random * 1664525u + 1013904223u;
        input.push_back(std::bit_cast<int32_t>(random));
    }
    const auto check = [&](uint32_t constant, bool logical) {
        for (int mode = 0; mode < (logical ? 1 : 4); ++mode) {
            INFO(constant, mode);
            Plan plan;
            plan.Inputs = 1;
            const auto emit = [&](Op op, std::initializer_list<Reg> args, uint32_t imm = 0, uint8_t form = 0) {
                const Reg dst = op == Op::Output || op == Op::StoreField ? NoReg : plan.Regs++;
                const Nature nature = op == Op::Input ? Nature::Real : Nature::Int;
                plan.Band(op == Op::ConstInt ? Band::Init : Band::Sample)
                    .push_back({uint8_t(op), form, nature, dst, imm, 0, uint32_t(plan.Operands.size()), uint32_t(args.size())});
                plan.Operands.insert(plan.Operands.end(), args);
                return dst;
            };
            Reg source = emit(Op::IntCast, {emit(Op::Input, {})});
            if (mode == 1) source = emit(Op::Extended, {source}, 0, uint8_t(Ext::Abs));
            if (mode == 2) source = emit(Op::BinOp, {source, emit(Op::ConstInt, {}, INT32_MAX)}, 0, uint8_t(BinOpCode::AND));
            const Reg c = emit(Op::ConstInt, {}, constant);
            for (uint8_t op = logical ? uint8_t(BinOpCode::AND) : 0; op < uint8_t(BinOpCode::Count_); ++op)
                for (bool reverse : {false, true}) {
                    const uint32_t channel = plan.Outputs++;
                    Reg x = source;
                    if (mode == 3) {
                        plan.Fields.emplace_back().Nature = Nature::Int;
                        x = emit(Op::LoadField, {}, channel);
                    }
                    // Place division and remainder in the first cached state slots.
                    const uint8_t form = mode == 3 ? (op + uint8_t(BinOpCode::Div)) % uint8_t(BinOpCode::Count_) : op;
                    const Reg result = emit(Op::BinOp, {reverse ? c : x, reverse ? x : c}, 0, form);
                    if (mode == 3) emit(Op::StoreField, {result}, channel);
                    emit(Op::Output, {result}, channel);
                }
            auto a = MakeExecutor<Interp>(plan, {}), b = MakeExecutor<Native>(plan, {});
            a->Init(48000);
            b->Init(48000);
            for (size_t k = 0; k < a->State.size(); ++k) a->State[k].I = int32_t(input[k % input.size()]);
            b->State = a->State;
            std::vector<std::vector<double>> av(plan.Outputs, std::vector<double>(input.size())), bv = av;
            std::vector<double *> ap, bp;
            for (int k = 0; k < plan.Outputs; ++k) {
                ap.push_back(av[k].data());
                bp.push_back(bv[k].data());
            }
            const double *ip[] = {input.data()};
            for (int frames : {0, 1, int(input.size()), 2}) {
                a->Compute(frames, ip, ap.data());
                b->Compute(frames, ip, bp.data());
                CHECK(av == bv);
                for (size_t k = 0; k < a->State.size(); ++k) CHECK(a->State[k].I == b->State[k].I);
            }
        }
    };
    for (uint32_t constant : constants) check(constant, false);
    // Cover every 32-bit ARM64 logical immediate pattern.
    for (uint32_t width = 2; width <= 32; width *= 2)
        for (uint32_t ones = 1; ones < width; ++ones) {
            uint32_t mask = (1u << ones) - 1;
            for (uint32_t n = width; n < 32; n *= 2) mask |= mask << n;
            for (uint32_t rotation = 0; rotation < width; ++rotation) check(std::rotr(mask, int(rotation)), true);
        }
}

TEST_CASE("native masked offsets preserve wrapping and intervening definitions") {
    for (uint32_t mask : {1u, 7u, 2047u, uint32_t(INT32_MAX)})
        for (BinOpCode op : {BinOpCode::Add, BinOpCode::Sub})
            for (int mode = 0; mode < 6; ++mode) {
                INFO(mask, int(op), mode);
                Plan plan;
                plan.Inputs = 1;
                const auto emit = [&](Op op, std::initializer_list<Reg> args, uint32_t imm = 0, BinOpCode form = BinOpCode::Add) {
                    const Reg dst = op == Op::Output || op == Op::GuardBegin || op == Op::GuardEnd ? NoReg : plan.Regs++;
                    plan.Band(op == Op::ConstInt ? Band::Init : Band::Sample)
                        .push_back(
                            {uint8_t(op), uint8_t(form), op == Op::Input ? Nature::Real : Nature::Int, dst, imm, 0, uint32_t(plan.Operands.size()),
                             uint32_t(args.size())}
                        );
                    plan.Operands.insert(plan.Operands.end(), args);
                    return dst;
                };
                const Reg input = emit(Op::Input, {}), source = emit(Op::IntCast, {input});
                const Reg m = emit(Op::ConstInt, {}, mask), amount = emit(Op::ConstInt, {}, (mask / 2 + 1) - (mode == 5));
                const Reg zero = emit(Op::ConstInt, {});
                if (mode == 4) emit(Op::GuardBegin, {zero});
                const Reg masked = emit(Op::BinOp, {source, m}, 0, BinOpCode::AND);
                if (mode == 4) emit(Op::GuardEnd, {});
                if (mode == 2 || mode == 3) {
                    emit(Op::IntCast, {zero});
                    plan.Band(Band::Sample).back().Dst = mode == 2 ? source : masked;
                }
                const Reg offset = emit(Op::BinOp, {source, amount}, 0, op);
                const Reg result = emit(Op::BinOp, {offset, m}, 0, BinOpCode::AND);
                emit(Op::Output, {result}, plan.Outputs++);
                emit(Op::Output, {masked}, plan.Outputs++);
                if (mode == 1) emit(Op::Output, {offset}, plan.Outputs++);
                auto a = MakeExecutor<Interp>(plan, {}), b = MakeExecutor<Native>(plan, {});
                a->Init(48000);
                b->Init(48000);
                const double samples[] = {INT32_MIN, INT32_MIN + 1, -2049, -2048, -1025, -1, 0, 1, 1023, 1024, 2047, 2048, INT32_MAX};
                const double *ip[] = {samples};
                double av[3][std::size(samples)]{}, bv[3][std::size(samples)]{};
                double *ap[] = {av[0], av[1], av[2]}, *bp[] = {bv[0], bv[1], bv[2]};
                for (int frames : {0, 1, int(std::size(samples)), 3}) {
                    a->Compute(frames, ip, ap);
                    b->Compute(frames, ip, bp);
                    for (int c = 0; c < plan.Outputs; ++c)
                        for (int k = 0; k < frames; ++k) CHECK(Same(av[c][k], bv[c][k]));
                }
            }
}

TEST_CASE("native constant specialization preserves guarded and repeated definitions") {
    for (bool repeated : {false, true}) {
        Plan plan;
        plan.Inputs = 2;
        plan.Outputs = 1;
        plan.Regs = 6;
        plan.Operands = {0, 1, 2, 3, 4, 5};
        if (repeated) plan.Band(Band::Init).push_back({uint8_t(Op::ConstInt), 0, Nature::Int, 3, uint32_t(-8)});
        plan.Band(Band::Sample) = {
            {uint8_t(Op::Input), 0, Nature::Real, 0},
            {uint8_t(Op::Input), 0, Nature::Real, 1, 1},
            {uint8_t(Op::IntCast), 0, Nature::Int, 2, 0, 0, 0, 1},
            {uint8_t(Op::IntCast), 0, Nature::Int, 5, 0, 0, 1, 1},
            {uint8_t(Op::GuardBegin), 0, Nature::Int, NoReg, 0, 0, 5, 1},
            {uint8_t(Op::ConstInt), 0, Nature::Int, 3, 4},
            {uint8_t(Op::GuardEnd)},
            {uint8_t(Op::BinOp), uint8_t(BinOpCode::Rem), Nature::Int, 4, 0, 0, 2, 2},
            {uint8_t(Op::Output), 0, Nature::Int, NoReg, 0, 0, 4, 1}
        };
        auto a = MakeExecutor<Interp>(plan, {}), b = MakeExecutor<Native>(plan, {});
        a->Init(48000);
        b->Init(48000);
        const double input[] = {-5, INT32_MIN, 17, -5}, condition[] = {0, 0, 1, 0};
        const double *ip[] = {input, condition};
        double av[4], bv[4];
        double *ap[] = {av}, *bp[] = {bv};
        a->Compute(4, ip, ap);
        b->Compute(4, ip, bp);
        CHECK(av[0] == -5);
        for (int k = 0; k < 4; ++k) CHECK(av[k] == bv[k]);
    }
}

TEST_CASE("native table indexing preserves bounds and integer wrapping") {
    for (const char *index :
         {"int(x)",
          "int(x)&15",
          "int(x)&31",
          "int(x)&-2",
          "max(0,min(15,int(x)))",
          "min(15,int(x))",
          "max(0,int(x))",
          "(int(x)&15)+2147483647",
          "abs(int(x))%16",
          "abs(int(x))%-16",
          "abs(int(x))%3",
          "int(x)%16",
          "int(x)%-16",
          "(int(x)&255)/16",
          "abs(int(x))/134217728",
          "int(x)/-1",
          "int(x)/-2147483648",
          "control(abs(int(x)),int(x)>0)%16",
          "int(x)<0",
          "select2(int(x),int(x)&15,int(x)&7)",
          "select3(int(x),int(x)&15,int(x)&7,-1)",
          "control(int(x)&15,int(x)>0)"})
        for (const char *body :
             {"(waveform{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16},index(x):rdtable)", "rwtable(16,0.0,index(x),x,index(x))",
              "rwtable(16,0,index(x),int(x),index(x))"}) {
            const std::string source = std::format("index(x)={}; process(x)={};", index, body);
            INFO(source);
            Fixture f(source.c_str());
            auto a = MakeExecutor<Interp>(f.Plan, f.Ui), b = MakeExecutor<Native>(f.Plan, f.Ui);
            const double input[] = {0, -1, 1, 2.75, -16, 15, 16, 31, 32, 2147483647., -2147483648., 2147483648., -2147483649., INFINITY, -INFINITY, NAN};
            for (int repeat = 0; repeat < 2; ++repeat) {
                a->Init(48000);
                b->Init(48000);
                for (double value : input) {
                    double ao, bo;
                    const double *ip[] = {&value};
                    double *ap[] = {&ao}, *bp[] = {&bo};
                    a->Compute(1, ip, ap);
                    b->Compute(1, ip, bp);
                    CHECK(Same(ao, bo));
                    for (size_t k = 0; k < a->State.size(); ++k) CHECK(Same(a->State[k].D, b->State[k].D));
                }
            }
        }
}

TEST_CASE("native field bounds normalize zero extents") {
    Fixture f("process(x)=rwtable(2,0.0,int(x),x,0);");
    REQUIRE(f.Plan.Fields.size() == 1);
    f.Plan.Fields[0].Extent = 0;
    // The second field detects writes past the normalized slot.
    f.Plan.Fields.emplace_back();
    auto a = MakeExecutor<Interp>(f.Plan, f.Ui), b = MakeExecutor<Native>(f.Plan, f.Ui);
    a->Init(48000);
    b->Init(48000);
    double input = 1, ao, bo;
    const double *ip[] = {&input};
    double *ap[] = {&ao}, *bp[] = {&bo};
    a->Compute(1, ip, ap);
    b->Compute(1, ip, bp);
    CHECK(ao == 1);
    CHECK(bo == ao);
    CHECK(b->State[1].D == 0);
}

TEST_CASE("native field addressing preserves tables and delays across offset boundaries") {
    for (bool integer : {false, true})
        for (uint32_t offset : {496u, 504u, 512u, 4088u, 4096u, 16376u, 16384u, 32760u, 32768u, 0xfffff8u, 0x1000000u, 0x1000008u}) {
            INFO(integer, offset);
            const std::string source = std::format(
                "process(x)=rwtable(4,{},int(x),{},int(x)),rwtable(4,{},int(x),{},3),({})',({}+1)';", integer ? "0" : "0.0", integer ? "int(x)" : "x",
                integer ? "0" : "0.0", integer ? "int(x)" : "x", integer ? "int(x)" : "x", integer ? "int(x)" : "x"
            );
            Fixture f(source.c_str());
            Field padding;
            padding.Extent = offset / 8;
            f.Plan.Fields.insert(f.Plan.Fields.begin(), padding);
            f.Plan.Fields.emplace_back();
            for (auto &band : f.Plan.Bands)
                for (auto &i : band)
                    if (Op(i.Op) == Op::LoadField || Op(i.Op) == Op::StoreField) ++i.Imm;
            auto a = MakeExecutor<Interp>(f.Plan, f.Ui), b = MakeExecutor<Native>(f.Plan, f.Ui);
            a->Init(48000);
            b->Init(48000);
            const uint32_t start = offset / 8;
            for (auto *dsp : {a.get(), b.get()}) dsp->State[start - 1].D = dsp->State.back().D = 19;
            double input[] = {-1, 0, 1.25, 2.75, 3, 4, 65536};
            const double *in[] = {input};
            double ao[4][7]{}, bo[4][7]{};
            double *ap[] = {ao[0], ao[1], ao[2], ao[3]}, *bp[] = {bo[0], bo[1], bo[2], bo[3]};
            for (int frames : {0, 1, 7, 0}) {
                a->Compute(frames, in, ap);
                b->Compute(frames, in, bp);
                for (int c = 0; c < 4; ++c)
                    for (int k = 0; k < frames; ++k) CHECK(Same(ao[c][k], bo[c][k]));
                for (size_t k = start - 1; k < a->State.size(); ++k) CHECK(Same(a->State[k].D, b->State[k].D));
            }
        }
}

TEST_CASE("native indexed stores preserve spilled integer conversions") {
    Plan plan;
    plan.Inputs = 1;
    plan.Regs = 34;
    plan.Fields.resize(3);
    plan.Fields[0].Extent = 5000;
    plan.Fields[1].Extent = 16;
    plan.Fields[2].Extent = 32;
    plan.Operands = {0};
    auto &sample = plan.Band(Band::Sample);
    sample.push_back({uint8_t(Op::Input), 0, Nature::Real, 0});
    sample.push_back({uint8_t(Op::IntCast), 0, Nature::Int, 1, 0, 0, 0, 1});
    for (Reg r = 2; r < plan.Regs; ++r) {
        plan.Band(Band::Init).push_back({uint8_t(Op::ConstInt), 0, Nature::Int, r, r - 2});
        sample.push_back({uint8_t(Op::StoreField), 0, Nature::Real, NoReg, 1, 0, uint32_t(plan.Operands.size()), 2});
        plan.Operands.insert(plan.Operands.end(), {1, r});
    }
    auto a = MakeExecutor<Interp>(plan, {}), b = MakeExecutor<Native>(plan, {});
    a->Init(48000);
    b->Init(48000);
    for (double input : {-1., 0., 7., 15., 16., 2147483647., -2147483648., double(INFINITY), double(NAN)}) {
        INFO(input);
        const double *ip[] = {&input};
        a->Compute(1, ip, nullptr);
        b->Compute(1, ip, nullptr);
        for (size_t k = 0; k < a->State.size(); ++k) CHECK(Same(a->State[k].D, b->State[k].D));
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
