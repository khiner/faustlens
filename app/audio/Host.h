// The audio callback performs no allocation or deallocation.
#pragma once

#include "runtime/Migrate.h"
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace faustlens {

struct Instance;

namespace audio {

// Zero missing source channels and discard excess channels.
void Deinterleave(const float *in, int32_t in_channels, int32_t frames, double *const *out, int32_t out_channels);
void Interleave(const double *const *in, int32_t in_channels, int32_t frames, float *out, int32_t out_channels);

// Crossfade into `to` starting at frame `done` of the `length`-frame transition.
void Crossfade(const double *const *from, int32_t from_channels, double *const *to, int32_t to_channels, int32_t frames, int64_t done, int64_t length);

// Enable FTZ and DAZ on the calling thread.
void EnableFlushToZero();

struct Host {
    struct Device;

    struct Voice {
        Instance *Dsp = nullptr;
        std::vector<std::vector<double>> InBuf, OutBuf;
        std::vector<double *> InAt, OutAt;
        const Instance *From = nullptr;
        StateTransfer Transfer;
    };

    static constexpr double FadeMilliseconds = 5.0;
    // Reserve space for all voices displaced by one swap plus one pending retirement.
    static constexpr size_t RetiredSlots = 4;

    std::unique_ptr<Device> Device;
    // Audio thread only while running.
    Voice *Current = nullptr, *Fading = nullptr;
    int64_t FadeDone = 0, FadeLength = 0;
    std::atomic<Voice *> Incoming{nullptr};
    std::atomic<Voice *> Retired[RetiredSlots] = {};
    int32_t Chunk = 0, DeviceIn = 0, DeviceOut = 0;
    double SampleRate = 0;
    bool Running = false;
    std::string DeviceName;
    std::string Warning;

    Host();
    ~Host();
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    // Retain the instance until Stop completes or Collect returns it.
    // Capture failures produce silent inputs and a warning.
    std::expected<void, std::string> Start(Instance &dsp);
    void Stop();

    // Publish only while running with no pending swap and sufficient retirement capacity.
    // Retain instance ownership until Collect returns it.
    bool Swap(Instance &next, const Instance *from = nullptr, const StateTransfer & = {});

    // Collect retired instances off the audio thread before destroying them or their Plans.
    std::vector<Instance *> Collect();

    void Process(const float *in, float *out, uint32_t frames);
    std::unique_ptr<Voice> MakeVoice(Instance &) const;
    bool Retire(Voice *);
};

} // namespace audio
} // namespace faustlens
