#pragma once

#include "property/Corpus.h"
#include "query/Query.h"
#include "signal/Plan.h"

#include "doctest.h"

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace faustlens::test {

inline std::filesystem::path OracleDir() {
    if (const char *env = std::getenv("FAUSTLENS_ORACLE_DIR")) return env;
    return std::filesystem::path(FAUSTLENS_BUILD_DIR) / "oracle";
}

// Construct both base storage objects before Graph references them.
struct ProgramState {
    Session Session;
    Signals Sigs;
};
struct Program : ProgramState, Graph {
    // Load the corpus file when source is empty; otherwise compile source under path.
    explicit Program(const std::filesystem::path &path, std::string source = {}, bool add_normal_form = true)
        : Graph(Session, Prepare(Session, path, std::move(source)), Sigs, add_normal_form) {}

    static std::string Prepare(faustlens::Session &s, const std::filesystem::path &path, std::string source) {
        if (source.empty()) {
            s.AddSearchPath(ImpulseDir() / "dsp");
            return std::filesystem::weakly_canonical(path).string();
        }
        s.SetBuffer(path.string(), std::move(source));
        return path.string();
    }
};

template<class File, class Body>
void ForEachDump(const char *suffix, std::expected<File, std::string> (*parse)(std::string_view), Body body, bool normalized = true) {
    const auto paths = DspPaths();
    REQUIRE(paths.size() == 94);
    for (const auto &path : paths) {
        const auto dump = OracleDir() / (path.stem().string() + suffix);
        INFO(dump.string());
        REQUIRE(std::filesystem::is_regular_file(dump));
        const auto reference = parse(ReadText(dump));
        REQUIRE_MESSAGE(reference, (reference ? "" : reference.error()));
        Program program(path, {}, normalized);
        REQUIRE(program.Ok);
        body(*reference, program);
    }
}

inline void CheckVocabulary(const char *label, const std::set<std::string> &seen, const std::set<std::string> &expected) {
    INFO(label);
    CHECK(seen == expected);
}

} // namespace faustlens::test
