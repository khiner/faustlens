// Cache successful and failed decodes by URL across instances.
#pragma once

#include "runtime/Soundfile.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace faustlens::audio {

struct Decoder : SoundfileReader {
    struct Entry {
        std::vector<std::vector<double>> Channels;
        int32_t Rate = 0;
        bool Ok = false;
    };

    std::vector<std::filesystem::path> Search;
    std::map<std::string, Entry> Cache;

    // Search directories for relative URLs after trying the URL as written.
    void AddSearchPath(std::filesystem::path);

    bool Read(const std::string &url, uint32_t part, std::vector<std::vector<double>> &channels, int32_t &rate) override;

    size_t CachedFrames() const;

    const Entry &Decode(const std::string &url);
};

} // namespace faustlens::audio
