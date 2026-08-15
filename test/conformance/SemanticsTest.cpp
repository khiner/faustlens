#include "conformance/Sweep.h"
#include "runtime/Interp.h"
#include "signal/Plan.h"
#include "signal/Ui.h"

#include "doctest.h"

#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

// `in` and `want` are per channel, and the frame count is `want[0].size()`.
struct Probe {
    const char *Clause;
    const char *Source;
    std::vector<std::vector<double>> In, Want;
    // A substring the diagnostic must contain, where the rule rejects the program.
    const char *Reject = nullptr;
};

const double Inf = std::numeric_limits<double>::infinity();
const double Nan = std::numeric_limits<double>::quiet_NaN();

const std::vector<Probe> &Probes() {
    static const std::vector<Probe> probes = {
        {"`/` always yields a float, even for two integers", "process = int(_) / int(_);", {{3}, {2}}, {{1.5}}},
        {"`%` is C signed remainder: the sign follows the dividend", "process = int(_) % int(_);", {{-7, 7, -7}, {3, -3, -3}}, {{-1, 1, -1}}},
        {"`>>` is arithmetic, not logical", "process = int(_) >> int(_);", {{-8, -1}, {1, 1}}, {{-4, -1}}},
        {"`^` yields the join, so `int ^ int` is an int and truncates", "process = int(_) ^ int(_);", {{2, 2}, {3, -1}}, {{8, 0}}},
        {"the integer arm of the `pow` expansion multiplies in int and wraps", "process = int(_) ^ 3;", {{2000}}, {{-589934592}}},
        {"division by zero is unguarded: floats go to infinity per IEEE", "process = _ , 0.0 : /;", {{1}}, {{Inf}}},
        {"integer arithmetic wraps rather than saturating", "process = int(_) * int(_);", {{100000}, {100000}}, {{1410065408}}},

        {"`int(x)` truncates toward zero, never rounds", "process = int(_);", {{2.7, -2.7, 2.5, -0.5}}, {{2, -2, 2, 0}}},
        // The reference leaves this undefined and its backends disagree, so we define it as
        // its `-cir` rewrite does. Expect a divergence.
        {"float-to-int out of range saturates and NaN converts to zero", "process = int(_);", {{1e18, -1e18, Nan}}, {{2147483647, -2147483648, 0}}},

        {"`:>` sums: output bus b takes inputs b, b + n, b + 2n, ...", "process = _,_,_,_ :> _,_;", {{1}, {2}, {10}, {20}}, {{11}, {22}}},
        {"`<:` replicates modulo the input count", "process = _,_ <: _,_,_,_;", {{1}, {2}}, {{1}, {2}, {1}, {2}}},
        {"`route` is 1-based, additive, and drops an out-of-range entry silently", "process = _,_ : route(2,2, 1,1, 2,1, 3,2);", {{1}, {2}}, {{3}, {0}}},

        {"within a sample an `rwtable` write happens before the read", "process = rwtable(4, 0.0, 0, _, 0);", {{7, 8, 9}}, {{7, 8, 9}}},
        {"a table index the interval cannot prove in range is clamped, not wrapped",
         "process = waveform{10,20,30,40}, int(_) : rdtable;",
         {{0, 3, 100, -5}},
         {{10, 40, 40, 10}}},

        {"a recursive group's output is read undelayed, the `mem` sitting in the body", "process = + ~ _;", {{1, 1, 1}}, {{1, 2, 3}}},
        {"an unbounded delay index is a compile error, not a widened allocation", "process = _ @ int(_);", {}, {}, "delay index is not in [0, INT_MAX)"},
        {"a delay line initializes to zero", "process = _';", {{1, 2, 3}}, {{0, 1, 2}}},
        {"except under `prefix`, where it initializes to the given value", "process = prefix(0.5, _);", {{1, 2, 3}}, {{0.5, 1, 2}}},
        {"a guarded value keeps its last computed result while the guard is false",
         "process = control(_, _ > 0.5);",
         {{1, 2, 3, 4}, {1, 0, 0, 1}},
         {{1, 1, 1, 4}}},

        {"a soundfile that does not load is 1024 silent frames at 44100, per part",
         "process = 0, 0 : soundfile(\"nothing-here\", 1);",
         {},
         {{1024}, {44100}, {0}}},
    };
    return probes;
}

// Exact: every expectation is a number the rule names, not one an accumulation approaches.
bool Same(double got, double want) {
    if (std::isnan(want)) return std::isnan(got);
    return got == want;
}

std::string Run(const Probe &p, std::vector<std::vector<double>> &got) {
    Program const prog("/probe.dsp", p.Source);
    if (!prog.Ok) return "did not evaluate";
    const auto lowered = prog.Lower();
    if (!lowered) return std::format("did not lower: {}", lowered.error());
    const Plan &plan = *lowered;
    if (size_t(plan.Inputs) != p.In.size()) return std::format("declares {} inputs, not {}", plan.Inputs, p.In.size());
    if (size_t(plan.Outputs) != p.Want.size()) return std::format("declares {} outputs, not {}", plan.Outputs, p.Want.size());

    std::map<uint32_t, int> const keep;
    Interp dsp(plan, BuildUiTree(prog.Prop.Ui, "probe", keep));
    dsp.Init(44100);

    const int32_t frames = int32_t(p.Want[0].size());
    std::vector<std::vector<double>> in = p.In;
    got.assign(p.Want.size(), std::vector<double>(frames, 0));
    std::vector<const double *> ip;
    std::vector<double *> op;
    ip.reserve(in.size());
    for (std::vector<double> &c : in) ip.push_back(c.data());
    op.reserve(got.size());
    for (std::vector<double> &c : got) op.push_back(c.data());
    // One `compute` for the whole probe -- the `.ir` runner covers block boundaries.
    dsp.Compute(frames, ip.data(), op.data());
    return "";
}

} // namespace

TEST_CASE("pinned semantics, each probed against the rule rather than against the oracle") {
    int ok = 0;
    for (const Probe &p : Probes()) {
        INFO(p.Clause);
        std::vector<std::vector<double>> got;
        const std::string why = Run(p, got);
        if (p.Reject) {
            const bool rejected = why.find(p.Reject) != std::string::npos;
            if (!rejected) MESSAGE("  ", why.empty() ? std::string("compiled") : why);
            CHECK(rejected);
            ok += rejected;
            continue;
        }
        if (!why.empty()) {
            MESSAGE("  ", why);
            CHECK(why.empty());
            continue;
        }
        bool matched = true;
        for (size_t c = 0; c < p.Want.size(); ++c)
            for (size_t f = 0; f < p.Want[c].size(); ++f)
                if (!Same(got[c][f], p.Want[c][f])) {
                    MESSAGE("  output ", c, " frame ", f, ": ", got[c][f], " where the rule says ", p.Want[c][f]);
                    matched = false;
                }
        CHECK(matched);
        ok += matched;
    }
    MESSAGE("semantic probes: ", ok, " of ", Probes().size(), " meet the rule they pin");
}
