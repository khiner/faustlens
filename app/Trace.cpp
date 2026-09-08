#include "Trace.h"

#include <algorithm>
#include <span>

namespace faustlens::app {

namespace {

bool Names(std::span<const ValueId> terms, ValueId v) { return v != NoTerm && std::ranges::contains(terms, v); }

} // namespace

Trace TraceControl(Session &s, const Plan &plan, uint32_t widget_label) {
    Trace t;
    t.Control = std::string(plan.Label(widget_label));
    for (const Field &f : plan.Fields)
        if (f.Kind == FieldKind::Widget && f.Label == widget_label && f.Origin != NoTerm && !Names(t.Terms, f.Origin)) t.Terms.push_back(f.Origin);
    if (t.Terms.empty()) return t;
    // Count every control declared by this source range.
    for (const Field &f : plan.Fields)
        if (f.Kind == FieldKind::Widget && Names(t.Terms, f.Origin)) ++t.Controls;
    // Select the first file deterministically when interned terms occur in several files.
    for (const std::string &p : s.Parsed())
        for (const ValueId v : t.Terms)
            if (!Marks(s.TermsOf(p).Refs, v).empty()) {
                t.Path = p;
                return t;
            }
    return t;
}

std::vector<Span> TraceMarks(const FileView &f, const Trace &t) {
    std::vector<Span> out;
    for (const ValueId v : t.Terms)
        for (const Span &sp : Marks(f, v)) out.push_back(sp);
    std::ranges::sort(out, [](const Span &a, const Span &b) { return a.Begin < b.Begin; });
    return out;
}

} // namespace faustlens::app
