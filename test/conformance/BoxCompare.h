// Compares two evaluated diagrams by isomorphism, not bytes: across sessions, ids and
// slot numbers agree only up to a bijection.
#pragma once

#include "box/Box.h"
#include "eval/Eval.h"
#include "query/Query.h"
#include "syntax/Term.h"

#include <expected>
#include <format>
#include <map>
#include <string>
#include <vector>

namespace faustlens::test {

struct BoxSide {
    const Boxes &Boxes;
    const Terms &Terms;
};

// Unexpected names the first pair of nodes that disagreed.
std::expected<void, std::string> Isomorphic(const BoxSide &a, BoxId x, const BoxSide &b, BoxId y);

// A shape dump for failure messages, not `faust -e`'s notation.
std::string PrintBox(const BoxSide &, BoxId, int max_depth = 6);

// What the reference would emit: `.`, `:` and `/` become `_`, and only a key's first
// value survives. `author`'s later values spill into `contributor`, which keeps its own.
std::map<std::string, std::vector<std::string>> DeclareView(const MetaSet &);

// Agreement under that rendering, ignoring keys the reference synthesizes into every document.
std::expected<void, std::string> SameDeclares(const MetaSet &ours, const MetaSet &theirs);

inline std::string FirstError(const Session &s) {
    for (const Diagnostic &d : s.Diagnostics()) {
        if (d.Severity != Severity::Error) continue;
        return std::format("{}{}", CodeName(d.Code), d.Payload.empty() ? "" : " " + d.Payload);
    }
    return {};
}

inline bool Raised(const Session &s, Code c) {
    for (const Diagnostic &d : s.Diagnostics())
        if (d.Code == c) return true;
    return false;
}

} // namespace faustlens::test
