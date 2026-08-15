#include "syntax/Diagnostic.h"

#include <algorithm>
#include <array>
#include <tuple>

namespace faustlens {
namespace {

constexpr std::array<std::string_view, size_t(Code::Count_)> Names = {
    "syn/unexpected-token",
    "syn/unterminated-comment",
    "syn/unterminated-string",
    "syn/unterminated-mdoc",
    "syn/empty-case",
    "syn/bad-listing-attribute",
    "syn/depth-exceeded",
    "res/file-not-found",
    "res/import-cycle",
    "res/read-error",
    "eval/unbound-name",
    "eval/arity-mismatch",
    "eval/no-matching-rule",
    "eval/redefinition",
    "eval/rule-shadowed",
    "eval/not-applicable",
    "eval/bad-access",
    "eval/non-constant-iteration-count",
    "eval/loop-detected",
    "eval/depth-exceeded",
    "eval/not-a-closure",
    "eval/bad-modulation-circuit",
    "eval/no-modulation-target",
    "eval/invalid-pattern",
    "prop/unsupported",
    "prop/duplicate-path",
    "prop/duplicate-bargraph-path",
    "info/precision-filtered",
};

// A deterministic order across runs.
bool DiagnosticLess(const Diagnostic &a, const Diagnostic &b) { return std::tie(a.File, a.Begin, a.Code) < std::tie(b.File, b.Begin, b.Code); }

} // namespace

std::string_view CodeName(Code c) { return Names[size_t(c)]; }

void SortAndDedupe(std::vector<Diagnostic> &ds) {
    std::ranges::stable_sort(ds, DiagnosticLess);
    const auto same = [](const Diagnostic &a, const Diagnostic &b) {
        return a.Code == b.Code && a.Subject == b.Subject && a.Payload == b.Payload && a.Begin == b.Begin && a.File == b.File;
    };
    // `ranges::unique` hands back the whole tail to drop, so there is no `end()` to repeat.
    const auto duplicates = std::ranges::unique(ds, same);
    ds.erase(duplicates.begin(), duplicates.end());
}

} // namespace faustlens
