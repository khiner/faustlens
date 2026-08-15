#include "Live.h"

#include <algorithm>
#include <chrono>
#include <expected>

namespace faustlens::app {

namespace {

// Milliseconds since `at`, which is advanced to now.
double Since(std::chrono::steady_clock::time_point &at) {
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - at).count();
    at = now;
    return ms;
}

std::expected<std::unique_ptr<Artifact>, std::string> Compile(Session &s, const std::string &path, Live::Timings &t) {
    auto a = std::make_unique<Artifact>();
    auto at = std::chrono::steady_clock::now();
    // Asked separately so the profile can tell them apart, both memoized.
    s.TermsOf(path);
    t.Parse = Since(at);
    s.Process(path);
    t.Evaluate = Since(at);
    const Graph g(s, path, a->Sigs);
    if (!g.Ok) return std::unexpected("does not evaluate to a box with a known arity");
    t.Propagate = Since(at);
    auto plan = g.Lower();
    if (!plan) return std::unexpected(std::move(plan).error());
    a->Plan = *std::move(plan);
    t.Lower = Since(at);

    a->Ui = g.Ui(RootLabel(s.Metadata));
    // A path must name one control, and a duplicate is a diagnostic, not a stop.
    a->Diags = CheckPaths(a->Ui);
    a->Hash = Hash(a->Plan);

    const Snapshot snap = Publish(s, {path});
    a->At = FieldOffsets(a->Plan, snap.File(path));
    t.Artifact = Since(at);
    return a;
}

} // namespace

std::vector<uint32_t> FieldOffsets(const Plan &p, const FileView *f) {
    std::vector<uint32_t> out;
    out.reserve(p.Fields.size());
    for (const Field &fl : p.Fields) {
        uint32_t first = Nowhere;
        if (f != nullptr && fl.Origin != NoTerm)
            for (const Span &sp : Marks(*f, fl.Origin)) first = std::min(first, sp.Begin);
        out.push_back(first);
    }
    return out;
}

void Live::Collect() {
    for (const Interp *done : Host.Collect()) std::erase_if(Retiring, [done](const std::unique_ptr<Artifact> &a) { return a->Dsp.get() == done; });
}

Live::Result Live::Reload(Session &s, const std::string &path, const controls::Values &controls) {
    Collect();
    Result r;
    const auto began = std::chrono::steady_clock::now();
    auto compiled = Compile(s, path, r.Timings);
    // The total spans every exit below, so it is stamped on the way out.
    const auto done = [&]() -> Result & {
        r.Timings.Total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        return r;
    };
    auto at = std::chrono::steady_clock::now();
    if (!compiled) {
        r.Why = std::move(compiled).error();
        return done();
    }
    std::unique_ptr<Artifact> next = *std::move(compiled);
    r.Compiled = true;

    // An unchanged program never reaches the audio thread, so the output is identical.
    if (Current && Current->Hash == next->Hash) {
        r.Unchanged = true;
        return done();
    }

    next->Dsp = std::make_unique<Interp>(next->Plan, next->Ui);
    // The decode cache outlives the artifact, so an edit does not re-decode.
    next->Dsp->LoadSoundfiles(&Sound);
    r.Timings.Instance = Since(at);

    if (!Current) {
        // Only a device knows the rate, so 44100 stands in where none is open.
        next->Dsp->Init(Host.Running ? Host.SampleRate : 44100);
        // Over the declared inits `Init` just wrote.
        controls::Apply(controls, next->Plan, next->Ui, *next->Dsp);
        r.Timings.Init = Since(at);
        Current = std::move(next);
        return done();
    }

    next->Dsp->Init(Current->Dsp->SampleRate);
    controls::Apply(controls, next->Plan, next->Ui, *next->Dsp);
    r.Timings.Init = Since(at);
    r.Migration = Migrate(Current->Plan, *Current->Dsp, Current->At, next->Plan, *next->Dsp, next->At);
    r.Timings.Migrate = Since(at);

    if (!Host.Running) {
        // No device, so nothing else can be reading the old instance.
        std::unique_ptr<Artifact> gone = std::move(Current);
        Current = std::move(next);
        gone.reset();
        r.Timings.Release += Since(at);
        return done();
    }
    if (!Host.Swap(*next->Dsp)) {
        // Backpressure, not failure: the audio thread has not let go of enough
        // instances yet, so the old one keeps playing.
        r.Why = "the audio thread has not released the last swap yet";
        return done();
    }
    r.Swapped = true;
    Retiring.push_back(std::move(Current));
    Current = std::move(next);
    return done();
}

} // namespace faustlens::app
