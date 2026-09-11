#include "signal/Differentiate.h"
#include "conformance/Sweep.h"
#include "runtime/Executors.h"
#include "signal/Promote.h"
#include "signal/Simplify.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using namespace faustlens;
using namespace faustlens::test;

namespace {

struct DerivativeFixture {
    Program Source;
    Plan Primal, Augmented;
    UiNode Ui;
    std::vector<std::string> Labels;
    std::vector<SigId> Roots;
    DifferentiateResult Derivative;

    explicit DerivativeFixture(std::string source) : Source("/differentiate.dsp", std::move(source)) {
        REQUIRE(Source.Ok);
        SetRoots(Source.Outs);
        Ui = Source.Ui("differentiate");
        for (const auto &item : Source.Prop.Ui)
            if (item.Kind == UiKind::HSlider || item.Kind == UiKind::VSlider || item.Kind == UiKind::NumEntry) Labels.emplace_back(Source.Sigs.Str(item.Label));
        std::ranges::sort(Labels);
        Labels.erase(std::unique(Labels.begin(), Labels.end()), Labels.end());
    }

    Plan Lower() {
        auto plan = Source.Lower();
        REQUIRE_MESSAGE(plan, (plan ? "" : plan.error()));
        return std::move(*plan);
    }

    void SetRoots(std::vector<SigId> roots) {
        Roots = std::move(roots);
        Source.Outs = Roots;
        Primal = Lower();
    }

    void Transform(bool explicit_directions = false, size_t count = 0, std::vector<double> directions = {}) {
        DifferentiateRequest request{Labels, explicit_directions, count, std::move(directions)};
        std::vector<uint64_t> hashes;
        for (SigId root : Roots) hashes.push_back(Source.Sigs.ContentHash(root));
        Derivative = Differentiate(Source.Sigs, Roots, request, Source.Prop.Ui);
        for (size_t i = 0; i < Roots.size(); ++i) CHECK(Source.Sigs.ContentHash(Roots[i]) == hashes[i]);
        REQUIRE_MESSAGE(Derivative.Ok(), Derivative.Error);
        CHECK(Source.Outs == Roots);
        Source.Outs.insert(Source.Outs.end(), Derivative.Tangents.begin(), Derivative.Tangents.end());
        Augmented = Lower();
        const auto natures = InferNatures(Source.Sigs);
        for (SigId tangent : Derivative.Tangents) CHECK(natures[tangent] == Nature::Real);
    }
};

using Audio = std::vector<std::vector<double>>;

Audio Render(Instance &dsp, const DerivativeFixture &f, std::span<const double> controls, std::span<const double> input, size_t block = 0) {
    REQUIRE(controls.size() == f.Labels.size());
    dsp.Init(48000);
    for (size_t j = 0; j < controls.size(); ++j) {
        const auto label = std::ranges::find(dsp.Plan.Labels, f.Labels[j]);
        REQUIRE(label != dsp.Plan.Labels.end());
        dsp.SetControl(uint32_t(label - dsp.Plan.Labels.begin()), controls[j]);
    }
    Audio output(size_t(dsp.Outputs()), std::vector<double>(input.size()));
    if (!block) block = input.size();
    for (size_t offset = 0; offset < input.size(); offset += block) {
        const double *inputs[] = {input.data() + offset};
        std::vector<double *> outputs;
        for (auto &channel : output) outputs.push_back(channel.data() + offset);
        dsp.Compute(int32_t(std::min(block, input.size() - offset)), inputs, outputs.data());
    }
    return output;
}

template<class Backend>
Audio Render(const Plan &plan, const DerivativeFixture &f, std::span<const double> controls, std::span<const double> input, size_t block = 0) {
    auto dsp = MakeExecutor<Backend>(plan, f.Ui);
    return Render(*dsp, f, controls, input, block);
}

template<class Backend> Audio Sample(const DerivativeFixture &f, std::vector<double> controls) {
    const std::array input{0.0};
    return Render<Backend>(f.Augmented, f, controls, input);
}

std::vector<double> Excitation(size_t count = 129) {
    std::vector<double> x(count);
    for (size_t k = 0; k < count; ++k) x[k] = 0.3 * std::sin(double(k) * 0.27) + 0.2 * std::cos(double(k) * 0.13);
    x[0] += 1;
    return x;
}

void Near(double actual, double expected, double tolerance = 2e-11) {
    INFO("actual=", actual, " expected=", expected);
    CHECK(std::isfinite(actual));
    CHECK(std::abs(actual - expected) <= tolerance * (1 + std::abs(expected)));
}

template<class Backend> Audio FiniteDifferences(DerivativeFixture &f, std::vector<double> controls) {
    const auto input = Excitation();
    auto augmented = Render<Backend>(f.Augmented, f, controls, input);
    const auto primal = Render<Backend>(f.Primal, f, controls, input);
    const size_t outputs = size_t(f.Primal.Outputs), columns = controls.size();
    REQUIRE(augmented.size() == outputs * (1 + columns));
    for (size_t k = 0; k < outputs; ++k)
        for (size_t n = 0; n < input.size(); ++n) CHECK(BitsOf(augmented[k][n]) == BitsOf(primal[k][n]));
    // Require agreement at all three step sizes.
    for (double scale : {1e-4, 1e-5, 1e-6})
        for (size_t j = 0; j < columns; ++j) {
            INFO("column=", j, " step=", scale);
            const double h = scale * std::max(1.0, std::abs(controls[j]));
            auto minus = controls, plus = controls;
            minus[j] -= h;
            plus[j] += h;
            const auto lo = Render<Backend>(f.Primal, f, minus, input), hi = Render<Backend>(f.Primal, f, plus, input);
            for (size_t k = 0; k < outputs; ++k)
                for (size_t n = 0; n < input.size(); ++n) Near(augmented[outputs + k * columns + j][n], (hi[k][n] - lo[k][n]) / (2 * h), 2e-6);
        }
    return augmented;
}

constexpr auto SingleControl = "a=hslider(\"a\",0.4,-10,10,0.01); ";

} // namespace

