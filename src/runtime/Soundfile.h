// What a `soundfile` reads at run time, and the host interface that fills one. The
// layout is pinned to the reference runtime's.
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

    std::vector<std::vector<double>> Owned; // one concatenated buffer per decoded channel
    // Past `owned.size()` these alias `c % owned.size()` rather than read as silence.
    std::vector<const double *> Channel;
    std::array<int32_t, Parts> Length{}, Rate{}, Offset{};
};

// The host's decoder. The compiler never opens a file itself.
struct SoundfileReader {
    virtual ~SoundfileReader() = default;
    // Returning false leaves the part silent and is not an error.
    virtual bool Read(const std::string &url, uint32_t part, std::vector<std::vector<double>> &channels, int32_t &rate) = 0;
};

// Never null: with nothing decodable it is the all-silent `defaultsound`.
std::unique_ptr<Soundfile> LoadSoundfile(const SoundfileDesc &, SoundfileReader *, uint32_t &unresolved);

} // namespace faustlens
