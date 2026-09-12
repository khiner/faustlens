#pragma once

#include "syntax/Edit.h"

namespace faustlens {
struct Session;
struct FileView;

// Return one rewrite for preview or materialization, preserving comments when spliced.
Edit Expand(Session &, const FileView &, RefId);

} // namespace faustlens
