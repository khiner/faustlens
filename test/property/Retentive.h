// Shared parts of the Retentiveness sweeps: an edit must retain the bytes of everything
// it did not change, comments and formatting included.
#pragma once

#include "property/Corpus.h"
#include "syntax/Parser.h"
#include "syntax/Splice.h"

#include "doctest.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace faustlens::test {

struct Loaded {
    Terms Terms;
    ParseResult R;
    std::optional<SpliceContext> Ctx;
    std::vector<RefId> Parent;

    explicit Loaded(const CorpusFile &);

    // Where a generated sweep may put a stage. `waveform`'s list takes numbers only.
    bool AcceptsExpression(RefId) const;
};

const char *BadScript(const EditScript &, const TermRef &target);

bool BytesSurvive(std::string_view src, uint32_t begin, uint32_t end, const EditScript &);

// `d` is owed *an* occurrence of its value inside the target, not *this* one.
bool Survives(const Loaded &, std::string_view src, const TermRef &d, const TermRef &target, const EditScript &);

// First off-path ref inside `target` whose bytes the splice lost, or `NoRef`. Only
// the rebuilt `path` and `dropped` may lose bytes.
RefId LostBytes(const Loaded &, std::string_view src, RefId target, std::span<const uint32_t> path, RefId dropped, const EditScript &);

// The confirming reparse is O(file), so it runs on a bounded number of refs per file.
constexpr size_t ReparseBudget = 40;

struct Sweep {
    std::vector<std::string> Failures;
    size_t A = 0, B = 0;
    bool Skipped = false;
};

bool Skipped(const CorpusFile &, Sweep &);

// Comments in a replaced region must be re-emitted. `seen` counts those it passed over.
bool CommentsSalvaged(const Loaded &, const CorpusFile &, const EditScript &, size_t &seen);

struct Merged {
    std::vector<std::string> Failures;
    size_t A = 0, B = 0, Skipped = 0;
};

Merged Merge(std::span<const Sweep> per_file);

inline void Report(const Merged &m, const char *what) {
    for (const std::string &s : m.Failures) MESSAGE(s);
    MESSAGE(what, " at ", m.A, " refs, salvaging ", m.B, " comments, skipping ", m.Skipped, " generated");
    CHECK(m.Failures.empty());
}

// Every corpus file on every core. `body(f, l, sw)` fills `sw`.
template<class Body> Merged SweepFiles(Body body) {
    return Merge(MapEach<Sweep>(WholeCorpus(), [&body](const CorpusFile &f) {
        Sweep sw;
        if (Skipped(f, sw)) return sw;
        Loaded l(f);
        if (!l.R.Diags.empty()) return sw;
        body(f, l, sw);
        return sw;
    }));
}

} // namespace faustlens::test
