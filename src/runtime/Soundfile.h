// Soundfile storage follows the reference runtime layout.
#pragma once

#include "signal/Plan.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace faustlens {

struct Soundfile {
    static constexpr uint32_t Parts = 256;
    static constexpr int32_t EmptyFrames = 1024, EmptyRate = 44100;

    std::vector<std::vector<double>> Owned;
    // Alias additional channels as c % owned.size().
    std::vector<const double *> Channel;
    std::array<int32_t, Parts> Length{}, Rate{}, Offset{};
};

struct SoundfileReader {
    virtual ~SoundfileReader() = default;
    // Return false to substitute silence for a part.
    virtual bool Read(const std::string &url, uint32_t part, std::vector<std::vector<double>> &channels, int32_t &rate) = 0;
};

// Return a non-null soundfile, using silent defaults when decoding fails.
std::unique_ptr<Soundfile> LoadSoundfile(const SoundfileDesc &, SoundfileReader *, uint32_t &unresolved);

} // namespace faustlens