TEST_CASE_TEMPLATE("differentiate arithmetic preserves primals and physical control columns", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f("a=hslider(\"a\",0.4,-10,10,1); b=vslider(\"b\",1.3,-10,10,1); process(x)=a*x+b,a/b,sin(a*b);");
    REQUIRE(f.Labels.size() == 2);
    f.Transform();
    CHECK(f.Derivative.DirectionCount == 2);
    CHECK(f.Derivative.Tangents.size() == 6);
    const std::vector<double> values{0.4, 1.3}, input{0.2, -0.7, 1.1};
    const auto y = Render<Backend>(f.Augmented, f, values, input);
    for (size_t n = 0; n < input.size(); ++n) {
        Near(y[3][n], input[n]);
        Near(y[4][n], 1);
        Near(y[5][n], 1 / values[1]);
        Near(y[6][n], -values[0] / (values[1] * values[1]));
        Near(y[7][n], values[1] * std::cos(values[0] * values[1]));
        Near(y[8][n], values[0] * std::cos(values[0] * values[1]));
    }
    FiniteDifferences<Backend>(f, values);
}

TEST_CASE_TEMPLATE("differentiate extended rules agree with analytic derivatives", Backend, FAUSTLENS_TEST_EXECUTORS) {
    struct Rule {
        const char *Expression;
        double Value, Expected;
    };
    const Rule rules[] = {
        {"assertbounds(0.0,1.0,a*0.25)", 0.4, 0.25},
        {"abs(a)", -0.4, -1},
        {"abs(a)", 0, 1},
        {"acos(a)", 0.4, -1 / std::sqrt(0.84)},
        {"asin(a)", 0.4, 1 / std::sqrt(0.84)},
        {"atan(a)", 0.4, 1 / 1.16},
        {"cos(a)", 0.4, -std::sin(0.4)},
        {"exp(a)", 0.4, std::exp(0.4)},
        {"log(a)", 0.4, 2.5},
        {"log10(a)", 0.4, 1 / (0.4 * std::log(10.0))},
        {"sin(a)", 0.4, std::cos(0.4)},
        {"sqrt(a)", 0.4, 1 / (2 * std::sqrt(0.4))},
        {"tan(a)", 0.4, 1 / std::pow(std::cos(0.4), 2)},
        {"a^3", -2, 12},
        {"a^3", 0, 0},
        {"a^1", 0, 1},
        {"2.0^a", 0.4, std::pow(2.0, 0.4) * std::log(2.0)},
        {"max(a,0.3)", 0.4, 1},
        {"max(a,0.3)", 0.2, 0},
        {"min(a,0.3)", 0.4, 0},
        {"min(a,0.3)", 0.2, 1},
        {"atan2(a,0.7)", 0.4, 0.7 / (0.16 + 0.49)},
        {"fmod(a,0.7)", 1.8, 1},
        {"remainder(a,0.7)", 1.8, 1}
    };
    for (const auto &rule : rules) {
        INFO(std::string_view(rule.Expression), " value=", rule.Value);
        DerivativeFixture f(std::string(SingleControl) + "process=" + rule.Expression + ";");
        f.Transform();
        const auto y = Sample<Backend>(f, {rule.Value});
        Near(y[1][0], rule.Expected);
    }
}

TEST_CASE_TEMPLATE("differentiate recurrence carries warmup sensitivities across blocks and resets", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f(std::string(SingleControl) + "process=*(1-a):+~*(a);");
    f.Transform();
    const std::vector<double> values{0.4};
    const auto input = Excitation();
    const auto y = Render<Backend>(f.Augmented, f, values, input);
    double previous = 0, derivative = 0;
    for (size_t n = 0; n < input.size(); ++n) {
        const double next = -input[n] + previous + values[0] * derivative;
        previous = (1 - values[0]) * input[n] + values[0] * previous;
        derivative = next;
        Near(y[0][n], previous);
        Near(y[1][n], derivative);
    }
    for (size_t block : {size_t(1), size_t(7), size_t(64)}) CHECK(Render<Backend>(f.Augmented, f, values, input, block) == y);
    auto reused = MakeExecutor<Backend>(f.Augmented, f.Ui);
    CHECK(Render(*reused, f, values, input, 13) == y);
    const std::vector<double> other{0.7};
    Render(*reused, f, other, input, 5);
    CHECK(Render(*reused, f, values, input, 17) == y);
    FiniteDifferences<Backend>(f, values);
}

