#pragma once

#include "audio/Decode.h"
#include "audio/Host.h"
#include "controls/Store.h"
#include "query/Query.h"
#include "runtime/Interp.h"
#include "runtime/Migrate.h"
#include "signal/Plan.h"
#include "signal/Signal.h"
#include "signal/Ui.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace faustlens::app {

// The DSP references the other artifact members.
struct Artifact {
    Signals Sigs;
    Plan Plan;
    UiNode Ui;
    uint64_t Hash = 0;
    // Source offsets captured before the ref tree is replaced.
    std::vector<uint32_t> At;
    // Metadata for the running program.
    std::vector<Diagnostic> Diags;
    std::unique_ptr<Interp> Dsp;
};

// Return the earliest source offset per Plan field, or `Nowhere`.
std::vector<uint32_t> FieldOffsets(const Plan &, const RefTree &refs);

struct Live {
    // Preparation time in ms, excluding hand-off, callback state copy, and retirement.
    struct Timings {
        double Parse = 0, Evaluate = 0, Propagate = 0, Lower = 0, Artifact = 0;
        double Instance = 0, Init = 0, Migrate = 0, Total = 0;
    };

    struct Result {
        bool Compiled = false;
        bool Swapped = false;
        bool Unchanged = false; // equal Plan hash
        bool Deferred = false; // pending audio swap
        std::string Why; // compile or swap failure
        Migration Migration;
        Timings Timings;
    };

    struct Prepared {
        std::shared_ptr<Artifact> Next;
        std::shared_ptr<const Artifact> Base;
        StateTransfer Transfer;
        Result Status;
    };

    // Compile and initialize on the worker without reading running DSP state.
    static Prepared
    Build(Session &, const std::string &path, std::shared_ptr<const Artifact> base, const controls::Values &, double sample_rate, audio::Decoder &);
    // Publish on the host-owning thread, retaining deferred instances for retry.
    Result Accept(Prepared &, const controls::Values & = {});

    // Compile and migrate `process` from `path`; the caller starts the audio device.
    Result Reload(Session &, const std::string &path, const controls::Values &controls = {});

    double SampleRate() const;

    // Collect on the host-owning thread and return artifacts to the worker for destruction.
    std::vector<std::shared_ptr<Artifact>> Collect();

    audio::Decoder Sound;
    // Latest accepted artifact; the callback may still use its predecessor.
    std::shared_ptr<Artifact> Current;
    // Artifacts retained until the audio thread releases their voices.
    std::vector<std::shared_ptr<Artifact>> Retiring;
    audio::Host Host; // Destroyed first to stop callbacks before artifacts.
};

} // namespace faustlens::app
