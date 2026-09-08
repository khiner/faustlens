#include "Live.h"

#include <algorithm>
#include <chrono>
#include <expected>

namespace faustlens::app {

double Live::SampleRate() const { return Host.Running ? Host.SampleRate : Current ? Current->Dsp->SampleRate : 44100; }

namespace {

double Since(std::chrono::steady_clock::time_point &at) {
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - at).count();
    at = now;
    return ms;
}

std::expected<std::unique_ptr<Artifact>, std::string> Compile(Session &s, const std::string &path, Live::Timings &t) {
    auto a = std::make_unique<Artifact>();
    auto at = std::chrono::steady_clock::now();
    // Time the memoized queries separately.
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
    // Report duplicate control paths without stopping compilation.
    a->Diags = CheckPaths(a->Ui);
    a->Hash = Hash(a->Plan);

    a->At = FieldOffsets(a->Plan, s.TermsOf(path).Refs);
    t.Artifact = Since(at);
    return a;
}

} // namespace

std::vector<uint32_t> FieldOffsets(const Plan &p, const RefTree &refs) {
    std::vector<uint32_t> out;
    out.reserve(p.Fields.size());
    for (const Field &fl : p.Fields) {
        uint32_t first = Nowhere;
        if (fl.Origin != NoTerm)
            for (const TermRef &ref : refs.Refs)
                if (ref.ValueId == fl.Origin) first = std::min(first, ref.SpanBegin);
        out.push_back(first);
    }
    return out;
}

std::vector<std::shared_ptr<Artifact>> Live::Collect() {
    std::vector<std::shared_ptr<Artifact>> garbage;
    for (const Interp *done : Host.Collect())
        std::erase_if(Retiring, [&](std::shared_ptr<Artifact> &a) {
            if (a->Dsp.get() != done) return false;
            garbage.push_back(std::move(a));
            return true;
        });
    return garbage;
}

Live::Prepared Live::Build(
    Session &s, const std::string &path, std::shared_ptr<const Artifact> base, const controls::Values &controls, double sample_rate, audio::Decoder &sound
) {
    Prepared prepared;
    prepared.Base = std::move(base);
    Result &r = prepared.Status;
    const auto began = std::chrono::steady_clock::now();
    auto compiled = Compile(s, path, r.Timings);
    const auto done = [&]() {
        r.Timings.Total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        return std::move(prepared);
    };
    if (!compiled) {
        r.Why = std::move(compiled).error();
        return done();
    }
    auto next = std::move(*compiled);
    r.Compiled = true;
    if (prepared.Base && prepared.Base->Hash == next->Hash) {
        r.Unchanged = true;
        return done();
    }
    auto at = std::chrono::steady_clock::now();
    next->Dsp = std::make_unique<Interp>(next->Plan, next->Ui);
    next->Dsp->LoadSoundfiles(&sound);
    r.Timings.Instance = Since(at);
    next->Dsp->Init(sample_rate);
    controls::Apply(controls, next->Plan, next->Ui, *next->Dsp);
    r.Timings.Init = Since(at);
    if (prepared.Base) {
        prepared.Transfer = MatchState(prepared.Base->Plan, prepared.Base->At, next->Plan, next->At);
        r.Migration = prepared.Transfer.Counts;
        r.Timings.Migrate = Since(at);
    }
    prepared.Next = std::move(next);
    return done();
}

Live::Result Live::Accept(Prepared &prepared, const controls::Values &controls) {
    Result r = prepared.Status;
    if (!r.Compiled) return r;
    if (prepared.Base.get() != Current.get()) {
        r.Compiled = false;
        r.Why = "the running program changed during compilation";
        return r;
    }
    if (r.Unchanged || !prepared.Next) return r;
    auto &next = prepared.Next;
    // Apply control changes made during compilation.
    controls::Apply(controls, next->Plan, next->Ui, *next->Dsp);
    if (Host.Running) {
        if (!Host.Swap(*next->Dsp, Current ? Current->Dsp.get() : nullptr, prepared.Transfer)) {
            r.Deferred = true;
            r.Why = "waiting for the audio swap boundary";
            return r;
        }
        r.Swapped = true;
        if (Current) Retiring.push_back(std::move(Current));
    } else if (Current) {
        TransferState(prepared.Transfer, *Current->Dsp, *next->Dsp);
    }
    Current = std::move(next);
    return r;
}

Live::Result Live::Reload(Session &s, const std::string &path, const controls::Values &controls) {
    Collect();
    auto prepared = Build(s, path, Current, controls, SampleRate(), Sound);
    return Accept(prepared, controls);
}

} // namespace faustlens::app