TEST_CASE_TEMPLATE("differentiate weighted directions are linear combinations of columns", Backend, FAUSTLENS_TEST_EXECUTORS) {
    const char *source = "a=hslider(\"a\",0.4,-10,10,0.01); b=hslider(\"b\",0.7,-10,10,0.01); process(x)=(x*a:+~*(b)),sin(a*b);";
    DerivativeFixture columns(source), weighted(source);
    columns.Transform();
    // v, w, v+w, and zero, stored by control then direction.
    weighted.Transform(true, 4, {2, -1, 1, 0, -3, 0.5, -2.5, 0});
    const std::vector<double> values{0.4, 0.7};
    const auto input = Excitation();
    const auto c = Render<Backend>(columns.Augmented, columns, values, input), w = Render<Backend>(weighted.Augmented, weighted, values, input);
    REQUIRE(w.size() == 10);
    for (size_t k = 0; k < 2; ++k)
        for (size_t n = 0; n < input.size(); ++n) {
            Near(w[2 + 4 * k][n], 2 * c[2 + 2 * k][n] - 3 * c[3 + 2 * k][n]);
            Near(w[3 + 4 * k][n], -c[2 + 2 * k][n] + 0.5 * c[3 + 2 * k][n]);
            Near(w[4 + 4 * k][n], w[2 + 4 * k][n] + w[3 + 4 * k][n]);
            CHECK(w[5 + 4 * k][n] == 0);
        }
}

TEST_CASE("differentiate zero directions preserve roots arena and plan hash") {
    DerivativeFixture f(std::string(SingleControl) + "process=*(a):+~*(0.7);");
    const size_t nodes = f.Source.Sigs.Size();
    const auto hash = Hash(f.Primal);
    f.Transform(true, 0);
    CHECK(f.Derivative.Tangents.empty());
    CHECK(f.Source.Sigs.Size() == nodes);
    CHECK(Hash(f.Augmented) == hash);
}

TEST_CASE("differentiate rejects malformed requests and classifies absent controls") {
    DerivativeFixture f(std::string(SingleControl) + "process=a;");
    DifferentiateRequest request{.Controls = {f.Labels[0], f.Labels[0]}};
    CHECK_FALSE(Differentiate(f.Source.Sigs, f.Roots, request).Ok());
    request.Controls = f.Labels;
    request.ExplicitDirections = true;
    request.DirectionCount = 2;
    request.Directions = {1};
    CHECK_FALSE(Differentiate(f.Source.Sigs, f.Roots, request).Ok());
    request.Directions = {1, std::numeric_limits<double>::infinity()};
    CHECK_FALSE(Differentiate(f.Source.Sigs, f.Roots, request).Ok());
    request.ExplicitDirections = false;
    request.DirectionCount = 0;
    request.Directions.clear();
    request.Controls = {"missing control"};
    const auto absent = Differentiate(f.Source.Sigs, f.Roots, request);
    REQUIRE(absent.Controls.size() == 1);
    CHECK_FALSE(absent.Controls[0].Present);
    CHECK_FALSE(absent.Diagnostics.empty());
}

