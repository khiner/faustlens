// Compare evaluated diagrams with a bijection over node and slot ids.
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

// Return the first mismatched node pair on failure.
std::expected<void, std::string> Isomorphic(const BoxSide &a, BoxId x, const BoxSide &b, BoxId y);

// Format graph structure for diagnostics.
std::string PrintBox(const BoxSide &, BoxId, int max_depth = 6);

// Normalize metadata keys and duplicate values to reference printing conventions.
std::map<std::string, std::vector<std::string>> DeclareView(const MetaSet &);

// Compare normalized metadata excluding reference-generated keys.
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
