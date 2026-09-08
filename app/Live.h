// Compile, migrate, hand off. No artifact means no swap, and the last good one plays on.
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

// `dsp` holds refs into the rest, so the bundle lives and dies together.
struct Artifact {
    Signals Sigs;
    Plan Plan;
    UiNode Ui;
    uint64_t Hash = 0;
    // Where each field is written, taken while the ref tree that answers it lives.
    std::vector<uint32_t> At;
    // Of the program being *heard*, not the one on screen.
    std::vector<Diagnostic> Diags;
    std::unique_ptr<Interp> Dsp;
};

// One offset per field in field order: the earliest byte in `refs` it came out of, or
// `Nowhere`. Valid while the Plan's compile is.
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
        bool Unchanged = false; // the same Plan, so no swap
        bool Deferred = false; // retry publication after the pending audio swap is taken
        std::string Why; // why not: the compile failed, or the host refused the swap
        Migration Migration;
        Timings Timings;
    };

    struct Prepared {
        std::shared_ptr<Artifact> Next;
        std::shared_ptr<const Artifact> Base;
        StateTransfer Transfer;
        Result Status;
    };

    // Worker thread: compile and initialize, without reading running DSP state.
    static Prepared
    Build(Session &, const std::string &path, std::shared_ptr<const Artifact> base, const controls::Values &, double sample_rate, audio::Decoder &);
    // Host-owning thread: publish a prepared instance. Leaves a deferred instance intact.
    Result Accept(Prepared &, const controls::Values & = {});

    // Compiles `process` of `path`, migrating running state into the new
    // instance. Does not open a device: the caller starts the host.
    Result Reload(Session &, const std::string &path, const controls::Values &controls = {});

    double SampleRate() const;

    // Host-owning thread, once a frame. Return artifacts to the worker for destruction.
    std::vector<std::shared_ptr<Artifact>> Collect();

    audio::Decoder Sound;
    // Latest accepted artifact; the callback may still be finishing its predecessor.
    std::shared_ptr<Artifact> Current;
    // Handed to the host and not yet collected, so still read by the audio thread.
    std::vector<std::shared_ptr<Artifact>> Retiring;
    audio::Host Host; // destroyed first, stopping callbacks before their artifacts
};

} // namespace faustlens::app