TEST_CASE_TEMPLATE("differentiate state and addressing rules agree with finite differences", Backend, FAUSTLENS_TEST_EXECUTORS) {
    struct Case {
        const char *Name, *Body;
        std::vector<double> Controls = {0.4};
        bool Barrier = false, Active = false;
    };
    const Case cases[] = {
        {"unit delay", "process(x)=(a*x)';"},
        {"fixed delay", "process(x)=(a*x)@3;"},
        {"prefix", "process(x)=prefix(0.0,a*x);"},
        {"independent control", "process(x)=control((a*x)',x>0);"},
        {"normalized enable", "process(x)=enable((a*x)',x>0);"},
        {"coupled feedback", "process(x)=x,x:((+,+):(*(a),*(0.3)))~(_,_) : +;"},
        {"shared filter", "filter=+~*(a); process(x)=x:filter:filter;"},
        {"nested feedback", "process(x)=x:(+~*(a)):((+~*(0.3)):*(a));"},
        {"feedback and delay", "process(x)=(sin(x*a):+~*(0.7)),((x*a)@3);"},
        {"active write index", "process(x)=rwtable(4,0.125,int(a),x,0);", {0.4}, true},
        {"active read index", "process(x)=rwtable(4,0.125,0,x,int(a));", {0.4}, true},
        {"active control", "process(x)=control(a*x,a>0.2);", {0.4}, true},
        {"active enable", "process(x)=enable((a*x)',a>0.2);", {0.4}, true},
        {"guarded prefix", "process(x)=control(prefix(1.0,a*x),x>0);", {0.4}, false, true},
        {"distinct prefix initializers", "process(x)=control(prefix(1.0,a*x),x>0)+control(prefix(2.0,a*x),x<0);", {0.4}, false, true},
        {"distinct table initializers",
         "process(x)=control(rwtable(4,0.3,(int(abs(x)*10)+2)%4,a*x,0),x>0)+control(rwtable(4,0.7,(int(abs(x)*10)+2)%4,a*x,0),x<0);",
         {0.4},
         false,
         true},
        {"independent guarded histories", "b=hslider(\"b\",0.7,-10,10,0.01); process(x)=control((a*x)',x>0),control((b*x)',x<0);", {0.4, 0.7}}
    };
    for (const auto &c : cases) SUBCASE(c.Name) {
            DerivativeFixture f(std::string(SingleControl) + c.Body);
            f.Transform();
            if (c.Barrier) CHECK(std::ranges::any_of(f.Derivative.Diagnostics, [](const auto &d) { return d.Kind == DerivativeDiagnosticKind::Barrier; }));
            if (c.Active) REQUIRE(f.Derivative.Controls[0].Active);
            FiniteDifferences<Backend>(f, c.Controls);
        }
}

TEST_CASE_TEMPLATE("differentiate discrete paths retain barrier diagnostics", Backend, FAUSTLENS_TEST_EXECUTORS) {
    for (const char *expression : {"float(int(a))", "floor(a)", "select2(a>0,2*a,3*a)", "x@max(0,int(a))"}) {
        INFO(std::string_view(expression));
        DerivativeFixture f(std::string(SingleControl) + "process(x)=" + expression + ";");
        f.Transform();
        CHECK_FALSE(f.Derivative.Diagnostics.empty());
        const std::vector<double> values{0.4}, input{1, 2, 3};
        const auto y = Render<Backend>(f.Augmented, f, values, input);
        for (double v : y[1]) Near(v, std::string_view(expression).starts_with("select2") ? 3 : 0);
    }
}

TEST_CASE("differentiate active foreign calls report unsupported derivatives") {
    DerivativeFixture f(std::string(SingleControl) + "process=ffunction(float derivative_probe(float),\"math.h\",\"\")(a);");
    DifferentiateRequest request{.Controls = f.Labels};
    const auto result = Differentiate(f.Source.Sigs, f.Roots, request);
    CHECK_FALSE(result.Ok());
    CHECK_FALSE(result.Diagnostics.empty());
    CHECK(f.Source.Outs == f.Roots);
    CHECK(f.Source.Lower().has_value());
}

TEST_CASE_TEMPLATE("differentiate mutable table data has independent tangent storage", Backend, FAUSTLENS_TEST_EXECUTORS) {
    for (const char *body : {"process(x)=rwtable(4,0.125,int(abs(x)*10)%4,a*x,0);", "process(x)=rwtable(4,0.125,0,a*x,0);"}) {
        INFO(std::string_view(body));
        DerivativeFixture f(std::string(SingleControl) + body);
        f.Transform();
        REQUIRE(f.Derivative.Controls[0].Active);
        const auto fresh = FiniteDifferences<Backend>(f, {0.4});
        const auto input = Excitation();
        const std::vector<double> values{0.4}, other{0.7};
        auto reused = MakeExecutor<Backend>(f.Augmented, f.Ui);
        Render(*reused, f, other, input);
        CHECK(Render(*reused, f, values, input, 7) == fresh);
    }
}

TEST_CASE("differentiate initialization dependence is rejected even through a discrete barrier") {
    for (const char *body : {"process=prefix(a,1.0);", "process=rdtable(4,a,0);", "process=prefix(float(int(a)),1.0);"}) {
        INFO(std::string_view(body));
        Program source("/init.dsp", std::string(SingleControl) + body);
        REQUIRE(source.Ok);
        DifferentiateRequest request;
        REQUIRE_FALSE(source.Prop.Ui.empty());
        request.Controls = {std::string(source.Sigs.Str(source.Prop.Ui[0].Label))};
        const auto result = Differentiate(source.Sigs, source.Outs, request, source.Prop.Ui);
        CHECK_FALSE(result.Ok());
        CHECK(std::ranges::any_of(result.Diagnostics, [](const auto &d) { return d.Kind == DerivativeDiagnosticKind::Unsupported; }));
    }
}

TEST_CASE_TEMPLATE("differentiate shared logical controls and bargraphs preserve control behavior", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f("a=hslider(\"a\",0.4,-10,10,0.01); b=vslider(\"a\",0.4,-10,10,0.01); process=(a+b):hbargraph(\"meter\",-20,20);");
    REQUIRE(f.Labels.size() == 1);
    f.Transform();
    const std::vector<double> values{0.4}, input{0};
    auto dsp = MakeExecutor<Backend>(f.Augmented, f.Ui);
    const auto y = Render(*dsp, f, values, input);
    Near(y[0][0], 0.8);
    Near(y[1][0], 2);
    const auto meters = dsp->ControlsOfKind(UiKind::HBargraph);
    REQUIRE(meters.size() == 1);
    Near(dsp->Control(meters[0]), 0.8);
}

