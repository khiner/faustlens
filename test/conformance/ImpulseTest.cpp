#include "conformance/Sweep.h"
#include "property/Corpus.h"
#include "runtime/Executors.h"

#include "signal/Plan.h"
#include "signal/Ui.h"
#include <bit>

#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

namespace fs = std::filesystem;

constexpr int32_t Block = 64, Section = 15000;
// Allow reference %8.6f output rounding.
constexpr double Tolerance = 2e-06;

// Skip randomized-block comparison for bs because it depends on the reference rand sequence.
bool ReadsBlockSize(const std::string &name) { return name == "bs"; }

bool ReadHarnessSound(void *, const std::string &, uint32_t part, std::vector<std::vector<double>> &ch, int32_t &rate) {
    ch.assign(2, std::vector<double>(4096));
    for (int32_t s = 0; s < 4096; ++s) {
        const double v = std::sin(part + (2 * M_PI * double(s) / 4096.0));
        ch[0][s] = ch[1][s] = v;
    }
    rate = 44100;
    return true;
}

bool Print(double v, double &out) {
    if (std::isnan(v) || std::isinf(v)) return false;
    char buf[32];
    // Match the reference harness's sample formatting.
    std::snprintf(buf, sizeof buf, "%8.6f", std::fabs(v) < 1e-06 ? 0.0 : v);
    out = std::strtod(buf, nullptr);
    return true;
}

struct Response {
    int32_t Inputs = 0, Outputs = 0;
    std::vector<double> Rows;
    int32_t Frames = 0;
    bool Aborted = false;
};

void RunSection(Instance &dsp, std::span<const uint32_t> buttons, bool split, Response &r, bool raw = false) {
    const int32_t nin = dsp.Inputs(), nout = dsp.Outputs();
    std::vector<std::vector<double>> in(std::max(nin, 1), std::vector<double>(Block, 0.0));
    std::vector<std::vector<double>> out(std::max(nout, 1), std::vector<double>(Block, 0.0));
    std::vector<const double *> in_at(std::max(nin, 1));
    std::vector<double *> out_at(std::max(nout, 1));

    for (int32_t done = 0, block = 0; done < Section; ++block) {
        const int32_t frames = std::min(Block, Section - done);
        for (int32_t c = 0; c < nin; ++c) {
            std::ranges::fill(in[c], 0.0);
            if (block == 0) in[c][0] = 1.0;
        }
        for (const uint32_t b : buttons) dsp.SetControl(b, block == 0 ? 1.0 : 0.0);

        const auto compute = [&](int32_t at, int32_t n) {
            for (int32_t c = 0; c < nin; ++c) in_at[c] = in[c].data() + at;
            for (int32_t c = 0; c < nout; ++c) out_at[c] = out[c].data() + at;
            dsp.Compute(n, in_at.data(), out_at.data());
        };
        if (split) {
            // Use half-block splits to check independence from the reference harness's randomized positions.
            compute(0, frames / 2);
            compute(frames / 2, frames - frames / 2);
        } else {
            compute(0, frames);
        }

        for (int32_t i = 0; i < frames; ++i) {
            for (int32_t c = 0; c < nout; ++c) {
                double v = out[c][i];
                if (!raw && !Print(v, v)) {
                    r.Aborted = true;
                    return;
                }
                r.Rows.push_back(v);
            }
            ++r.Frames;
        }
        done += frames;
    }
}

struct Reference {
    bool Ok = false;
    int32_t Inputs = 0, Outputs = 0;
    std::vector<double> Rows;
};

Reference ReadReference(const fs::path &p, int32_t want_rows) {
    Reference r;
    std::ifstream in(p);
    if (!in) return r;
    std::string line, dummy;
    for (int header = 0; header < 3; ++header) {
        if (!std::getline(in, line)) return r;
        std::istringstream ls(line);
        int32_t v = 0;
        ls >> dummy >> dummy >> v;
        if (header == 0) r.Inputs = v;
        if (header == 1) r.Outputs = v;
    }
    r.Rows.reserve(size_t(want_rows) * r.Outputs);
    for (int32_t row = 0; row < want_rows && std::getline(in, line); ++row) {
        std::istringstream ls(line);
        ls >> dummy >> dummy;
        for (int32_t c = 0; c < r.Outputs; ++c) {
            double v = 0;
            ls >> v;
            r.Rows.push_back(v);
        }
    }
    r.Ok = true;
    return r;
}

struct Verdict {
    std::string Name;
    std::string Why;
    std::string Example;
};

