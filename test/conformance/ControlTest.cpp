// "Only what depends on it" is asserted over graph reachability. "Within one block" is
// only measured.
#include "conformance/Sweep.h"
#include "property/Corpus.h"
#include "runtime/Interp.h"
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

// A value the host could write that is not the one `resetControls` left there.
double Elsewhere(const UiNode &w) {
    if (w.Kind == UiKind::Button || w.Kind == UiKind::Checkbox) return w.Init == 0 ? 1 : 0;
    return w.Init == w.Max ? w.Min : w.Max;
}

std::vector<double> Blocks(const Plan &p, const UiNode &ui, uint32_t write, double value) {
    Interp dsp(p, ui);
    dsp.Init(44100);
    const int32_t nin = dsp.Inputs(), nout = dsp.Outputs();
    std::vector<std::vector<double>> in(std::max(nin, 1), std::vector<double>(Block, 0.0));
    std::vector<std::vector<double>> out(std::max(nout, 1), std::vector<double>(Block, 0.0));
    std::vector<const double *> ip(std::max(nin, 1));
    std::vector<double *> op(std::max(nout, 1));
    for (int32_t c = 0; c < nin; ++c) ip[c] = in[c].data();
    for (int32_t c = 0; c < nout; ++c) op[c] = out[c].data();

    for (int32_t b = 0; b <= Settle; ++b) {
        // A sine, not an impulse: most programs are silent by the measured block and undercount.
        for (int32_t c = 0; c < nin; ++c)
            for (int32_t i = 0; i < Block; ++i) in[c][i] = 0.25 * std::sin(2 * M_PI * 440.0 * (b * Block + i) / 44100.0);
        if (b == Settle && write != 0xFFFFFFFFu) dsp.SetControl(write, value);
        dsp.Compute(Block, ip.data(), op.data());
    }
    std::vector<double> rows;
    for (int32_t i = 0; i < Block; ++i)
        for (int32_t c = 0; c < nout; ++c) rows.push_back(out[c][i]);
    return rows;
}

struct Result {
    std::string Name;
    int Controls = 0, Moved = 0;
    // Per kind, because a whole kind reading zero is the shape a scheduling bug takes.
    std::map<std::string, std::pair<int, int>> ByKind; // moved, total
    std::vector<std::string> Bad;
};

const char *KindName(UiKind k) {
    switch (k) {
        case UiKind::Button: return "button";
        case UiKind::Checkbox: return "checkbox";
        case UiKind::VSlider: return "vslider";
        case UiKind::HSlider: return "hslider";
        case UiKind::NumEntry: return "nentry";
        default: return "other";
    }
}

Result Measure(const fs::path &path) {
    Result r;
    r.Name = path.stem().string();

    Program const prog(path);
    if (!prog.Ok) return r;
    const Signals &sigs = prog.Sigs;
    const std::vector<SigId> &outs = prog.Outs;
    const auto lowered = prog.Lower();
    if (!lowered) return r;
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

    const std::vector<double> base = Blocks(plan, ui, 0xFFFFFFFFu, 0);
    const int32_t nout = plan.Outputs;
    for (const UiNode *w : widgets) {
        ++r.Controls;
        const std::vector<double> moved = Blocks(plan, ui, w->WidgetLabel, Elsewhere(*w));
        bool any = false;
        for (int32_t c = 0; c < nout; ++c) {
            bool changed = false;
            for (int32_t i = 0; i < Block && !changed; ++i) changed = base[size_t(i) * nout + c] != moved[size_t(i) * nout + c];
            if (!changed) continue;
            any = true;
            if (!depends[c].contains(w->WidgetLabel))
                r.Bad.push_back(std::format("{}: writing `{}` moved output {}, which does not read it", r.Name, w->Label, c));
        }
        r.Moved += any;
        auto &k = r.ByKind[KindName(w->Kind)];
        k.first += any;
        ++k.second;
    }
    return r;
}

} // namespace

TEST_CASE("control responsiveness: a control changes the next block, and only what reads it") {
    const std::vector<Result> results = MapEach<Result>(DspPaths(), Measure);

    int controls = 0, moved = 0, programs = 0;
    std::map<std::string, std::pair<int, int>> by_kind;
    std::vector<std::string> bad;
    for (const Result &r : results) {
        controls += r.Controls;
        moved += r.Moved;
        programs += r.Controls > 0;
        for (const auto &[k, n] : r.ByKind) {
            by_kind[k].first += n.first;
            by_kind[k].second += n.second;
        }
        bad.insert(bad.end(), r.Bad.begin(), r.Bad.end());
    }

    MESSAGE("control responsiveness: ", moved, " of ", controls, " controls across ", programs, " programs move an output in the block after the write");
    std::string kinds;
    for (const auto &[k, n] : by_kind) kinds += std::format("{}{} {}/{}", kinds.empty() ? "" : ", ", k, n.first, n.second);
    MESSAGE("  by kind: ", kinds);
    for (const std::string &b : bad) MESSAGE("  ", b);
    CHECK(bad.empty());
    // A control read in the wrong band stops moving anything without failing the check above.
    CHECK(moved >= 560);
}
