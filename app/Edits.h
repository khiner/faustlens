// Edits require the source bytes used to compute their links.
#pragma once

#include "boxview/Select.h"
#include "editor/Workspace.h"
#include "query/Snapshot.h"
#include "syntax/Edit.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace faustlens::app {

// Use a distinct key for each connective: `<` for `<:` and `>` for `:>`.
enum class Key : uint8_t {
    None,
    Sequence,
    Parallel,
    Split,
    Merge,
    Recursive,
    Remove,
};

Key KeyForChar(unsigned codepoint);

struct Connective {
    char Char;
    Key Edit;
};
std::span<const Connective> Connectives();

// Return the replacement text for a composition key, such as `a : _` for `:`.
std::string ComposeExample(Terms &, Key);

Edit EditFor(Terms &, const FileView &, const boxview::Selection &, Key);

// Return the editable lexeme at the caret, including quotes, or an empty string.
std::string_view TextOf(const Terms &, const FileView &, const boxview::Selection &);
Edit EditForText(Terms &, const FileView &, const boxview::Selection &, std::string_view);

// Toggle a connection between 1-based source channels, supplied in either order.
Edit RewireDrag(Terms &, const FileView &, const boxview::Selection &route, uint32_t in, uint32_t out);

// Commit one Workspace edit if the buffer still matches the source refs.
bool Apply(const Terms &, Workspace &, const FileView &, const Edit &);

} // namespace faustlens::app
