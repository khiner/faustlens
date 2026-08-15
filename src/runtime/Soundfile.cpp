#include "runtime/Soundfile.h"

#include <algorithm>

namespace faustlens {

std::unique_ptr<Soundfile> LoadSoundfile(const SoundfileDesc &d, SoundfileReader *reader, uint32_t &unresolved) {
    unresolved = 0;

    // Decode first: the widest part decides how many buffers to allocate.
    struct Part {
        std::vector<std::vector<double>> Channels;
        int32_t Length = Soundfile::EmptyFrames, Rate = Soundfile::EmptyRate;
    };
    std::vector<Part> parts;
    size_t widest = 1;
    for (const std::string &url : d.Urls) {
        if (parts.size() >= Soundfile::Parts) break;
        Part p;
        const uint32_t part = uint32_t(parts.size());
        if (reader && reader->Read(url, part, p.Channels, p.Rate) && !p.Channels.empty()) {
            p.Length = int32_t(p.Channels[0].size());
            widest = std::max(widest, p.Channels.size());
        } else {
            ++unresolved;
            p.Channels.clear();
            p.Length = Soundfile::EmptyFrames;
            p.Rate = Soundfile::EmptyRate;
        }
        parts.push_back(std::move(p));
    }

    auto sf = std::make_unique<Soundfile>();
    int64_t total = 0;
    for (const Part &p : parts) total += p.Length;
    total += int64_t(Soundfile::Parts - parts.size()) * Soundfile::EmptyFrames;

    sf->Owned.assign(widest, std::vector<double>(size_t(total), 0.0));

    int32_t offset = 0;
    for (uint32_t part = 0; part < Soundfile::Parts; ++part) {
        const Part *p = part < parts.size() ? &parts[part] : nullptr;
        sf->Length[part] = p ? p->Length : Soundfile::EmptyFrames;
        sf->Rate[part] = p ? p->Rate : Soundfile::EmptyRate;
        sf->Offset[part] = offset;
        if (p)
            for (size_t c = 0; c < p->Channels.size(); ++c) std::ranges::copy(p->Channels[c], sf->Owned[c].begin() + offset);
        offset += sf->Length[part];
    }

    sf->Channel.resize(std::max<uint32_t>(d.Channels, 1));
    for (size_t c = 0; c < sf->Channel.size(); ++c) sf->Channel[c] = sf->Owned[c % widest].data();
    return sf;
}

} // namespace faustlens
