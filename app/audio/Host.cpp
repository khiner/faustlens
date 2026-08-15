#include "audio/Host.h"

#include "runtime/Interp.h"

#include "miniaudio.h"

#include <algorithm>
#include <format>

#if defined(__aarch64__) || defined(_M_ARM64)
#elif defined(__x86_64__) || defined(_M_X64)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

namespace faustlens::audio {

void Deinterleave(const float *in, int32_t in_channels, int32_t frames, double *const *out, int32_t out_channels) {
    for (int32_t c = 0; c < out_channels; ++c) {
        double *dst = out[c];
        if (!in || c >= in_channels) {
            std::fill(dst, dst + frames, 0.0);
            continue;
        }
        for (int32_t f = 0; f < frames; ++f) dst[f] = in[size_t(f) * in_channels + c];
    }
}

void Interleave(const double *const *in, int32_t in_channels, int32_t frames, float *out, int32_t out_channels) {
    for (int32_t c = 0; c < out_channels; ++c) {
        if (c >= in_channels) {
            for (int32_t f = 0; f < frames; ++f) out[size_t(f) * out_channels + c] = 0.0f;
            continue;
        }
        const double *src = in[c];
        for (int32_t f = 0; f < frames; ++f) out[size_t(f) * out_channels + c] = float(src[f]);
    }
}

void Crossfade(const double *const *from, int32_t from_channels, double *const *to, int32_t to_channels, int32_t frames, int64_t done, int64_t length) {
    if (length <= 0) return;
    for (int32_t c = 0; c < to_channels; ++c) {
        double *dst = to[c];
        const double *src = c < from_channels ? from[c] : nullptr;
        for (int32_t f = 0; f < frames; ++f) {
            const int64_t at = done + f;
            const double t = at >= length ? 1.0 : double(at) / double(length);
            const double old = src ? src[f] : 0.0;
            // `old + t * (new - old)`, not `t * new + (1 - t) * old`: where the instances
            // agree the difference is exactly zero.
            dst[f] = old + t * (dst[f] - old);
        }
    }
}

void EnableFlushToZero() {
#if defined(__aarch64__) || defined(_M_ARM64)
    // FPCR's FZ flushes denormals in and out, so there is no separate DAZ.
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));
#elif defined(__x86_64__) || defined(_M_X64)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

struct Host::Device {
    ma_device Device{};
    bool Open = false;

    static void Data(ma_device *d, void *out, const void *in, ma_uint32 frames) {
        static_cast<Host *>(d->pUserData)->Process(static_cast<const float *>(in), static_cast<float *>(out), frames);
    }
};

Host::Host() = default;

Host::~Host() { Stop(); }

std::unique_ptr<Host::Voice> Host::MakeVoice(Interp &dsp) const {
    auto v = std::make_unique<Voice>();
    v->Dsp = &dsp;
    v->InBuf.assign(std::max(dsp.Inputs(), 0), std::vector<double>(Chunk, 0.0));
    v->OutBuf.assign(std::max(dsp.Outputs(), 0), std::vector<double>(Chunk, 0.0));
    for (std::vector<double> &c : v->InBuf) v->InAt.push_back(c.data());
    for (std::vector<double> &c : v->OutBuf) v->OutAt.push_back(c.data());
    return v;
}

// Lock-free from any thread.
bool Host::Retire(Voice *v) {
    for (std::atomic<Voice *> &slot : Retired) {
        Voice *empty = nullptr;
        if (slot.compare_exchange_strong(empty, v, std::memory_order_release, std::memory_order_relaxed)) return true;
    }
    return false;
}

std::vector<Interp *> Host::Collect() {
    std::vector<Interp *> out;
    for (std::atomic<Voice *> &slot : Retired) {
        Voice const *v = slot.exchange(nullptr, std::memory_order_acquire);
        if (!v) continue;
        out.push_back(v->Dsp);
        delete v;
    }
    return out;
}

