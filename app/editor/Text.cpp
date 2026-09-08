#include "editor/Text.h"

namespace faustlens::app {

void TextDraft::Sync(const Buffer &buffer) {
    if (Seen != buffer.Shared || Cursor != buffer.Cursor || Anchor != buffer.Anchor) {
        Text = buffer.Text();
        Cursor = buffer.Cursor;
        Anchor = buffer.Anchor;
        Reload = true;
        Seen = buffer.Shared;
    }
}

bool TextDraft::Commit(Workspace &ws, const std::string &path) {
    Buffer *buffer = ws.Find(path);
    if (!buffer || Seen != buffer->Shared) return false;
    const auto &old = buffer->Text();
    size_t begin = 0, old_end = old.size(), new_end = Text.size();
    while (begin < old_end && begin < new_end && old[begin] == Text[begin]) ++begin;
    while (old_end > begin && new_end > begin && old[old_end - 1] == Text[new_end - 1]) --old_end, --new_end;
    const bool changed = old_end != begin || new_end != begin;
    if (changed) ws.Edit(path, {{uint32_t(begin), uint32_t(old_end), Text.substr(begin, new_end - begin)}});
    buffer->SetSelection(Cursor, Anchor);
    Seen = buffer->Shared;
    return changed;
}

} // namespace faustlens::app
