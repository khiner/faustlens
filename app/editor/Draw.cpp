#include "editor/Draw.h"

#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>

namespace faustlens::app {
namespace {

int Cursor(ImGuiInputTextCallbackData *data) {
    auto &draft = *static_cast<TextDraft *>(data->UserData);
    draft.Cursor = uint32_t(data->CursorPos);
    draft.Anchor = draft.Cursor;
    if (data->HasSelection()) draft.Anchor = uint32_t(data->CursorPos == data->SelectionStart ? data->SelectionEnd : data->SelectionStart);
    return 0;
}

} // namespace

WorkspaceKeys ReadWorkspaceKeys() {
    constexpr auto flags = ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverActive;
    return {
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, flags), ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, flags),
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, flags)
    };
}

bool DrawText(const char *label, TextDraft &draft, const ImVec2 &size, std::span<const Span> marks, bool reveal) {
    const ImGuiID id = ImGui::GetID(label);
    if (draft.Reload) {
        if (ImGuiInputTextState *state = ImGui::GetInputTextState(id)) {
            state->ReloadUserBufAndKeepSelection();
            state->ReloadSelectionStart = int(draft.Anchor);
            state->ReloadSelectionEnd = int(draft.Cursor);
        }
        draft.Reload = false;
    }
    ImGuiWindow *parent = ImGui::GetCurrentWindow();
    const int before = parent->DC.ChildWindows.Size;
    const auto flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_NoUndoRedo | ImGuiInputTextFlags_CallbackAlways;
    const bool changed = ImGui::InputTextMultiline(label, &draft.Text, size, flags, Cursor, &draft);
    if (parent->DC.ChildWindows.Size <= before) return changed;
    ImGuiWindow *child = parent->DC.ChildWindows.back();
    const float line_height = ImGui::GetTextLineHeight();
    const ImVec2 origin(child->Pos.x + ImGui::GetStyle().FramePadding.x - child->Scroll.x, child->Pos.y + ImGui::GetStyle().FramePadding.y - child->Scroll.y);
    child->DrawList->PushClipRect(child->InnerClipRect.Min, child->InnerClipRect.Max, true);
    uint32_t offset = 0;
    float y = origin.y;
    while (offset <= draft.Text.size()) {
        const size_t newline = draft.Text.find('\n', offset);
        const size_t end = newline == std::string::npos ? draft.Text.size() : newline;
        if (y + line_height >= child->InnerClipRect.Min.y && y <= child->InnerClipRect.Max.y)
            for (const Span &mark : marks) {
                if (mark.End <= offset || mark.Begin > end) continue;
                const uint32_t begin = std::max(mark.Begin, offset), last = std::min(mark.End, uint32_t(end));
                const char *text = draft.Text.data();
                const float left = ImGui::CalcTextSize(text + offset, text + begin).x;
                const float right = ImGui::CalcTextSize(text + offset, text + last).x;
                child->DrawList->AddRectFilled({origin.x + left, y}, {origin.x + right, y + line_height}, 0x442F5A80);
            }
        if (reveal && draft.Cursor >= offset && draft.Cursor <= end) {
            ImGui::SetScrollY(child, std::max(0.0f, y - origin.y - child->InnerRect.GetHeight() * 0.35f));
            reveal = false;
        }
        if (newline == std::string::npos) break;
        offset = uint32_t(newline + 1);
        y += line_height;
    }
    child->DrawList->PopClipRect();
    return changed;
}

} // namespace faustlens::app
