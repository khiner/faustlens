#pragma once

#include "property/Corpus.h"
#include "query/Query.h"
#include "signal/Plan.h"

#include "doctest.h"

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
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

struct Census {
    std::map<std::string, int> Counts;
    std::map<std::string, std::string> Examples;

    void Add(const std::string &reason, std::string witness = {}) {
        ++Counts[reason];
        if (!witness.empty() && !Examples.contains(reason)) Examples[reason] = std::move(witness);
    }
    void Report() const {
        for (const auto &[reason, n] : Counts) MESSAGE("  ", n, "x ", reason);
        for (const auto &[reason, e] : Examples) MESSAGE("  e.g. ", e);
    }
};

// Return empty for missing or invalid dumps, counting parse failures as divergences.
template<class File> using Parse = std::expected<File, std::string> (*)(std::string_view);

template<class File> std::optional<File> ReadDump(const std::filesystem::path &dump, Parse<File> parse, Census &census, int &differed) {
    if (!std::filesystem::is_regular_file(dump)) return std::nullopt;
    auto out = parse(ReadText(dump));
    if (out) return *std::move(out);
    ++differed;
    census.Add("the dump did not parse", std::move(out).error());
    return std::nullopt;
}

template<class File, class Body>
void ForEachDump(const char *suffix, Parse<File> parse, Census &census, int &differed, Body body, bool add_normal_form = true) {
    for (const std::filesystem::path &p : DspPaths()) {
        const std::string name = p.stem().string();
        const std::optional<File> theirs = ReadDump<File>(OracleDir() / (name + suffix), parse, census, differed);
        if (!theirs) continue;
        Program prog(p, {}, add_normal_form);
        if (!prog.Ok) {
            ++differed;
            census.Add("did not evaluate");
            continue;
        }
        body(name, *theirs, prog);
    }
}

template<class C> std::string Join(const C &items, const char *sep = " ") {
    std::string out;
    for (const auto &item : items) out += std::format("{}{}", out.empty() ? "" : sep, item);
    return out;
}

inline std::string JoinCounts(const std::map<std::string, int> &counts, const char *sep = " ") {
    std::string out;
    for (const auto &[k, n] : counts) out += std::format("{}{}×{}", out.empty() ? "" : sep, k, n);
    return out;
}

// Require both expected and observed names to match.
inline void PinVocabulary(const char *what, const std::set<std::string> &seen, const std::set<std::string> &known) {
    INFO(what);
    std::vector<std::string> unknown;
    for (const std::string &s : seen)
        if (!known.contains(s)) unknown.push_back(s);
    for (const std::string &s : unknown) MESSAGE("unpinned ", what, ": ", s);
    CHECK(unknown.empty());
    CHECK(seen.size() == known.size());
}

// Record association-order differences and whether they occur within feedback.
struct Deferred {
    const char *Name;
    bool Feedback;
};
inline constexpr Deferred AssociationOrder[] = {
    {"bells", false},
    {"carre_volterra", true},
    {"cubic_distortion", true},
    {"freeverb", true},
    {"gate_compressor", true},
    {"grain3", false},
    {"mixer", true},
    {"modulations", false},
    {"osc", false},
    {"osci", false},
    {"parametric_eq", true},
    {"phaser_flanger", true},
    {"smoothdelay", true},
    {"spectral_tilt", true},
    {"tester", false},
    {"tester2", false},
    {"thru_zero_flanger", false},
    {"vcf_wah_pedals", true},
    {"virtual_analog_oscillators", true},
    {"zita_rev1", false},
};

inline const Deferred *Pinned(const std::string &name) {
    for (const Deferred &d : AssociationOrder)
        if (name == d.Name) return &d;
    return nullptr;
}

} // namespace faustlens::test
