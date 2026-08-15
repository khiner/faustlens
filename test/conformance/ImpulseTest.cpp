// Reproduces the reference's harness: two compared sections at 44100 Hz, an impulse on
// every input and buttons (not checkboxes) at 1 in the first block. The 2e-06 tolerance
// comes from its `%8.6f` printing.
#include "conformance/Sweep.h"
#include "property/Corpus.h"
#include "runtime/Interp.h"
#include "signal/Plan.h"
#include "signal/Ui.h"

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
constexpr double Tolerance = 2e-06;

// `bs` reads the block size, so only the reference's `rand()` reproduces section two. Skip it.
bool ReadsBlockSize(const std::string &name) { return name == "bs"; }

// The regenerated `table.ir` is miscompiled, its fill loop needing signed `int` overflow to wrap.
bool PinnedToShipped(const std::string &name) { return name == "table"; }

struct HarnessSound : SoundfileReader {
    bool Read(const std::string &, uint32_t part, std::vector<std::vector<double>> &ch, int32_t &rate) override {
        ch.assign(2, std::vector<double>(4096));
        for (int32_t s = 0; s < 4096; ++s) {
            const double v = std::sin(part + (2 * M_PI * double(s) / 4096.0));
            ch[0][s] = ch[1][s] = v;
        }
        rate = 44100;
        return true;
    }
};

// Round-tripped, because reading the printed text back is what the comparison sees.
bool Print(double v, double &out) {
    if (std::isnan(v) || std::isinf(v)) return false;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%8.6f", std::fabs(v) < 1e-06 ? 0.0 : v);
    out = std::strtod(buf, nullptr);
    return true;
}

struct Response {
    int32_t Inputs = 0, Outputs = 0;
    std::vector<double> Rows; // `outputs` values per frame, two sections' worth
    int32_t Frames = 0;
    bool Aborted = false;
};

// `split` runs each block as two `compute` calls, at the midpoint where the reference randomizes.
void RunSection(Interp &dsp, std::span<const uint32_t> buttons, bool split, Response &r) {
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
            compute(0, frames / 2);
            compute(frames / 2, frames - frames / 2);
        } else {
            compute(0, frames);
        }

        for (int32_t i = 0; i < frames; ++i) {
            for (int32_t c = 0; c < nout; ++c) {
                double v;
                if (!Print(out[c][i], v)) {
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
    std::string Why; // empty where it matched
    std::string Note;
    std::string Example;
    bool Compared = false;
};

void WriteIr(const fs::path &dir, const std::string &name, const Response &r) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream out(dir / (name + ".ir"));
    out << "number_of_inputs  : " << std::setw(3) << r.Inputs << "\n"
        << "number_of_outputs : " << std::setw(3) << r.Outputs << "\n"
        << "number_of_frames  : " << std::setw(6) << r.Frames << "\n";
    char buf[32];
    for (int32_t f = 0; f < r.Frames; ++f) {
        std::snprintf(buf, sizeof buf, "%6d : ", f);
        out << buf;
        for (int32_t c = 0; c < r.Outputs; ++c) {
            std::snprintf(buf, sizeof buf, " %8.6f", r.Rows[size_t(f) * r.Outputs + c]);
            out << buf;
        }
        out << "\n";
    }
}

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

    HarnessSound sound;
    Interp dsp(plan, ui);
    dsp.LoadSoundfiles(&sound);
    dsp.Init(44100);

    Response r;
    r.Inputs = plan.Inputs;
    r.Outputs = plan.Outputs;
    const std::vector<uint32_t> buttons = dsp.ControlsOfKind(UiKind::Button);
    RunSection(dsp, buttons, false, r);
    // The harness makes a new dsp per section, so the second starts from `init`.
    if (!r.Aborted) {
        Interp again(plan, ui);
        again.LoadSoundfiles(&sound);
        again.Init(44100);
        RunSection(again, again.ControlsOfKind(UiKind::Button), true, r);
    }

    if (const char *dir = std::getenv("FAUSTLENS_IR_OUT")) WriteIr(dir, v.Name, r);

    const int32_t want = ReadsBlockSize(v.Name) ? Section : 2 * Section;
    const fs::path from = PinnedToShipped(v.Name) ? ImpulseDir() / "reference" / (v.Name + ".ir") : OracleDir() / "ir" / (v.Name + ".ir");
    const Reference ref = ReadReference(from, want);
    if (!ref.Ok) {
        v.Why = "no reference `.ir`";
        return v;
    }
    v.Compared = true;
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
    if (v.Why.empty() && PinnedToShipped(v.Name)) v.Note = "matched against the shipped `.ir`: the regenerated one is miscompiled";
    return v;
}

} // namespace

TEST_CASE("impulse responses: the interpreter against the reference's `.ir`") {
    const std::vector<fs::path> paths = DspPaths();
    const std::vector<Verdict> verdicts = MapEach<Verdict>(paths, Measure);

    int matched = 0, compared = 0;
    Census census;
    std::vector<std::string> notes;
    for (const Verdict &v : verdicts) {
        compared += v.Compared;
        if (!v.Note.empty()) notes.push_back(v.Name + ": " + v.Note);
        if (v.Why.empty()) {
            ++matched;
            continue;
        }
        census.Add(v.Why, v.Example.empty() ? v.Name : v.Example);
    }

    MESSAGE(".ir impulse parity: ", matched, " of ", paths.size(), " match the reference within 2e-06, over ", compared, " compared");
    census.Report();
    for (const std::string &n : notes) MESSAGE("  ", n);

    // Everything on `AssociationOrder` matches too, `freeverb` and `zita_rev1` included.
    CHECK(matched == 94);
}
