#pragma once

#include "editor/Text.h"
#include "query/Snapshot.h"

#include "imgui.h"

namespace faustlens::app {

// Caller commits at frame end. The widget has no separate undo stack.
bool DrawText(const char *id, TextDraft &, const ImVec2 &size, std::span<const Span> marks = {}, bool reveal = false);

struct WorkspaceKeys {
    bool Undo = false, Redo = false, Save = false;
};
WorkspaceKeys ReadWorkspaceKeys();

} // namespace faustlens::app
