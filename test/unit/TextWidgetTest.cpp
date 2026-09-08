#include "doctest.h"
#include "editor/Draw.h"
#include "imgui_internal.h"

using namespace faustlens;
using namespace faustlens::app;

namespace {

struct Widget {
    Workspace Ws;
    TextDraft Draft;
    ImGuiID Id = 0;
    bool Saved = false;

    Widget() {
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.DisplaySize = {640, 480};
        io.DeltaTime = 1.0f / 60;
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char *pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        Ws.Open("/t.dsp", "process=_;");
    }
    ~Widget() { ImGui::DestroyContext(); }
    Buffer &Buffer() { return *Ws.Find("/t.dsp"); }

    void Frame(bool focus = false) {
        Draft.Sync(Buffer());
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({640, 480});
        ImGui::Begin("source", nullptr, ImGuiWindowFlags_NoSavedSettings);
        if (focus) ImGui::SetKeyboardFocusHere();
        Id = ImGui::GetID("program");
        DrawText("program", Draft, {600, 400});
        Draft.Commit(Ws, "/t.dsp");
        const auto keys = ReadWorkspaceKeys();
        if (keys.Undo) Ws.Undo();
        if (keys.Redo) Ws.Redo();
        Saved |= keys.Save;
        ImGui::End();
        ImGui::Render();
    }
    void Key(ImGuiKey key) {
        ImGui::GetIO().AddKeyEvent(key, true);
        Frame();
        ImGui::GetIO().AddKeyEvent(key, false);
        Frame();
    }
    void Command(ImGuiKey key, bool shift = false) {
        auto &io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Super, true); // native macOS command is remapped by ImGui
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
        Key(key);
        io.AddKeyEvent(ImGuiMod_Super, false);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
        Frame();
    }
};

} // namespace

TEST_CASE("the active text widget accepts arbitrary text and reloads shared undo with selection") {
    Widget w;
    w.Frame(true);
    w.Frame();
    REQUIRE(ImGui::GetInputTextState(w.Id));
    w.Buffer().SetSelection(10, 8);
    w.Frame();
    ImGui::GetIO().AddInputCharactersUTF8("(é");
    w.Frame();
    CHECK(w.Buffer().Text() == "process=(é");
    CHECK(w.Buffer().Cursor == 11);
    CHECK(w.Buffer().Anchor == 11);
    CHECK(w.Ws.UndoStack.size() == 1);
    w.Key(ImGuiKey_Enter);
    CHECK(w.Buffer().Text() == "process=(é\n");
    REQUIRE(w.Ws.Undo());
    w.Frame();
    CHECK(w.Draft.Text == "process=(é");
    CHECK(w.Draft.Cursor == 11);
    CHECK(w.Draft.Anchor == 11);
    REQUIRE(w.Ws.Undo());
    w.Frame();
    CHECK(w.Draft.Text == "process=_;");
    CHECK(w.Draft.Cursor == 10);
    CHECK(w.Draft.Anchor == 8);
    const auto *state = ImGui::GetInputTextState(w.Id);
    REQUIRE(state);
    CHECK((state->Flags & ImGuiInputTextFlags_NoUndoRedo) != 0);
    REQUIRE(w.Ws.Redo());
    w.Frame();
    CHECK(w.Draft.Text == "process=(é");
    CHECK(w.Draft.Cursor == 11);
    CHECK(w.Draft.Anchor == 11);
    w.Key(ImGuiKey_Backspace);
    CHECK(w.Buffer().Text() == "process=("); // removes a whole UTF-8 character
    w.Command(ImGuiKey_Z);
    CHECK(w.Buffer().Text() == "process=(é");
    w.Command(ImGuiKey_Z, true);
    CHECK(w.Buffer().Text() == "process=(");
    w.Command(ImGuiKey_S);
    CHECK(w.Saved);
}
