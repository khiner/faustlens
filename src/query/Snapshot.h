// One immutable snapshot per compile drain, rendered by the UI and up to one compile
// behind the buffer. Only permanent ids, so a view over boxes copies out a render list.
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

// Every byte range naming this value. A value written in three places has three.
std::vector<Span> Marks(const RefTree &, ValueId);
std::vector<Span> Marks(const FileView &, ValueId);

// Every occurrence of the subject, or the diagnostic's own byte range if it has
// no subject.
std::vector<Span> Marks(const FileView &, const Diagnostic &);

// The innermost ref whose span contains `offset`. `O(depth)`.
RefId Innermost(const FileView &, uint32_t offset);

// `NoTerm` where the offset is outside the root span.
ValueId ValueAt(const FileView &, uint32_t offset);

// Innermost first, for a view that draws only some kinds.
std::vector<ValueId> ValuesAt(const FileView &, uint32_t offset);

// Anchor a selection with this rather than `OffsetOf`, which can land the caret
// on an occurrence the reader never pointed at.
std::optional<uint32_t> OffsetOfRef(const FileView &, RefId);

// Satisfies `ValueAt(f, *OffsetOf(f, v)) == v`. For a composition, never its
// first byte, which it shares with its first child.
std::optional<uint32_t> OffsetOf(const FileView &, ValueId);

// `NoTerm` where the file defines `process` by a pattern or not at all.
ValueId ProcessBody(const Terms &, ValueId program);
// Only the ref resolves to a particular occurrence.
RefId ProcessBodyRef(const Terms &, const FileView &);

Snapshot Publish(Session &, const std::vector<std::string> &open_paths);

} // namespace faustlens