TEST_CASE("differentiate cancellation keeps per-control structural activity") {
    DerivativeFixture f("a=hslider(\"a\",0.4,-10,10,0.01); b=hslider(\"b\",0.4,-10,10,0.01); process=a-b;");
    f.Transform(true, 1, {1, 1});
    REQUIRE(f.Derivative.Controls.size() == 2);
    for (const auto &control : f.Derivative.Controls) {
        CHECK(control.Present);
        CHECK(control.Structural);
        CHECK(control.Active);
    }
}

TEST_CASE_TEMPLATE("differentiate singular mathematical derivatives stay nonfinite", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f(std::string(SingleControl) + "process=sqrt(a);");
    f.Transform();
    const auto y = Sample<Backend>(f, {0});
    CHECK(y[0][0] == 0);
    CHECK(std::isinf(y[1][0]));
}

TEST_CASE_TEMPLATE("differentiate zero weighted directions avoid singular derivative construction", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f(std::string(SingleControl) + "process=sqrt(a);");
    f.Transform(true, 1, {0});
    const auto y = Sample<Backend>(f, {0});
    CHECK(y[0][0] == 0);
    CHECK(y[1][0] == 0);
}

TEST_CASE_TEMPLATE("differentiate augmented execution preserves corresponding primal state", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f(std::string(SingleControl) + "process(x)=(a*x:+~*(0.7)),rwtable(4,0.125,int(abs(x)*10)%4,a*x,0),(a*x)@3;");
    f.Transform();
    auto plain = MakeExecutor<Backend>(f.Primal, f.Ui), augmented = MakeExecutor<Backend>(f.Augmented, f.Ui);
    const auto input = Excitation();
    const std::vector<double> values{0.4};
    Render(*plain, f, values, input, 7);
    Render(*augmented, f, values, input, 7);
    for (size_t i = 0; i < f.Primal.Fields.size(); ++i) {
        const auto &field = f.Primal.Fields[i];
        const auto found = std::ranges::find_if(f.Augmented.Fields, [&](const auto &other) {
            return other.Sig == field.Sig && other.Kind == field.Kind && other.Hash == field.Hash && other.Extent == field.Extent;
        });
        REQUIRE(found != f.Augmented.Fields.end());
        const auto a = plain->FieldState(uint32_t(i)), b = augmented->FieldState(uint32_t(found - f.Augmented.Fields.begin()));
        REQUIRE(a.size() == b.size());
        for (size_t slot = 0; slot < a.size(); ++slot) {
            if (field.Nature == Nature::Int) CHECK(a[slot].I == b[slot].I);
            else CHECK(BitsOf(a[slot].D) == BitsOf(b[slot].D));
        }
    }
}

TEST_CASE_TEMPLATE("differentiate inactive singular expressions have zero tangents", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f(std::string(SingleControl) + "process(x)=a,sqrt(x);");
    f.Transform();
    const auto y = Sample<Backend>(f, {0.4});
    CHECK(y[0][0] == 0.4);
    CHECK(y[1][0] == 0);
    CHECK(y[2][0] == 1);
    CHECK(y[3][0] == 0);
}

TEST_CASE("differentiate eliminated controls differ from missing control labels") {
    DerivativeFixture f(std::string(SingleControl) + "process=a*0;");
    REQUIRE(f.Labels.size() == 1);
    DifferentiateRequest request{.Controls = f.Labels};
    const auto result = Differentiate(f.Source.Sigs, f.Roots, request, f.Source.Prop.Ui);
    REQUIRE(result.Ok());
    REQUIRE(result.Controls.size() == 1);
    CHECK(result.Controls[0].Present);
    CHECK_FALSE(result.Controls[0].Structural);
    CHECK_FALSE(result.Controls[0].Active);
    CHECK_FALSE(result.Diagnostics.empty());
}

TEST_CASE("differentiate rejects inconsistent logical control metadata") {
    DerivativeFixture f("a=hslider(\"a\",0.4,-10,10,0.01); b=vslider(\"a\",0.4,-5,5,0.01); process=a+b;");
    DifferentiateRequest request{.Controls = f.Labels};
    const auto result = Differentiate(f.Source.Sigs, f.Roots, request, f.Source.Prop.Ui);
    CHECK_FALSE(result.Ok());
}

