#pragma once

#include "signal/Signal.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace faustlens {

struct UiItem;

struct DifferentiateRequest {
    std::vector<std::string> Controls;
    bool ExplicitDirections = false;
    size_t DirectionCount = 0;
    // Row-major control-by-direction matrix; selected mode uses the identity.
    std::vector<double> Directions;
    size_t MaxDirections = 256;
    // Checked between rule expansions; the final expansion can exceed this limit.
    size_t MaxNewNodes = 1'000'000;
    size_t MaxDiagnostics = 65'536;
    size_t MaxWorkspaceBytes = 256 * 1024 * 1024;
    size_t MaxAnalysisVisits = 64'000'000;
};

enum class DerivativeDiagnosticKind { Absent, Barrier, Unsupported };
struct DifferentiationDiagnostic {
    DerivativeDiagnosticKind Kind;
    size_t Control;
    SigId Signal = NoSig;
    ValueId Origin = NoTerm;
    std::string Reason;
};
struct ControlDerivativeInfo {
    std::string Label;
    bool Present = false;
    // Conservative per-control dependencies, unaffected by direction cancellation.
    bool Structural = false;
    bool Active = false;
};
struct DifferentiateResult {
    // Output-major: output * DirectionCount + direction. Append to unchanged primal roots.
    std::vector<SigId> Tangents;
    size_t DirectionCount = 0;
    std::vector<ControlDerivativeInfo> Controls;
    std::vector<DifferentiationDiagnostic> Diagnostics;
    std::string Error;
    size_t AllocatedNodes = 0;
    size_t EstimatedWorkspaceBytes = 0;
    bool Ok() const { return Error.empty(); }
};

// Requires normalized signals, physical controls constant during each render, and seed-independent initial state.
// Ui metadata identifies eliminated widgets and validates repeated control labels.
// Reports piecewise/discrete conventions and rejects active foreign calls, bit reinterpretation,
// and seed-dependent initialization before constructing tangents.
// Renderers must reject nonfinite primal/tangent samples.
// Budget failures can leave unreachable nodes in the append-only arena.
DifferentiateResult Differentiate(Signals &, std::span<const SigId> roots, const DifferentiateRequest &, std::span<const UiItem> ui = {});

} // namespace faustlens
