#include "property/Corpus.h"

#include "files/Vfs.h"

#include <algorithm>
#include <pthread.h>
#include <set>

namespace faustlens::test {
namespace {

namespace fs = std::filesystem;

std::vector<CorpusFile> Collect(const fs::path &root, std::string_view extension) {
    std::vector<CorpusFile> out;
    if (!fs::is_directory(root)) return out;
    const fs::path base = FAUSTLENS_FAUST_DIR;
    for (const auto &e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || e.path().extension() != extension) continue;
        out.push_back({e.path(), fs::relative(e.path(), base).string(), ReadText(e.path())});
    }
    std::ranges::sort(out, [](const CorpusFile &a, const CorpusFile &b) { return a.Relative < b.Relative; });
    return out;
}

// Twenty-one use multirate syntax reference Faust has no grammar for. The other, `error24.dsp`,
// is `process = :`.
const std::set<std::string> &PinnedRejections() {
    static const std::set<std::string> Set = {
        "tests/error-tests/error24.dsp",
        "tests/multirate-error-tests/multirate_2.dsp",
        "tests/multirate-error-tests/multirate_3.dsp",
        "tests/multirate-error-tests/multirate_6.dsp",
        "tests/multirate-error-tests/rateconflict_1.dsp",
        "tests/multirate-error-tests/rateconflict_2.dsp",
        "tests/pass-tests/dimensioncheck_2.dsp",
        "tests/pass-tests/dimensioncheck_3.dsp",
        "tests/pass-tests/ratepass_1.dsp",
        "tests/pass-tests/ratepass_2.dsp",
        "tests/pass-tests/ratepass_3.dsp",
        "tests/pass-tests/ratepass_4.dsp",
        "tests/pass-tests/ratepass_5.dsp",
        "tests/pass-tests/ratepass_6.dsp",
        "tests/pass-tests/ratepass_7.dsp",
        "tests/pass-tests/ratepass_8.dsp",
        "tests/pass-tests/ratepass_9.dsp",
        "tests/pass-tests/ratepass_10.dsp",
        "tests/pass-tests/ratepass_11.dsp",
        "tests/pass-tests/ratepass_12.dsp",
        "tests/pending-tests/mr_oversample.dsp",
        "tests/pending-tests/mr_undersample.dsp",
    }; // 22 files, leaving 319 accepted
    return Set;
}

} // namespace

fs::path ImpulseDir() { return fs::path(FAUSTLENS_FAUST_DIR) / "tests/impulse-tests"; }

std::vector<fs::path> PathsIn(const fs::path &dir, const char *extension) {
    std::vector<fs::path> out;
    for (const auto &e : fs::directory_iterator(dir))
        if (e.path().extension() == extension) out.push_back(e.path());
    std::ranges::sort(out);
    return out;
}

std::string ReadText(const fs::path &p) { return faustlens::ReadFile(p).value_or(std::string()); }

std::vector<CorpusFile> TestsCorpus() { return Collect(fs::path(FAUSTLENS_FAUST_DIR) / "tests", ".dsp"); }
std::vector<CorpusFile> ExamplesCorpus() { return Collect(fs::path(FAUSTLENS_FAUST_DIR) / "examples", ".dsp"); }
std::vector<CorpusFile> LibrariesCorpus() { return Collect(fs::path(FAUSTLENS_FAUST_DIR) / "libraries", ".lib"); }

std::vector<CorpusFile> WholeCorpus() {
    std::vector<CorpusFile> out = TestsCorpus();
    for (auto &c : ExamplesCorpus()) out.push_back(std::move(c));
    for (auto &c : LibrariesCorpus()) out.push_back(std::move(c));
    return out;
}

bool IsPinnedRejection(const std::string &relative) { return PinnedRejections().contains(relative); }

// 64 MB stacks: the 2048-frame recursion bound overflows 8 MB under AddressSanitizer.
void RunPool(unsigned threads, const std::function<void()> &worker) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    std::vector<pthread_t> pool(threads);
    const auto trampoline = [](void *arg) -> void * {
        (*static_cast<const std::function<void()> *>(arg))();
        return nullptr;
    };
    for (pthread_t &t : pool) pthread_create(&t, &attr, trampoline, const_cast<std::function<void()> *>(&worker));
    for (pthread_t t : pool) pthread_join(t, nullptr);
    pthread_attr_destroy(&attr);
}

bool IsGeneratedOutlier(const std::string &relative) { return relative == "tests/codegen-tests/bug-wall.dsp"; }

} // namespace faustlens::test