TEST_CASE("differentiate ignores unsupported unreachable recursion branches") {
    DerivativeFixture f(std::string(SingleControl) + "process=a,ffunction(float derivative_probe(float),\"math.h\",\"\")(a);");
    const SigId reserved = f.Source.Sigs.OpenRec();
    const SigId group = f.Source.Sigs.CloseRec(reserved, f.Roots);
    const SigId projection = f.Source.Sigs.Make(SigKind::Proj, 0, 0, 0, {group});
    f.SetRoots({projection});
    f.Transform();
    CHECK(std::ranges::none_of(f.Derivative.Diagnostics, [](const auto &d) { return d.Kind == DerivativeDiagnosticKind::Unsupported; }));
    Near(Sample<Interp>(f, {0.4})[1][0], 1);
}

TEST_CASE_TEMPLATE("differentiate binary extended functions include both operand contributions", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f("a=hslider(\"a\",1.8,0.1,3,0.01); b=nentry(\"b\",0.7,0.1,3,0.01); process=fmod(a,b),remainder(a,b),a^b,atan2(a,b);");
    f.Transform();
    FiniteDifferences<Backend>(f, {1.8, 0.7});
    const auto y = Sample<Backend>(f, {1.8, 0.7});
    Near(y[5][0], -2);
    Near(y[7][0], -3);
}

TEST_CASE("differentiate request budgets refuse excess work with explicit errors") {
    for (int limit = 0; limit < 4; ++limit) {
        INFO(limit);
        DerivativeFixture f(std::string(SingleControl) + "process(x)=sin(a*x):+~*(a);");
        DifferentiateRequest request{.Controls = f.Labels};
        if (limit == 0) request.MaxDirections = 0;
        if (limit == 1) request.MaxWorkspaceBytes = 1;
        if (limit == 2) request.MaxAnalysisVisits = 1;
        if (limit == 3) request.MaxNewNodes = 1;
        const size_t nodes = f.Source.Sigs.Size();
        const auto result = Differentiate(f.Source.Sigs, f.Roots, request, f.Source.Prop.Ui);
        CHECK_FALSE(result.Ok());
        CHECK_FALSE(result.Error.empty());
        CHECK(result.Tangents.empty());
        CHECK(f.Source.Outs == f.Roots);
        if (limit < 3) CHECK(f.Source.Sigs.Size() == nodes);
    }
}

TEST_CASE_TEMPLATE("differentiate selects full control paths without merging equal leaf labels", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f("process=vgroup(\"left\",hslider(\"a\",0.4,-10,10,0.01)),vgroup(\"right\",hslider(\"a\",0.7,-10,10,0.01));");
    REQUIRE(f.Labels.size() == 2);
    CHECK(f.Labels[0] != f.Labels[1]);
    f.Transform();
    const auto y = Sample<Backend>(f, {0.4, 0.7});
    Near(y[0][0], 0.4);
    Near(y[1][0], 0.7);
    Near(y[2][0], 1);
    Near(y[3][0], 0);
    Near(y[4][0], 0);
    Near(y[5][0], 1);
}

TEST_CASE("differentiate refuses discrete widgets as continuous seeds") {
    for (const char *widget : {"button", "checkbox"}) {
        DerivativeFixture f(std::string("process=") + widget + "(\"switch\");");
        REQUIRE(f.Source.Prop.Ui.size() == 1);
        DifferentiateRequest request{.Controls = {std::string(f.Source.Sigs.Str(f.Source.Prop.Ui[0].Label))}};
        const auto result = Differentiate(f.Source.Sigs, f.Roots, request, f.Source.Prop.Ui);
        CHECK_FALSE(result.Ok());
        CHECK_FALSE(result.Error.empty());
    }
}

TEST_CASE("differentiate soundfile addressing reports structural barriers") {
    DerivativeFixture f(std::string(SingleControl) + "process=0,int(a):soundfile(\"nothing-here\",1);");
    f.Transform();
    REQUIRE(f.Derivative.Controls.size() == 1);
    CHECK(f.Derivative.Controls[0].Structural);
    CHECK_FALSE(f.Derivative.Controls[0].Active);
    CHECK(std::ranges::any_of(f.Derivative.Diagnostics, [](const auto &d) { return d.Kind == DerivativeDiagnosticKind::Barrier; }));
}

