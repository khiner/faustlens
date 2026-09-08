// Evaluate a source occurrence and lift it to a surface rewrite.
#pragma once

#include "query/Query.h"
#include "query/Snapshot.h"
#include "syntax/Edit.h"

namespace faustlens::app {

// Preview draws the replacement; materialization commits this same edit.
// Comments survive materialization by salvage.
Edit Expand(Session &, const FileView &, RefId);

} // namespace faustlens::app
