#pragma once

#include "query/Query.h"
#include "query/Snapshot.h"
#include "syntax/Edit.h"

namespace faustlens::app {

// Return one rewrite for preview or materialization, preserving comments when spliced.
Edit Expand(Session &, const FileView &, RefId);

} // namespace faustlens::app