TEST_CASE("differentiate state discriminators preserve content hashes across arena allocation orders") {
    struct Hashes {
        std::array<uint64_t, 4> Content, Shape;
        std::array<SigId, 4> Roots;
    };
    const auto construct = [](Signals &s) {
        const SigId input = s.Make(SigKind::Input, 0, 0, 0, {}), zero = s.MakeReal(0), size = s.MakeInt(4), index = s.MakeInt(0);
        std::array<SigId, 4> original, tangent;
        for (size_t j = 0; j < 2; ++j) {
            const SigId init = s.MakeReal(j == 0 ? 0.3 : 0.7);
            original[j] = s.Make(SigKind::Prefix, {init, input});
            tangent[j] = s.Make(SigKind::Prefix, 0, original[j], 1, {zero, input});
            const SigId generator = s.Make(SigKind::Gen, {init}), zeroGenerator = s.Make(SigKind::Gen, {zero});
            original[j + 2] = s.Make(SigKind::WRTbl, {size, generator, index, input});
            tangent[j + 2] = s.Make(SigKind::WRTbl, 0, original[j + 2], 1, {size, zeroGenerator, index, input});
        }
        CHECK(tangent[0] != tangent[1]);
        CHECK(tangent[2] != tangent[3]);
        Hashes hashes;
        hashes.Roots = tangent;
        for (size_t j = 0; j < tangent.size(); ++j) {
            hashes.Content[j] = s.ContentHash(tangent[j]);
            hashes.Shape[j] = s.ShapeHash(tangent[j]);
        }
        CHECK(hashes.Content[0] != hashes.Content[1]);
        CHECK(hashes.Content[2] != hashes.Content[3]);
        CHECK(hashes.Shape[0] == hashes.Shape[1]);
        CHECK(hashes.Shape[2] == hashes.Shape[3]);
        return hashes;
    };
    Signals first, second;
    for (int32_t k = 0; k < 37; ++k) second.MakeInt(k + 100);
    const auto a = construct(first), b = construct(second);
    CHECK(a.Roots != b.Roots);
    CHECK(a.Content == b.Content);
    CHECK(a.Shape == b.Shape);
}

TEST_CASE("differentiate state discriminators survive promotion simplification and table clamping") {
    Signals s;
    const SigId input = s.Make(SigKind::Input, 0, 0, 0, {}), size = s.MakeInt(4), zero = s.MakeInt(0);
    std::array<SigId, 4> original, tagged;
    for (size_t j = 0; j < 2; ++j) {
        const SigId init = s.MakeReal(j == 0 ? 0.3 : 0.7), gen = s.Make(SigKind::Gen, {init});
        original[j] = s.Make(SigKind::Prefix, {init, input});
        // Integer initialization and a real next value force promotion.
        tagged[j] = s.Make(SigKind::Prefix, 0, original[j], 1, {zero, input});
        original[j + 2] = s.Make(SigKind::WRTbl, {size, gen, zero, input});
        // Force index casting/clamping and write-value promotion.
        tagged[j + 2] = s.Make(SigKind::WRTbl, 0, original[j + 2], 1, {size, gen, input, zero});
    }
    std::array<uint64_t, 4> primalHashes;
    for (size_t j = 0; j < original.size(); ++j) primalHashes[j] = s.ContentHash(original[j]);
    const auto normalized = Normalize(s, tagged, true);
    REQUIRE(normalized.size() == tagged.size());
    CHECK(normalized[0] != normalized[1]);
    CHECK(normalized[2] != normalized[3]);
    for (size_t j = 0; j < tagged.size(); ++j) {
        CHECK(s.KindOf(normalized[j]) == (j < 2 ? SigKind::Prefix : SigKind::WRTbl));
        CHECK(s.Get(normalized[j]).Aux == 1);
        CHECK(s.Get(normalized[j]).Payload == original[j]);
        CHECK(s.ContentHash(original[j]) == primalHashes[j]);
    }
    const auto twice = Normalize(s, normalized, true);
    CHECK(normalized == twice);
}

TEST_CASE_TEMPLATE("differentiate standard fractional delay interpolates the active delay parameter", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f("import(\"stdfaust.lib\"); a=hslider(\"delay\",1.4,0.1,7,0.01); process=de.fdelay(8,a);");
    f.Transform();
    REQUIRE(f.Derivative.Controls[0].Active);
    const auto y = FiniteDifferences<Backend>(f, {1.4});
    const auto input = Excitation();
    for (size_t n = 0; n < input.size(); ++n) Near(y[1][n], (n >= 2 ? input[n - 2] : 0) - (n >= 1 ? input[n - 1] : 0));
}

TEST_CASE_TEMPLATE("differentiate standard lowpass follows cutoff through recursive coefficients", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f("import(\"stdfaust.lib\"); a=hslider(\"cutoff\",1000,20,20000,1); process=fi.lowpass(2,a);");
    f.Transform();
    REQUIRE(f.Derivative.Controls[0].Active);
    const auto y = FiniteDifferences<Backend>(f, {1000});
    CHECK(std::ranges::any_of(y[1], [](double value) { return std::abs(value) > 1e-8; }));
}

TEST_CASE_TEMPLATE("differentiate oscillator frequency and phase follow accumulated history", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f(
        "import(\"stdfaust.lib\"); a=hslider(\"frequency\",440,20,20000,1); b=hslider(\"phase\",0.3,-3,3,0.01); process=sin(2*ma.PI*os.phasor(1,a)+b);"
    );
    f.Transform();
    REQUIRE(f.Derivative.Controls[0].Active);
    REQUIRE(f.Derivative.Controls[1].Active);
    const std::vector<double> values{440, 0.3};
    const auto y = FiniteDifferences<Backend>(f, values);
    for (size_t n = 0; n < y[0].size(); ++n) {
        const double radiansPerHz = 2 * std::acos(-1.0) * double(n) / 48000;
        const double cosine = std::cos(radiansPerHz * values[0] + values[1]);
        Near(y[1][n], radiansPerHz * cosine);
        Near(y[2][n], cosine);
    }
}

