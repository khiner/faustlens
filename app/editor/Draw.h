#pragma once

#include "editor/Text.h"
#include "query/Snapshot.h"

#include "imgui.h"

namespace faustlens::app {

// Commit at frame end through Workspace history.
bool DrawText(const char *id, TextDraft &, const ImVec2 &size, std::span<const Span> marks = {}, bool reveal = false);

struct WorkspaceKeys {
    bool Undo = false, Redo = false, Save = false;
};
WorkspaceKeys ReadWorkspaceKeys();

} // namespace faustlens::app
