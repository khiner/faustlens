#include "audio/Decode.h"

#include "miniaudio.h"

#include <algorithm>

namespace faustlens::audio {

namespace {

constexpr ma_uint64 Chunk = 4096;

} // namespace

void Decoder::AddSearchPath(std::filesystem::path p) { Search.push_back(std::move(p)); }

size_t Decoder::CachedFrames() const {
    size_t n = 0;
    for (const auto &[url, e] : Cache)
        if (e.Ok && !e.Channels.empty()) n += e.Channels[0].size();
    return n;
}

const Decoder::Entry &Decoder::Decode(const std::string &url) {
    const auto it = Cache.find(url);
    if (it != Cache.end()) return it->second;
    Entry &e = Cache[url];

    std::vector<std::filesystem::path> tries{url};
    for (const std::filesystem::path &dir : Search) tries.push_back(dir / url);

    ma_decoder dec{};
    // Zeros ask for the file's native channels and rate, not a resample.
    ma_decoder_config const cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    bool open = false;
    for (const std::filesystem::path &p : tries) {
        if (ma_decoder_init_file(p.string().c_str(), &cfg, &dec) == MA_SUCCESS) {
            open = true;
            break;
        }
    }
    if (!open) return e;

    const int32_t channels = int32_t(dec.outputChannels);
    e.Rate = int32_t(dec.outputSampleRate);
    e.Channels.assign(std::max(channels, 1), std::vector<double>{});

    // To the end, not the reported length: an mp3's frame count is an estimate.
    std::vector<float> buf(Chunk * std::max(channels, 1));
    for (;;) {
        ma_uint64 got = 0;
        if (ma_decoder_read_pcm_frames(&dec, buf.data(), Chunk, &got) != MA_SUCCESS) break;
        for (ma_uint64 f = 0; f < got; ++f)
            for (int32_t c = 0; c < channels; ++c) e.Channels[c].push_back(buf[f * channels + c]);
        if (got < Chunk) break;
    }
    ma_decoder_uninit(&dec);

    e.Ok = !e.Channels.empty() && !e.Channels[0].empty();
    return e;
}

bool Decoder::Read(const std::string &url, uint32_t, std::vector<std::vector<double>> &channels, int32_t &rate) {
    const Entry &e = Decode(url);
    if (!e.Ok) return false;
    channels = e.Channels;
    rate = e.Rate;
    return true;
}

} // namespace faustlens::audio
