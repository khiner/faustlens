// The text widget's draft commits through the same history as structural edits.
#pragma once

#include "editor/Workspace.h"

namespace faustlens::app {

struct TextDraft {
    std::string Text;
    uint32_t Cursor = 0, Anchor = 0;
    bool Reload = false;
    std::shared_ptr<const std::string> Seen;

    void Sync(const Buffer &);
    bool Commit(Workspace &, const std::string &path);
};

} // namespace faustlens::app
