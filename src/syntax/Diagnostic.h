#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace faustlens {

enum class Severity : uint8_t { Error, Warning, Info };

// Preserve phase prefixes used by conformance comparisons.
enum class Code : uint16_t {
    SynUnexpectedToken,
    SynUnterminatedComment,
    SynUnterminatedString,
    SynUnterminatedMdoc,
    SynEmptyCase,
    SynBadListingAttribute,
    SynDepthExceeded,
    ResFileNotFound,
    ResImportCycle,
    ResReadError,
    EvalUnboundName,
    EvalArityMismatch,
    EvalNoMatchingRule,
    EvalRedefinition,
    EvalRuleShadowed, // warning
    EvalNotApplicable,
    EvalBadAccess,
    EvalNonConstantIterationCount,
    EvalLoopDetected,
    EvalDepthExceeded,
    EvalNotAClosure,
    EvalBadModulationCircuit,
    EvalNoModulationTarget, // warning
    EvalInvalidPattern,
    PropUnsupported,
    PropDuplicatePath,
    PropDuplicateBargraphPath, // warning
    InfoPrecisionFiltered,
    Count_
};

std::string_view CodeName(Code); // e.g. "syn/unexpected-token"

inline constexpr uint32_t NoValue = 0xFFFFFFFFu;

struct Diagnostic {
    Severity Severity = Severity::Error;
    Code Code = Code::SynUnexpectedToken;
    // Source value id, or NoValue for an explicit span.
    uint32_t Subject = NoValue;
    uint32_t File = 0; // file resolution index
    uint32_t Begin = 0, End = 0;
    std::vector<uint32_t> Related;
    std::string Payload;
};

// Deduplicate diagnostics from memoized subterms.
void SortAndDedupe(std::vector<Diagnostic> &);

} // namespace faustlens
