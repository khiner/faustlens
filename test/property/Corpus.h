#pragma once

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace faustlens::test {

std::filesystem::path ImpulseDir();

std::vector<std::filesystem::path> PathsIn(const std::filesystem::path &dir, const char *extension);

// Return empty for unreadable files.
std::string ReadText(const std::filesystem::path &);

inline std::string_view Trim(std::string_view s) {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && blank(s.front())) s.remove_prefix(1);
    while (!s.empty() && blank(s.back())) s.remove_suffix(1);
    return s;
}

// Visit untrimmed newline-separated segments, including a trailing empty segment, until body returns false.
template<class Body> bool ForEachLine(std::string_view text, Body body) {
    for (size_t at = 0; at <= text.size();) {
        const size_t nl = text.find('\n', at);
        const size_t end = nl == std::string_view::npos ? text.size() : nl;
        if (!body(text.substr(at, end - at))) return false;
        at = end + 1;
    }
    return true;
}

struct CorpusFile {
    std::filesystem::path Path;
    std::string Relative;
    std::string Text;
};

std::vector<CorpusFile> TestsCorpus();
std::vector<CorpusFile> ExamplesCorpus();
std::vector<CorpusFile> LibrariesCorpus();

std::vector<CorpusFile> WholeCorpus();

inline std::vector<std::filesystem::path> DspPaths() { return PathsIn(ImpulseDir() / "dsp", ".dsp"); }

// Return corpus files rejected by the pinned reference compiler.
bool IsPinnedRejection(const std::string &relative);

// Exclude generated bug-wall.dsp from quadratic per-ref sweeps.
bool IsGeneratedOutlier(const std::string &relative);

// Run with an explicit pthread stack size for deep sanitizer checks.
void RunPool(unsigned threads, const std::function<void()> &worker);

template<class R, class T, class Body> std::vector<R> MapEach(const std::vector<T> &items, Body body) {
    std::vector<R> out(items.size());
    std::atomic<size_t> next{0};
    const unsigned threads = std::max(1u, std::min<unsigned>(std::max(1u, std::thread::hardware_concurrency()), unsigned(items.size())));
    RunPool(threads, [&] {
        for (size_t i = next++; i < items.size(); i = next++) out[i] = body(items[i]);
    });
    return out;
}

} // namespace faustlens::test
