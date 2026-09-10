#include "conformance/Sweep.h"
#include "property/Corpus.h"
#include "runtime/Executors.h"
#include "signal/Plan.h"
#include "signal/Ui.h"

#include "doctest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

namespace fs = std::filesystem;

constexpr int32_t Block = 64, Settle = 2;

bool IsInput(UiKind k) {
    switch (k) {
        case UiKind::Button:
        case UiKind::Checkbox:
        case UiKind::VSlider:
        case UiKind::HSlider:
        case UiKind::NumEntry: return true;
        default: return false;
    }
}

double Elsewhere(const UiNode &w) {
    if (w.Kind == UiKind::Button || w.Kind == UiKind::Checkbox) return w.Init == 0 ? 1 : 0;
    return w.Init == w.Max ? w.Min : w.Max;
}

template<class Backend> std::vector<double> Blocks(const Plan &p, const UiNode &ui, uint32_t write, double value) {
    auto dsp = MakeExecutor<Backend>(p, ui);
    dsp->Init(44100);
    const int32_t nin = dsp->Inputs(), nout = dsp->Outputs();
    std::vector<std::vector<double>> in(std::max(nin, 1), std::vector<double>(Block, 0.0));
    std::vector<std::vector<double>> out(std::max(nout, 1), std::vector<double>(Block, 0.0));
    std::vector<const double *> ip(std::max(nin, 1));
    std::vector<double *> op(std::max(nout, 1));
    for (int32_t c = 0; c < nin; ++c) ip[c] = in[c].data();
    for (int32_t c = 0; c < nout; ++c) op[c] = out[c].data();

    for (int32_t b = 0; b <= Settle; ++b) {
        // Sine input provides a continuous signal during control measurements.
        for (int32_t c = 0; c < nin; ++c)
            for (int32_t i = 0; i < Block; ++i) in[c][i] = 0.25 * std::sin(2 * M_PI * 440.0 * (b * Block + i) / 44100.0);
        if (b == Settle && write != 0xFFFFFFFFu) dsp->SetControl(write, value);
        dsp->Compute(Block, ip.data(), op.data());
    }
    std::vector<double> rows;
    for (int32_t i = 0; i < Block; ++i)
        for (int32_t c = 0; c < nout; ++c) rows.push_back(out[c][i]);
    return rows;
}

struct Result {
    std::string Name;
    std::vector<std::string> Bad;
};

template<class Backend> Result Measure(const fs::path &path) {
    Result r;
    r.Name = path.stem().string();

    Program const prog(path);
    if (!prog.Ok) {
        r.Bad.push_back("program did not compile");
        return r;
    }
    const Signals &sigs = prog.Sigs;
    const std::vector<SigId> &outs = prog.Outs;
    const auto lowered = prog.Lower();
    if (!lowered) {
        r.Bad.push_back(lowered.error());
        return r;
    }
    const Plan &plan = *lowered;

    std::vector<std::set<uint32_t>> depends(outs.size());
    std::map<uint32_t, int> keep;
    for (size_t c = 0; c < outs.size(); ++c)
        Reachable(sigs, std::array{outs[c]}, [&](SigId id) {
            if (IsLabelled(sigs.KindOf(id))) {
                depends[c].insert(sigs.Get(id).Payload);
                ++keep[sigs.Get(id).Payload];
            }
            return true;
        });
    const UiNode ui = BuildUiTree(prog.Prop.Ui, r.Name, keep);

    std::vector<const UiNode *> widgets;
    ForEachWidget(ui, [&](const UiNode &w) {
        if (IsInput(w.Kind)) widgets.push_back(&w);
        return true;
    });
    if (widgets.empty()) return r;

    const std::vector<double> base = Blocks<Backend>(plan, ui, 0xFFFFFFFFu, 0);
    const int32_t nout = plan.Outputs;
    for (const UiNode *w : widgets) {
        const std::vector<double> moved = Blocks<Backend>(plan, ui, w->WidgetLabel, Elsewhere(*w));
        for (int32_t c = 0; c < nout; ++c) {
            bool changed = false;
            for (int32_t i = 0; i < Block && !changed; ++i) changed = base[size_t(i) * nout + c] != moved[size_t(i) * nout + c];
            if (!changed) continue;
            if (!depends[c].contains(w->WidgetLabel))
                r.Bad.push_back(std::format("{}: writing `{}` moved output {}, which does not read it", r.Name, w->Label, c));
        }
    }
    return r;
}

} // namespace

TEST_CASE_TEMPLATE("control changes affect only dependent outputs", Backend, FAUSTLENS_TEST_EXECUTORS) {
    const auto paths = DspPaths();
    REQUIRE(paths.size() == 94);
    const auto results = MapEach<Result>(paths, Measure<Backend>);
    REQUIRE(results.size() == paths.size());
    for (const auto &result : results) {
        INFO(result.Name);
        for (const auto &error : result.Bad) FAIL_CHECK(error);
    }
}