TEST_CASE("differentiate diagnostics honor the caller allocation budget") {
    DerivativeFixture f(std::string(SingleControl) + "process=floor(a);");
    DifferentiateRequest request{.Controls = f.Labels};
    request.MaxDiagnostics = 0;
    const auto result = Differentiate(f.Source.Sigs, f.Roots, request, f.Source.Prop.Ui);
    CHECK_FALSE(result.Ok());
    CHECK(result.Diagnostics.empty());
    CHECK(result.Tangents.empty());
}

TEST_CASE_TEMPLATE("differentiate raw extended rules preserve ordinary and tiny analytic derivatives", Backend, FAUSTLENS_TEST_EXECUTORS) {
    struct Rule {
        Ext Op;
        double Argument, Expected, Direction = 1;
    };
    const Rule rules[] = {
        {Ext::Acosh, 1.4, 1 / std::sqrt(1.4 * 1.4 - 1)},
        {Ext::Asinh, 0.4, 1 / std::sqrt(1.16)},
        {Ext::Atanh, 0.4, 1 / 0.84},
        {Ext::Cosh, 0.4, std::sinh(0.4)},
        {Ext::Sinh, 0.4, std::cosh(0.4)},
        {Ext::Tanh, 0.4, 1 - std::pow(std::tanh(0.4), 2)},
        {Ext::Ceil, 0.4, 0},
        {Ext::Rint, 0.4, 0},
        {Ext::Round, 0.4, 0},
        {Ext::Asinh, 1e200, 1e-200},
        {Ext::Asinh, -1e200, 1e-200},
        {Ext::Acosh, 1e200, 1e-200},
        {Ext::Atan, 1e200, 1e-200, 1e200},
        {Ext::Atan, -1e200, 1e-200, 1e200},
        // Python Decimal references: 80 digits, rounded to binary64.
        {Ext::Log10, 1e308, 4.34294481903252e-309},
        {Ext::Tanh, 20, 1.6993417021166355e-17},
        {Ext::Tanh, -20, 1.6993417021166355e-17},
        {Ext::Tanh, 500, 2.0303835590197827e-134, 1e300}
    };
    for (const auto &rule : rules) {
        INFO(int(rule.Op), rule.Argument, rule.Direction);
        DerivativeFixture f("a=hslider(\"a\",1.0,-1e308,1e308,1); process=a;");
        const SigId root = f.Source.Sigs.Make(SigKind::Extended, uint8_t(rule.Op), 0, 0, {f.Roots[0]});
        f.SetRoots({root});
        f.Transform(true, 1, {rule.Direction});
        const auto y = Sample<Backend>(f, {rule.Argument});
        CHECK(std::isfinite(y[0][0]));
        CHECK(std::isfinite(y[1][0]));
        if (rule.Expected == 0) CHECK(y[1][0] == 0);
        else CHECK(std::abs((y[1][0] - rule.Expected) / rule.Expected) <= 2e-14);
    }
}

TEST_CASE_TEMPLATE("differentiate atan2 scales both coordinates without losing small derivatives", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f("a=hslider(\"a\",1,-1e201,1e201,1); b=hslider(\"b\",1,-1e201,1e201,1); process=atan2(a,b);");
    f.Transform();
    for (double magnitude : {1e200, 1e-200})
        for (double signA : {-1.0, 1.0})
            for (double signB : {-1.0, 1.0}) {
                INFO(magnitude, signA, signB);
                const auto y = Sample<Backend>(f, {signA * magnitude, signB * magnitude});
                const double da = signB * (0.5 / magnitude), db = -signA * (0.5 / magnitude);
                CHECK(std::isfinite(y[1][0]));
                CHECK(std::isfinite(y[2][0]));
                CHECK(std::abs((y[1][0] - da) / da) <= 2e-14);
                CHECK(std::abs((y[2][0] - db) / db) <= 2e-14);
            }
    const auto y = Sample<Backend>(f, {0, 0});
    CHECK_FALSE(std::isfinite(y[1][0]));
    CHECK_FALSE(std::isfinite(y[2][0]));
}

TEST_CASE_TEMPLATE("differentiate eager invalid branches preserve finite selected derivatives", Backend, FAUSTLENS_TEST_EXECUTORS) {
    DerivativeFixture f(std::string(SingleControl) + "process=select2(a>0,sqrt(-1*a),sqrt(a));");
    f.Transform();
    size_t squareRoots = 0;
    for (const auto &band : f.Augmented.Bands)
        for (const auto &instruction : band)
            if (Op(instruction.Op) == Op::Extended && Ext(instruction.Form) == Ext::Sqrt) ++squareRoots;
    // Verify both branches remain in the eager execution plan.
    CHECK(squareRoots >= 2);
    const auto y = Sample<Backend>(f, {0.4});
    Near(y[0][0], std::sqrt(0.4));
    Near(y[1][0], 1 / (2 * std::sqrt(0.4)));
}
