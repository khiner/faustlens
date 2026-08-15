// Compile, migrate, hand off. No artifact means no swap, and the last good one plays on.
#pragma once

#include "audio/Decode.h"
#include "audio/Host.h"
#include "controls/Store.h"
#include "query/Query.h"
#include "query/Snapshot.h"
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

// One offset per field in field order: the earliest byte in `f` it came out of, or
// `Nowhere`. Valid while the Plan's compile is.
std::vector<uint32_t> FieldOffsets(const Plan &, const FileView *f);

struct Live {
    // The edit-to-audio profile, in ms, measured on the thread that reloaded.
    struct Timings {
        double Parse = 0, Evaluate = 0, Propagate = 0, Lower = 0, Artifact = 0;
        double Instance = 0, Init = 0, Migrate = 0, Total = 0;
        // Letting the previous artifact go, which the audio thread may not do.
        double Release = 0;
    };

    struct Result {
        bool Compiled = false;
        bool Swapped = false;
        bool Unchanged = false; // the same Plan, so no swap
        std::string Why; // why not: the compile failed, or the host refused the swap
        Migration Migration;
        Timings Timings;
    };

    // Compiles `process` of `path`, migrating running state into the new
    // instance. Does not open a device: the caller starts the host.
    Result Reload(Session &, const std::string &path, const controls::Values &controls = {});

    // Call from the `Reload` thread once a frame, or a swap is refused for room.
    void Collect();

    audio::Decoder Sound;
    audio::Host Host;
    // The artifact being *heard*, which after a failed compile is not the one seen.
    std::unique_ptr<Artifact> Current;
    // Handed to the host and not yet collected, so still read by the audio thread.
    std::vector<std::unique_ptr<Artifact>> Retiring;
};

} // namespace faustlens::app
