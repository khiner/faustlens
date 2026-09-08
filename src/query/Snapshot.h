// Published source data and refs remain valid until their snapshot is replaced.
#pragma once

#include "query/Query.h"
#include "syntax/Term.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace faustlens {

struct FileView {
    std::string Path;
    std::string Text;
    ValueId Root = NoTerm;
    RefTree Refs;
    TokenVector Tokens;
};

struct Span {
    uint32_t Begin = 0, End = 0;
};

struct Snapshot {
    uint64_t Revision = 0;
    std::vector<FileView> Files;
    std::vector<Diagnostic> Diags;

    const FileView *File(std::string_view path) const;
};

// Return every source range for this value.
std::vector<Span> Marks(const RefTree &, ValueId);
std::vector<Span> Marks(const FileView &, ValueId);

// Return subject occurrences, or the diagnostic's explicit range when it has no subject.
std::vector<Span> Marks(const FileView &, const Diagnostic &);

// Return the innermost ref containing offset.
RefId Innermost(const FileView &, uint32_t offset);

// Return NoTerm when offset is outside the root span.
ValueId ValueAt(const FileView &, uint32_t offset);

// Return ancestors innermost first.
std::vector<ValueId> ValuesAt(const FileView &, uint32_t offset);

// Return a byte anchor for this source occurrence.
std::optional<uint32_t> OffsetOfRef(const FileView &, RefId);

// Return an offset satisfying ValueAt(f, offset) == v, outside child spans.
std::optional<uint32_t> OffsetOf(const FileView &, ValueId);

// Return NoTerm for missing or pattern-defined process.
ValueId ProcessBody(const Terms &, ValueId program);
// Return the specific process body occurrence.
RefId ProcessBodyRef(const Terms &, const FileView &);

Snapshot Publish(Session &, const std::vector<std::string> &open_paths);

} // namespace faustlens
