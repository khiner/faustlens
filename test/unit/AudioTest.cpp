// The audio host at the parts a real device would hide: channel map, crossfade, decoder cache.
#include "audio/Decode.h"
#include "audio/Host.h"

#include "doctest.h"

#include <cmath>
#include <filesystem>
#include <limits>
#include <thread>
#include <vector>

using namespace faustlens;
using namespace faustlens::audio;

namespace {

std::vector<double *> At(std::vector<std::vector<double>> &v) {
    std::vector<double *> out;
    out.reserve(v.size());
    for (std::vector<double> &c : v) out.push_back(c.data());
    return out;
}

} // namespace

TEST_CASE("the device's channels map onto the program's positionally") {
    // Two frames of a three-channel device: 1 2 3, 4 5 6.
    const std::vector<float> device{1, 2, 3, 4, 5, 6};

    SUBCASE("channel for channel") {
        std::vector<std::vector<double>> dsp(3, std::vector<double>(2, -1));
        Deinterleave(device.data(), 3, 2, At(dsp).data(), 3);
        CHECK(dsp[0] == std::vector<double>{1, 4});
        CHECK(dsp[1] == std::vector<double>{2, 5});
        CHECK(dsp[2] == std::vector<double>{3, 6});
    }
    SUBCASE("a surplus device channel is ignored") {
        std::vector<std::vector<double>> dsp(2, std::vector<double>(2, -1));
        Deinterleave(device.data(), 3, 2, At(dsp).data(), 2);
        CHECK(dsp[0] == std::vector<double>{1, 4});
        CHECK(dsp[1] == std::vector<double>{2, 5});
    }
    SUBCASE("a channel the device does not have is silence, not stale") {
        std::vector<std::vector<double>> dsp(4, std::vector<double>(2, -1));
        Deinterleave(device.data(), 3, 2, At(dsp).data(), 4);
        CHECK(dsp[3] == std::vector<double>{0, 0});
    }
    SUBCASE("no capture device at all is silence on every input") {
        std::vector<std::vector<double>> dsp(2, std::vector<double>(2, -1));
        Deinterleave(nullptr, 0, 2, At(dsp).data(), 2);
        CHECK(dsp[0] == std::vector<double>{0, 0});
        CHECK(dsp[1] == std::vector<double>{0, 0});
    }
}

TEST_CASE("the program's channels map onto the device's positionally") {
    std::vector<std::vector<double>> dsp{{1, 4}, {2, 5}, {3, 6}};
    const std::vector<const double *> at{dsp[0].data(), dsp[1].data(), dsp[2].data()};

    SUBCASE("a surplus output is dropped") {
        std::vector<float> device(4, -1);
        Interleave(at.data(), 3, 2, device.data(), 2);
        CHECK(device == std::vector<float>{1, 2, 4, 5});
    }
    SUBCASE("a device channel the program does not fill is silenced") {
        std::vector<float> device(8, -1);
        Interleave(at.data(), 3, 2, device.data(), 4);
        CHECK(device == std::vector<float>{1, 2, 3, 0, 4, 5, 6, 0});
    }
}

TEST_CASE("the crossfade weights sum to one, so two equal instances sum to that signal") {
    // Exactness needs `old + t * (new - old)`. `t * new + (1 - t) * old` is not exact.
    std::vector<double> a{0.1, -0.3, 1e-9, 0.7, -1.0 / 3.0, 12345.678};
    std::vector<double> b = a;
    const double *const from[] = {a.data()};
    double *const to[] = {b.data()};
    const int32_t n = int32_t(a.size());
    for (int64_t done = 0; done <= 8; ++done) {
        b = a;
        Crossfade(from, 1, to, 1, n, done, 8);
        CHECK(b == a);
    }
}

TEST_CASE("the crossfade is linear, and a new channel comes up from silence") {
    std::vector<double> old_(4, 1.0);
    std::vector<double> next(4, 0.0), fresh(4, 1.0);
    const double *const from[] = {old_.data()};
    double *const to[] = {next.data(), fresh.data()};
    // Four frames of an eight-frame fade, incoming at 0, so the mix walks down.
    Crossfade(from, 1, to, 2, 4, 0, 8);
    CHECK(next[0] == doctest::Approx(1.0));
    CHECK(next[1] == doctest::Approx(0.875));
    CHECK(next[2] == doctest::Approx(0.75));
    CHECK(next[3] == doctest::Approx(0.625));
    CHECK(fresh[0] == doctest::Approx(0.0));
    CHECK(fresh[3] == doctest::Approx(0.375));

    // Past the end the incoming instance is the output, so the counter may overrun.
    std::vector<double> done_(2, 5.0);
    std::vector<double> keep(2, 1.0);
    const double *const late_from[] = {done_.data()};
    double *const late_to[] = {keep.data()};
    Crossfade(late_from, 1, late_to, 1, 2, 8, 8);
    CHECK(keep[0] == doctest::Approx(1.0));
}

TEST_CASE("flush-to-zero is a per-thread setting, and it takes") {
    // The multiply is denormal. Another thread, so the conformance oracle never runs under FTZ.
    volatile double const denormal = std::numeric_limits<double>::min() / 4;
    volatile double const here = denormal * 0.5;
    CHECK(here != 0.0);

    double there = -1;
    std::thread t([&] {
        EnableFlushToZero();
        volatile double const d = std::numeric_limits<double>::min() / 4;
        there = d * 0.5;
    });
    t.join();
    CHECK(there == 0.0);

    volatile double const after = denormal * 0.5;
    CHECK(after != 0.0);
}

TEST_CASE("the decoder reads a file once and caches it by URL across recompiles") {
    // miniaudio's own test asset, so the shape is known without writing a file first.
    const std::filesystem::path flac = std::filesystem::path(FAUSTLENS_MINIAUDIO_DIR) / "data/16-44100-stereo.flac";
    REQUIRE(std::filesystem::exists(flac));

    Decoder d;
    std::vector<std::vector<double>> channels;
    int32_t rate = 0;
    REQUIRE(d.Read(flac.string(), 0, channels, rate));
    CHECK(rate == 44100);
    CHECK(channels.size() == 2);
    CHECK(channels[0].size() > 0);
    CHECK(channels[0].size() == channels[1].size());
    // In range says the interleaved f32 came apart the right way.
    for (const double v : channels[0]) REQUIRE(std::fabs(v) <= 1.0);

    const size_t frames = d.CachedFrames();
    SUBCASE("the second read is the cache") {
        std::vector<std::vector<double>> again;
        CHECK(d.Read(flac.string(), 0, again, rate));
        CHECK(again == channels);
        CHECK(d.Cache.size() == 1);
        CHECK(d.CachedFrames() == frames);
    }
    SUBCASE("a URL that does not resolve is a false, and is remembered too") {
        std::vector<std::vector<double>> none;
        CHECK_FALSE(d.Read("no-such-file.wav", 0, none, rate));
        CHECK_FALSE(d.Read("no-such-file.wav", 0, none, rate));
        // The failure is cached too, so a missing file is not reopened once per recompile.
        CHECK(d.Cache.size() == 2);
        CHECK(d.CachedFrames() == frames);
    }
    SUBCASE("a search path is tried after the URL as written") {
        Decoder rel;
        std::vector<std::vector<double>> by_name;
        CHECK_FALSE(rel.Read("16-44100-stereo.flac", 0, by_name, rate));
        Decoder found;
        found.AddSearchPath(flac.parent_path());
        CHECK(found.Read("16-44100-stereo.flac", 0, by_name, rate));
        CHECK(by_name.size() == 2);
    }
}