Verdict Measure(const fs::path &path) {
    Verdict v;
    v.Name = path.stem().string();

    Program const prog(path);
    if (!prog.Ok) {
        v.Why = "did not evaluate";
        return v;
    }

    const auto lowered = prog.Lower();
    if (!lowered) {
        v.Why = "did not lower: " + lowered.error();
        return v;
    }
    const Plan &plan = *lowered;

    const UiNode ui = prog.Ui(v.Name);

    SoundfileReader sound{nullptr, ReadHarnessSound};
    Interp dsp(plan, ui);
    dsp.LoadSoundfiles(&sound);
    dsp.Init(44100);

    Response r;
    r.Inputs = plan.Inputs;
    r.Outputs = plan.Outputs;
    const std::vector<uint32_t> buttons = dsp.ControlsOfKind(UiKind::Button);
    RunSection(dsp, buttons, false, r);
    if (!r.Aborted) {
        Interp again(plan, ui);
        again.LoadSoundfiles(&sound);
        again.Init(44100);
        RunSection(again, again.ControlsOfKind(UiKind::Button), true, r);
    }

    const int32_t want = ReadsBlockSize(v.Name) ? Section : 2 * Section;
    // The generated table fill requires wrapping signed arithmetic; use the shipped reference trace.
    const fs::path from = v.Name == "table" ? ImpulseDir() / "reference" / (v.Name + ".ir") : OracleDir() / "ir" / (v.Name + ".ir");
    const Reference ref = ReadReference(from, want);
    if (!ref.Ok) {
        v.Why = "no reference `.ir`";
        return v;
    }
    if (ref.Inputs != r.Inputs || ref.Outputs != r.Outputs) {
        v.Why = "channel counts differ from the reference's";
        v.Example = std::format("{}: {}/{} against {}/{}", v.Name, r.Inputs, r.Outputs, ref.Inputs, ref.Outputs);
        return v;
    }

    std::string differs;
    if (r.Aborted) {
        differs = "diverged to NaN or infinity";
        v.Example = std::format("{}: at frame {}", v.Name, r.Frames);
    }
    const size_t rows = std::min<size_t>(r.Rows.size(), ref.Rows.size());
    for (size_t k = 0; differs.empty() && k < rows; ++k) {
        const double delta = std::fabs(r.Rows[k] - ref.Rows[k]);
        if (delta <= Tolerance) continue;
        char buf[256];
        std::snprintf(
            buf, sizeof buf, "%s: frame %zu output %zu, %8.6f against %8.6f (delta %g)", v.Name.c_str(), k / r.Outputs, k % r.Outputs, r.Rows[k], ref.Rows[k],
            delta
        );
        differs = "differs beyond 2e-06";
        v.Example = buf;
    }

    v.Why = differs;
    return v;
}

} // namespace

TEST_CASE("source programs and reference diagrams match the reference impulse responses") {
    for (bool diagrams : {false, true}) {
        INFO(diagrams);
        auto paths = DspPaths();
        REQUIRE(paths.size() == 94);
        if (diagrams)
            for (auto &path : paths) path = OracleDir() / (path.stem().string() + ".box");
        const auto verdicts = MapEach<Verdict>(paths, Measure);
        REQUIRE(verdicts.size() == paths.size());
        for (const Verdict &v : verdicts) {
            INFO(v.Name, v.Example);
            CHECK_MESSAGE(v.Why.empty(), v.Why);
        }
    }
}

#if defined(__APPLE__) && defined(__aarch64__)
TEST_CASE("native corpus responses and final state match the interpreter at full precision") {
    const auto same = [](double a, double b) { return (std::isnan(a) && std::isnan(b)) || std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b); };
    const auto paths = DspPaths();
    REQUIRE(paths.size() == 94);
    for (const auto &path : paths) {
        INFO(path.string());
        const Program program(path);
        REQUIRE(program.Ok);
        const auto plan = program.Lower();
        REQUIRE(plan);
        const auto ui = program.Ui("native");
        auto compiled = arm64::Program::Compile(*plan, ui);
        REQUIRE(compiled);
        const auto bytes = (*compiled)->Encode();
        auto decoded = arm64::Program::Decode(bytes);
        REQUIRE(decoded);
        REQUIRE((*decoded)->Encode() == bytes);
        auto code = NativeCode::Publish(*decoded);
        REQUIRE(code);
        auto interp = MakeExecutor<Interp>(*plan, ui);
        auto native = Native::Create(*code);
        SoundfileReader sound{nullptr, ReadHarnessSound};
        interp->LoadSoundfiles(&sound);
        native->LoadSoundfiles(&sound);
        for (bool split : {false, true}) {
            INFO(split);
            interp->Init(44100);
            native->Init(44100);
            Response a, b;
            RunSection(*interp, interp->ControlsOfKind(UiKind::Button), split, a, true);
            RunSection(*native, native->ControlsOfKind(UiKind::Button), split, b, true);
            REQUIRE(a.Rows.size() == b.Rows.size());
            for (size_t k = 0; k < a.Rows.size(); ++k) {
                INFO(k, a.Rows[k], b.Rows[k]);
                REQUIRE(same(a.Rows[k], b.Rows[k]));
            }
            for (size_t f = 0; f < plan->Fields.size(); ++f)
                for (uint32_t k = 0; k < plan->Fields[f].Extent; ++k) {
                    INFO(f, k);
                    const size_t at = interp->FieldAt[f] + k;
                    if (plan->Fields[f].Nature == Nature::Int) REQUIRE(interp->State[at].I == native->State[at].I);
                    else REQUIRE(same(interp->State[at].D, native->State[at].D));
                }
        }
    }
}
#endif
