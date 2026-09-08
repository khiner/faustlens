// One source occurrence can declare several controls through iteration.
#pragma once

#include "query/Query.h"
#include "query/Snapshot.h"
#include "signal/Plan.h"

#include <cstdint>
#include <string>
#include <vector>

namespace faustlens::app {

struct Trace {
    std::vector<ValueId> Terms;
    // First parsed file containing a declaring ref, or empty.
    std::string Path;
    // Label text remains valid across syntax pools.
    std::string Control;
    size_t Controls = 0;

    explicit operator bool() const { return !Terms.empty() && !Path.empty(); }
};

// Query the Session to include files absent from the snapshot.
Trace TraceControl(Session &, const Plan &, uint32_t widget_label);

// Return declaring occurrences in source order.
std::vector<Span> TraceMarks(const FileView &, const Trace &);

} // namespace faustlens::app