void Host::Process(const float *in, float *out, uint32_t frames) {
    // On *this* thread: a start notification fires on whichever thread started it.
    static thread_local bool ftz = false;
    if (!ftz) {
        ftz = true;
        EnableFlushToZero();
    }
    if (!out) return;

    if (Voice *next = Incoming.exchange(nullptr, std::memory_order_acquire)) {
        if (Fading && !Retire(Fading)) {
            // `Swap` guaranteed room, so this is unreachable. Dropping it leaks.
            Incoming.store(next, std::memory_order_release);
        } else {
            Fading = Current;
            Current = next;
            FadeDone = 0;
            FadeLength = int64_t(SampleRate * FadeMilliseconds / 1000.0);
        }
    }

    if (!Current || Chunk == 0) {
        std::fill(out, out + size_t(frames) * DeviceOut, 0.0f);
        return;
    }

    for (uint32_t done = 0; done < frames;) {
        const int32_t n = std::min<int32_t>(Chunk, int32_t(frames - done));
        const float *in_at = in ? in + size_t(done) * DeviceIn : nullptr;

        Deinterleave(in_at, DeviceIn, n, Current->InAt.data(), Current->Dsp->Inputs());
        Current->Dsp->Compute(n, Current->InAt.data(), Current->OutAt.data());

        if (Fading) {
            Deinterleave(in_at, DeviceIn, n, Fading->InAt.data(), Fading->Dsp->Inputs());
            Fading->Dsp->Compute(n, Fading->InAt.data(), Fading->OutAt.data());
            Crossfade(Fading->OutAt.data(), Fading->Dsp->Outputs(), Current->OutAt.data(), Current->Dsp->Outputs(), n, FadeDone, FadeLength);
            FadeDone += n;
            if (FadeDone >= FadeLength && Retire(Fading)) Fading = nullptr;
        }

        Interleave(Current->OutAt.data(), Current->Dsp->Outputs(), n, out + size_t(done) * DeviceOut, DeviceOut);
        done += uint32_t(n);
    }
}

bool Host::Swap(Interp &next) {
    if (!Running) return false;
    // Counted before publishing, so the audio thread's retires cannot fail: an untaken
    // hand-off, the one fading, and the one replaced.
    size_t free_slots = 0;
    for (const std::atomic<Voice *> &slot : Retired) free_slots += slot.load(std::memory_order_relaxed) == nullptr ? 1 : 0;
    if (free_slots < 3) return false;

    std::unique_ptr<Voice> v = MakeVoice(next);
    if (Voice *stale = Incoming.exchange(v.release(), std::memory_order_acq_rel))
        if (!Retire(stale)) delete stale;
    return true;
}

std::expected<void, std::string> Host::Start(Interp &dsp) {
    Stop();
    Warning.clear();

    auto configure = [&](bool with_capture) {
        ma_device_config cfg = ma_device_config_init(with_capture ? ma_device_type_duplex : ma_device_type_playback);
        // Zero channels and rate ask for the device's own, converting format only.
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = 0;
        cfg.capture.format = ma_format_f32;
        cfg.capture.channels = 0;
        cfg.sampleRate = 0;
        cfg.dataCallback = &Device::Data;
        cfg.pUserData = this;
        return cfg;
    };

    Device = std::make_unique<struct Device>();
    const bool want_capture = dsp.Inputs() > 0;
    ma_device_config cfg = configure(want_capture);
    ma_result rc = ma_device_init(nullptr, &cfg, &Device->Device);
    if (rc != MA_SUCCESS && want_capture) {
        Warning = "no capture device; inputs are silence";
        cfg = configure(false);
        rc = ma_device_init(nullptr, &cfg, &Device->Device);
    }
    if (rc != MA_SUCCESS) {
        Device.reset();
        return std::unexpected(std::format("could not open an audio device: {}", ma_result_description(rc)));
    }
    Device->Open = true;

    DeviceOut = int32_t(Device->Device.playback.channels);
    DeviceIn = Device->Device.type == ma_device_type_duplex ? int32_t(Device->Device.capture.channels) : 0;
    SampleRate = Device->Device.sampleRate;
    DeviceName = Device->Device.playback.name;

    Chunk = std::max<int32_t>(1024, int32_t(Device->Device.playback.internalPeriodSizeInFrames));

    // Before the device runs, so the first callback meets a live instance.
    dsp.Init(SampleRate);
    Current = MakeVoice(dsp).release();

    rc = ma_device_start(&Device->Device);
    if (rc != MA_SUCCESS) {
        delete Current;
        Current = nullptr;
        ma_device_uninit(&Device->Device);
        Device.reset();
        return std::unexpected(std::format("could not start the audio device: {}", ma_result_description(rc)));
    }
    Running = true;
    return {};
}

void Host::Stop() {
    if (Device && Device->Open) { ma_device_uninit(&Device->Device); }
    Device.reset();
    Running = false;
    for (Voice **v : {&Current, &Fading})
        if (*v) {
            if (!Retire(*v)) delete *v;
            *v = nullptr;
        }
    if (Voice *pending = Incoming.exchange(nullptr, std::memory_order_acq_rel))
        if (!Retire(pending)) delete pending;
}

} // namespace faustlens::audio
