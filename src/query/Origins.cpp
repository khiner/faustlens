#include "query/Origins.h"
#include "signal/Plan.h"
#include <algorithm>

namespace faustlens {
namespace {
std::vector<Span> SpansOf(const RefTree &refs, std::span<const ValueId> terms) {
    std::vector<Span> spans;
    if (terms.empty()) return spans;
    for (const auto &ref : refs.Refs)
        if (ref.ValueId != NoTerm && std::ranges::contains(terms, ref.ValueId)) spans.push_back({ref.SpanBegin, ref.SpanEnd});
    std::ranges::sort(spans, {}, &Span::Begin);
    return spans;
}
} // namespace
std::vector<Span> OriginSpans(const FileView &file, std::span<const ValueId> terms) { return SpansOf(file.Refs, terms); }
ControlOrigins FindControlOrigins(Session &session, const Plan &plan, uint32_t label) {
    ControlOrigins result;
    for (const auto &field : plan.Fields)
        if (field.Kind == FieldKind::Widget && field.Label == label && field.Origin != NoTerm && !std::ranges::contains(result.Terms, field.Origin))
            result.Terms.push_back(field.Origin);
    if (result.Terms.empty()) return result;
    for (const auto &field : plan.Fields) result.Controls += field.Kind == FieldKind::Widget && std::ranges::contains(result.Terms, field.Origin);
    size_t occurrences{0};
    for (const auto &path : session.Parsed()) {
        auto spans{SpansOf(session.TermsOf(path).Refs, result.Terms)};
        if (spans.empty()) continue;
        occurrences += spans.size();
        result.Files.push_back({path, std::move(spans)});
    }
    result.Ambiguous = occurrences > 1;
    return result;
}
} // namespace faustlens
