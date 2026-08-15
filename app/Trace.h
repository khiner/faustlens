// A control and the source declaring it. A link set, not a jump: an `hslider` in a
// `par` declares as many as 64 controls off one line.
#pragma once

#include "query/Query.h"
#include "query/Snapshot.h"
#include "signal/Plan.h"

#include <cstdint>
#include <string>
#include <vector>

namespace faustlens::app {

struct Trace {
    // Plural where two widget fields share a label path.
    std::vector<ValueId> Terms;
    // The first parsed file whose ref tree writes one of them, empty where none does.
    std::string Path;
    // Text rather than an id: an id indexes the arena that lowered this plan.
    std::string Control;
    size_t Controls = 0;

    explicit operator bool() const { return !Terms.empty() && !Path.empty(); }
};

// Asks `Session` rather than the snapshot: the file is regularly not open.
Trace TraceControl(Session &, const Plan &, uint32_t widget_label);

// Every occurrence in one file of anything the trace named, in source order.
std::vector<Span> TraceMarks(const FileView &, const Trace &);

} // namespace faustlens::app
