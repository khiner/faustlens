#pragma once
#include "query/Snapshot.h"

namespace faustlens {
struct Plan;
struct SourceOccurrences {
    std::string Path;
    std::vector<Span> Spans;
};
struct ControlOrigins {
    std::vector<ValueId> Terms;
    std::vector<SourceOccurrences> Files;
    size_t Controls{0};
    bool Ambiguous{false};
};
// Return candidate source occurrences; equal interned terms can include declarations outside the executed expression.
ControlOrigins FindControlOrigins(Session &, const Plan &, uint32_t label);
std::vector<Span> OriginSpans(const FileView &, std::span<const ValueId>);
} // namespace faustlens
