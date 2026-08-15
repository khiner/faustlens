// The audio device and the hand-off between instances. The callback neither
// allocates nor frees.
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace faustlens {

struct Interp;

namespace audio {

// A channel the source lacks is zeroed, and one the destination lacks is dropped.
void Deinterleave(const float *in, int32_t in_channels, int32_t frames, double *const *out, int32_t out_channels);
void Interleave(const double *const *in, int32_t in_channels, int32_t frames, float *out, int32_t out_channels);

// In place: `to` leaves holding the mix. `done` and `length` are frames.
void Crossfade(const double *const *from, int32_t from_channels, double *const *to, int32_t to_channels, int32_t frames, int64_t done, int64_t length);

// Sets FTZ and DAZ on the calling thread: they are per-thread bits.
void EnableFlushToZero();

struct Host {
    struct Device; // miniaudio's, kept out of this header

    // Buffers allocated by `Swap`'s caller, since nothing resizes under audio.
    struct Voice {
        Interp *Dsp = nullptr;
        std::vector<std::vector<double>> InBuf, OutBuf;
        std::vector<double *> InAt, OutAt;
    };

    static constexpr double FadeMilliseconds = 5.0;
    // What one swap can displace, plus one, so a retire always has a slot.
    static constexpr size_t RetiredSlots = 4;

    std::unique_ptr<Device> Device;
    // Audio thread only, once running.
    Voice *Current = nullptr, *Fading = nullptr;
    int64_t FadeDone = 0, FadeLength = 0;
    std::atomic<Voice *> Incoming{nullptr};
    std::atomic<Voice *> Retired[RetiredSlots] = {};
    int32_t Chunk = 0, DeviceIn = 0, DeviceOut = 0;
    double SampleRate = 0;
    bool Running = false;
    std::string DeviceName;
    // A failed capture open, which `Start` survives: inputs are silence.
    std::string Warning;

    Host();
    ~Host();
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    // `dsp` must outlive this. A failed capture open falls back to silent inputs
    // and is a `Warning`, not an error.
    std::expected<void, std::string> Start(Interp &dsp);
    void Stop();

    // The caller keeps ownership until `Collect` returns the instance. False
    // where there is no room: backpressure.
    bool Swap(Interp &next);

    // Instances the audio thread has finished with, freed off it. Call from any
    // other thread, and before their Plans go.
    std::vector<Interp *> Collect();

    void Process(const float *in, float *out, uint32_t frames);
    std::unique_ptr<Voice> MakeVoice(Interp &) const;
    bool Retire(Voice *);
};

} // namespace audio
} // namespace faustlens
