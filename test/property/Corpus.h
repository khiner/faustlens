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

// Empty where unreadable. Not `ReadFile`, so a test with both namespaces open can call it.
std::string ReadText(const std::filesystem::path &);

inline std::string_view Trim(std::string_view s) {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && blank(s.front())) s.remove_prefix(1);
    while (!s.empty() && blank(s.back())) s.remove_suffix(1);
    return s;
}

// Each `\n`-separated line untrimmed, a trailing empty chunk included. Stops where `body` does.
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
    std::string Relative; // relative to lib/faust
    std::string Text;
};

// 341 files, of which reference Faust accepts 319.
std::vector<CorpusFile> TestsCorpus();
std::vector<CorpusFile> ExamplesCorpus();
std::vector<CorpusFile> LibrariesCorpus();

std::vector<CorpusFile> WholeCorpus();

// The 94 programs every sweep that needs a *compiling* corpus runs over.
inline std::vector<std::filesystem::path> DspPaths() { return PathsIn(ImpulseDir() / "dsp", ".dsp"); }

// The files reference Faust 2.85.9 rejects: an accept means the parser has drifted.
bool IsPinnedRejection(const std::string &relative);

// `bug-wall.dsp`: generated, 9.5 MB, 687k commas, and quadratic for a per-ref sweep.
bool IsGeneratedOutlier(const std::string &relative);

// `pthread`s for the stack size: `std::thread` cannot portably beat macOS's 512 KB.
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
