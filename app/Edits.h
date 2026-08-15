// Selection plus a key, over the edit catalogue. An edit binds to the source its links
// were computed against.
#pragma once

#include "boxview/Select.h"
#include "editor/Workspace.h"
#include "query/Query.h"
#include "query/Snapshot.h"
#include "syntax/Edit.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace faustlens::app {

// Keyed on the character each connective owns alone: `<` only begins `<:`, `>` only
// ends `:>`, where `:` would have to wait.
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

// One table, so the help and the keymap cannot disagree.
struct Connective {
    char Char;
    Key Edit;
};
std::span<const Connective> Connectives();

// What one key press produces, as text: `a : _` for `:`.
std::string ComposeExample(Terms &, Key);

// A new stage goes after the selection.
Edit EditFor(Terms &, const FileView &, const boxview::Selection &, Key);

// The lexeme, so a label keeps its quotes, empty where no edit may change it. Scoped to
// the caret's node.
std::string_view TextOf(const Terms &, const FileView &, const boxview::Selection &);
Edit EditForText(Terms &, const FileView &, const boxview::Selection &, std::string_view);

// `in` and `out` are 1-based source channels, either way round. Toggles: connect
// where the pair is absent, disconnect where present.
Edit RewireDrag(Terms &, const FileView &, const boxview::Selection &route, uint32_t in, uint32_t out);

// Splice, apply as one undoable step, and tell the session. Refuses if `view` is not
// that file's buffer.
bool Apply(Session &, Workspace &, const std::string &path, const FileView &view, const Edit &);

} // namespace faustlens::app
