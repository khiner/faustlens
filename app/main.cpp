#include "Compiler.h"
#include "Edits.h"
#include "Live.h"
#include "Trace.h"
#include "boxview/Draw.h"
#include "boxview/Layout.h"
#include "boxview/Select.h"
#include "controls/Draw.h"
#include "editor/Draw.h"
#include "editor/Workspace.h"
#include "files/Vfs.h"
#include "query/Snapshot.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "imgui_internal.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace faustlens;
using faustlens::app::Artifact;

namespace {

struct InlineField {
    bool Open = false;
    bool Focus = false;
    bool Armed = false; // previously focused
    char Text[128] = {};
};

// A child-index path distinguishes identical routes sharing one interned value.
struct PortDrag {
    bool Active = false;
    std::vector<uint32_t> At;
    bool Input = false;
    uint32_t Channel = 0;
    ImVec2 From{};
};

struct App {
    std::string Path, Focus;
    app::Workspace Ws;
    std::map<std::string, std::string> OnDisk;
    app::Live Live;
    app::Compiler Compiler;
    std::unique_ptr<app::Compiler::Publication> View = std::make_unique<app::Compiler::Publication>();
    uint64_t Requested = 0, DocumentRevision = 1;
    bool AudioAccepted = false, AudioAttempted = false;
    boxview::Selection Sel;
    InlineField Field;
    PortDrag Drag;
    std::vector<RefId> Open;
    bool ResolveSelection = false;
    std::string Refused, Conflict, WantFile, Opening, TabFocus;
    std::vector<std::string> Touched;
    std::optional<RefId> WantMaterialize;
    std::optional<uint32_t> WantTrace;
    app::Trace Traced;
    bool Reveal = false;
    std::map<std::string, app::TextDraft> Drafts;
    std::string Stale, AudioError;
    app::Live::Result Last;
    std::map<std::string, std::filesystem::file_time_type> Watching;

    faustlens::Terms &Terms() { return View->Terms; }
    const Snapshot &Snap() const { return View->Snap; }
    bool Fresh(const FileView &f) const {
        const auto *b = Ws.Find(f.Path);
        return b && b->Text() == f.Text;
    }
    bool Dirty(const std::string &p) const {
        const auto *b = Ws.Find(p);
        const auto saved = OnDisk.find(p);
        return b && saved != OnDisk.end() && b->Text() != saved->second;
    }

    void Queue(bool edited = false) {
        if (edited) {
            ++DocumentRevision;
            if (const auto *b = Ws.Find(Focus)) Sel.Caret = b->Cursor;
            ResolveSelection = true;
            Refused.clear();
            Traced = {};
            Open.clear();
            WantMaterialize.reset();
            Field = {};
            Drag = {};
        }
        app::Compiler::Request request;
        request.Root = Path;
        for (const auto &[path, buffer] : Ws.Files) request.Buffers.emplace(path, buffer.Shared);
        request.OpenFiles = Ws.Paths();
        if (!Opening.empty() && !Ws.IsOpen(Opening)) request.OpenFiles.push_back(Opening);
        request.Touched = std::exchange(Touched, {});
        request.Current = Live.Current;
        request.Controls = Ws.Controls;
        request.SampleRate = Live.SampleRate();
        request.DocumentRevision = DocumentRevision;
        request.ViewRevision = View->DocumentRevision;
        if (const auto *f = Snap().File(Path)) request.ViewText = f->Text;
        request.Expanded = Open;
        request.Materialize = WantMaterialize;
        request.TraceLabel = WantTrace;
        Requested = Compiler.Submit(std::move(request));
    }

    void Load(const std::string &p) {
        Path = Focus = std::filesystem::absolute(p).lexically_normal().string();
        const auto read = ReadFile(Path);
        const std::string text = read.value_or(std::string());
        OnDisk[Path] = text;
        Ws.Open(Path, text);
        if (!read) Refused = read.error();
        Queue();
    }

    void OpenFile(const std::string &p) {
        if (Ws.IsOpen(p)) {
            Focus = p;
            Sel = {};
            Sel.Caret = Ws.Find(p)->Cursor;
            ResolveSelection = true;
            if (Traced && Traced.Path == p) Reveal = true;
        } else {
            Opening = p;
            Queue();
        }
    }

    void Save() {
        const auto *buffer = Ws.Find(Focus);
        if (!buffer) return;
        const auto target = std::filesystem::path(Focus).is_absolute() ? std::filesystem::path(Focus) : std::filesystem::path(Path).parent_path() / Focus;
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file.write(buffer->Text().data(), std::streamsize(buffer->Text().size()));
        file.close();
        if (!file) {
            Refused = "could not save " + target.string();
            return;
        }
        OnDisk[Focus] = buffer->Text();
        std::error_code error;
        Watching[target.string()] = std::filesystem::last_write_time(target, error);
        Conflict.clear();
        Refused.clear();
    }

    void Sync() {
        Compiler.Retire(Live.Collect());
        if (auto ready = Compiler.Poll()) {
            if (ready->Ticket != Requested) {
                Compiler.Retire(std::move(ready));
                return;
            }
            for (const FileView &f : ready->Snap.Files)
                if (!Ws.IsOpen(f.Path)) {
                    Ws.Open(f.Path, f.Text);
                    OnDisk[f.Path] = f.Text;
                }
            if (!Opening.empty() && Ws.IsOpen(Opening)) OpenFile(std::exchange(Opening, {}));
            Refused = ready->Refused;
            if (ready->Traced) {
                Traced = std::move(*ready->Traced);
                if (Traced) {
                    WantFile = Traced.Path;
                    Reveal = true;
                }
            }
            WantMaterialize.reset();
            WantTrace.reset();
            for (const auto &p : ready->AvailableFiles) {
                if (Watching.contains(p)) continue;
                std::error_code error;
                const auto when = std::filesystem::last_write_time(p, error);
                if (!error) Watching.emplace(p, when);
            }
            Compiler.Retire(std::exchange(View, std::move(ready)));
            AudioAccepted = false;
            ResolveSelection = true;
            if (const auto *b = Ws.Find(Focus)) Sel.Caret = b->Cursor;
            if (View->Materialized) {
                if (const auto *f = Snap().File(Path); f && app::Apply(Terms(), Ws, *f, *View->Materialized)) Queue(true);
                View->Materialized.reset();
            }
        }
        if (View->Ticket != Requested || AudioAccepted) return;
        Last = Live.Accept(View->Audio, Ws.Controls);
        if (Last.Deferred) return;
        AudioAccepted = true;
        Stale = Last.Compiled ? std::string() : Last.Why;
        if (Live.Current && !AudioAttempted) {
            AudioAttempted = true;
            auto started = Live.Host.Start(*Live.Current->Dsp);
            AudioError = started ? std::string() : started.error();
            controls::Apply(Ws.Controls, Live.Current->Plan, Live.Current->Ui, *Live.Current->Dsp);
        }
    }

    void PollForEdits() {
        bool changed = false;
        for (auto &[p, when] : Watching) {
            std::error_code error;
            const auto now = std::filesystem::last_write_time(p, error);
            if (error || now == when) continue;
            when = now;
            if (Ws.IsOpen(p) && Dirty(p)) {
                Conflict = "changed on disk, and this buffer has unsaved edits";
                continue;
            }
            if (auto *b = Ws.Find(p)) {
                if (const auto read = ReadFile(p)) {
                    const uint32_t cursor = b->Cursor, anchor = b->Anchor;
                    Ws.Open(p, *read);
                    Ws.Find(p)->SetSelection(cursor, anchor);
                    OnDisk[p] = *read;
                }
            }
            Touched.push_back(p);
            changed = true;
        }
        if (changed) Queue(true);
    }

    void ApplyEdit(const Edit &e) {
        const FileView *f = Snap().File(Focus);
        if (!f || !Fresh(*f)) return;
        if (!e) {
            Refused = e.Declined ? e.Declined : "";
            return;
        }
        auto *b = Ws.Find(Focus);
        b->SetCursor(Sel.Caret);
        if (!app::Apply(Terms(), Ws, *f, e)) return;
        Queue(true);
    }

    void ExpandSelection(bool materialize) {
        const FileView *f = Snap().File(Path);
        if (!f || Focus != Path || !Fresh(*f)) return;
        const RefId ref = boxview::SelectedRef(*f, Sel);
        if (ref == NoRef) return;
        if (materialize) {
            WantMaterialize = ref;
            Ws.Find(Path)->SetCursor(Sel.Caret);
        } else if (std::erase(Open, ref) == 0) Open.push_back(ref);
        Queue();
    }

    void Undo(bool redo) {
        const auto step = redo ? Ws.Redo() : Ws.Undo();
        if (!step) return;
        if (Live.Current) controls::Apply(Ws.Controls, Live.Current->Plan, Live.Current->Ui, *Live.Current->Dsp);
        if (!step.Texts.empty()) Queue(true);
    }

    void TraceBack(uint32_t label) {
        WantTrace = label;
        Queue();
    }
};

std::vector<uint32_t> PathToNode(const boxview::Node &root, const boxview::Node &n) {
    std::vector<uint32_t> path;
    boxview::Layout::HitPath(root, n.Bounds.X + n.Bounds.W / 2, n.Bounds.Y + n.Bounds.H / 2, path);
    return path;
}

std::optional<uint32_t> SourcePane(App &app, const FileView *f, std::span<const Span> marks, bool selection) {
    std::optional<uint32_t> clicked;
    ImGui::Begin("source");
    std::string want;
    const bool moved = app.TabFocus != app.Focus;
    app.TabFocus = app.Focus;
    if (ImGui::BeginTabBar("##files")) {
        for (const std::string &p : app.Ws.Paths()) {
            const std::string label = std::filesystem::path(p).filename().string() + (app.Dirty(p) ? " *" : "");
            const ImGuiTabItemFlags flags = moved && p == app.Focus ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem((label + "###" + p).c_str(), nullptr, flags)) {
                if (app.Focus != p && !moved) want = p;
                ImGui::EndTabItem();
            }
        }
        if (ImGui::BeginTabItem("+")) {
            for (const std::string &p : app.View->AvailableFiles)
                if (!app.Ws.IsOpen(p) && ImGui::Selectable(p.c_str())) want = p;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!want.empty() && want != app.Focus) app.WantFile = want;
    ImGui::TextUnformatted(app.Focus.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Save")) app.Save();
    if (!app.Conflict.empty()) ImGui::TextWrapped("%s", app.Conflict.c_str());
    if (app.View->Ticket != app.Requested) ImGui::TextUnformatted("compiling...");
    if (f && app.Fresh(*f)) {
        size_t shown = 0;
        for (const Diagnostic &diagnostic : app.Snap().Diags) {
            if (shown++ == 4) break;
            ImGui::TextWrapped("%s: %s", CodeName(diagnostic.Code).data(), diagnostic.Payload.c_str());
        }
    }
    ImGui::Separator();
    if (auto *buffer = app.Ws.Find(app.Focus)) {
        auto &draft = app.Drafts[app.Focus];
        draft.Sync(*buffer);
        const uint32_t before = draft.Cursor;
        const bool fresh = f && app.Fresh(*f);
        if (fresh && app.Reveal && (selection || !marks.empty())) {
            buffer->SetCursor(selection ? app.Sel.Caret : marks.front().Begin);
            draft.Sync(*buffer);
        }
        ImGui::PushID(app.Focus.c_str());
        const bool changed = app::DrawText("##program", draft, ImGui::GetContentRegionAvail(), fresh ? marks : std::span<const Span>{}, app.Reveal);
        ImGui::PopID();
        app.Reveal = false;
        if (draft.Commit(app.Ws, app.Focus)) app.Queue(true);
        else if (!changed && draft.Cursor != before && fresh) clicked = draft.Cursor;
    }
    ImGui::End();
    return clicked;
}

// Apply edits after both views read the current selection.
struct Intent {
    std::optional<Edit> Edit;
    bool Undo = false, Redo = false;
};

// Route edits globally and arrow keys to the focused window.
Intent HandleKeys(App &app, const FileView &f) {
    Intent in;
    // Reserve character keys for the active text field.
    if (app.Field.Open) return in;

    constexpr ImGuiInputFlags Global = ImGuiInputFlags_RouteGlobal;
    // Call Shortcut unconditionally to register each route every frame.
    const auto keys = app::ReadWorkspaceKeys();
    if (keys.Save) app.Save();
    if (keys.Undo || keys.Redo) {
        (keys.Redo ? in.Redo : in.Undo) = true;
        return in;
    }
    if (ImGui::GetIO().WantTextInput || !app.Fresh(f)) return in;
    const bool out = ImGui::Shortcut(ImGuiKey_UpArrow, ImGuiInputFlags_RouteFocused);
    const bool into = ImGui::Shortcut(ImGuiKey_DownArrow, ImGuiInputFlags_RouteFocused);
    const bool expand = ImGui::Shortcut(ImGuiKey_Space, Global);
    const bool materialize = ImGui::Shortcut(ImGuiKey_M, Global);
    const bool remove = ImGui::Shortcut(ImGuiKey_Delete, Global) | ImGui::Shortcut(ImGuiKey_Backspace, Global);
    const bool edit_text = ImGui::Shortcut(ImGuiKey_Enter, Global) | ImGui::Shortcut(ImGuiKey_KeypadEnter, Global);

    if (out) boxview::SelectOut(app.Sel, f);
    if (into) boxview::SelectIn(app.Sel, f);
    if (expand) app.ExpandSelection(false);
    if (materialize) app.ExpandSelection(true);
    if (remove) in.Edit = app::EditFor(app.Terms(), f, app.Sel, app::Key::Remove);
    if (edit_text) {
        const std::string_view text = app::TextOf(app.Terms(), f, app.Sel);
        if (!text.empty() && text.size() < sizeof app.Field.Text) {
            app.Field = {};
            app.Field.Open = app.Field.Focus = true;
            text.copy(app.Field.Text, text.size());
        }
    }
    const ImGuiIO &io = ImGui::GetIO();
    for (int i = 0; i < io.InputQueueCharacters.Size && !in.Edit; ++i) {
        const app::Key key = app::KeyForChar(io.InputQueueCharacters[i]);
        if (key != app::Key::None) in.Edit = app::EditFor(app.Terms(), f, app.Sel, key);
    }
    return in;
}

void HelpPane(App &app) {
    ImGui::Begin("help");
    const auto row = [](const char *key, const char *what) {
        ImGui::TextUnformatted(key);
        ImGui::SameLine(92 * ImGui::GetStyle().FontScaleDpi);
        ImGui::TextUnformatted(what);
    };

    ImGui::SeparatorText("selection");
    row("click", "a box or its text");
    row("up / down", "out / in");
    row("drag", "a route's ports, to wire or unwire");
    row("right-click", "a control, to mark what declares it");

    ImGui::SeparatorText("compose");
    ImGui::TextWrapped("With `a` selected:");
    for (const app::Connective &c : app::Connectives()) {
        const char key[2] = {c.Char, 0};
        row(key, app::ComposeExample(app.Terms(), c.Edit).c_str());
    }

    ImGui::SeparatorText("edit");
    row("delete", "remove the stage");
    row("enter", "a literal or label");
    row("cmd-z", "undo / redo, text and controls included");
    row("cmd-s", "save the current source file");

    ImGui::SeparatorText("evaluate");
    row("space", "expand, read-only");
    row("m", "materialize");

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Text and diagram edits share one history. Diagram edits rewrite terms "
        "and splice the result into the source."
    );
    ImGui::End();
}

std::optional<Edit> DrawField(App &app, const FileView &f, const boxview::Node *at, ImVec2 origin) {
    if (!app.Field.Open) return {};
    if (at != nullptr) ImGui::SetNextWindowPos({origin.x + at->Bounds.X, origin.y + at->Bounds.Bottom() + 2});
    ImGui::Begin(
        "##inline", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
    );
    if (app.Field.Focus) {
        ImGui::SetKeyboardFocusHere();
        app.Field.Focus = false;
    }
    ImGui::SetNextItemWidth(200 * ImGui::GetStyle().FontScaleDpi);
    const bool entered = ImGui::InputText("##text", app.Field.Text, sizeof app.Field.Text, ImGuiInputTextFlags_EnterReturnsTrue);
    const bool active = ImGui::IsItemActive();
    ImGui::End();
    app.Field.Armed = app.Field.Armed || active;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        app.Field = {};
        return {};
    }
    if (!entered && !(app.Field.Armed && !active)) return {};
    const Edit e = app::EditForText(app.Terms(), f, app.Sel, app.Field.Text);
    app.Field = {};
    return e;
}

void ControlPane(App &app) {
    ImGui::Begin("controls");
    const Artifact *art = app.Live.Current.get();
    if (!app.Stale.empty()) {
        ImGui::TextWrapped("hearing the last program that compiled: %s", app.Stale.c_str());
        ImGui::Separator();
    }
    if (!art) {
        ImGui::TextUnformatted("nothing has compiled yet, so the output is silence");
        ImGui::End();
        return;
    }
    const Interp &dsp = *art->Dsp;
    const audio::Host &host = app.Live.Host;
    if (host.Running) {
        ImGui::Text("%s, %.0f Hz", host.DeviceName.c_str(), host.SampleRate);
    } else {
        ImGui::TextUnformatted("no audio");
    }
    if (!app.AudioError.empty()) ImGui::TextWrapped("%s", app.AudioError.c_str());
    if (!host.Warning.empty()) ImGui::TextWrapped("%s", host.Warning.c_str());
    if (app.Last.Compiled) ImGui::Text("%.1f ms to prepare%s", app.Last.Timings.Total, app.Last.Unchanged ? ", unchanged" : "");
    if (host.Running && (host.DeviceIn != dsp.Inputs() || host.DeviceOut != dsp.Outputs()))
        ImGui::Text("%d in %d out, device has %d and %d", dsp.Inputs(), dsp.Outputs(), host.DeviceIn, host.DeviceOut);
    for (const std::string &d : dsp.Diagnostics) ImGui::TextWrapped("%s", d.c_str());
    for (const Diagnostic &d : art->Diags) ImGui::TextWrapped("%s: %s", d.Severity == Severity::Error ? "error" : "warning", d.Payload.c_str());
    if (!app.Traced.Control.empty()) {
        const std::string what = std::filesystem::path(app.Traced.Path).filename().string();
        if (!app.Traced) ImGui::TextWrapped("%s: nothing the compile parsed declares it", app.Traced.Control.c_str());
        else if (app.Traced.Controls > 1)
            ImGui::TextWrapped("%s is declared in %s, along with %zu other controls", app.Traced.Control.c_str(), what.c_str(), app.Traced.Controls - 1);
        else ImGui::TextWrapped("%s is declared in %s", app.Traced.Control.c_str(), what.c_str());
    }
    ImGui::Separator();
    // Commit a control gesture after the drag ends.
    const controls::Report r = controls::Draw(art->Plan, art->Ui, *art->Dsp, app.Ws.Controls);
    if (r.Ended) app.Ws.CommitGesture(app.Ws.Controls);
    if (r.Traced) app.TraceBack(*r.Traced);
    ImGui::End();
}

// Build the initial layout once to keep the diagram visible.
void BuildDefaultLayout(ImGuiID dock) {
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->Size);
    ImGuiID side = 0;
    const ImGuiID main = ImGui::DockBuilderSplitNode(dock, ImGuiDir_Left, 0.72f, nullptr, &side);
    ImGuiID diagram = 0;
    const ImGuiID source = ImGui::DockBuilderSplitNode(main, ImGuiDir_Up, 0.35f, nullptr, &diagram);
    ImGuiID controls = 0;
    const ImGuiID help = ImGui::DockBuilderSplitNode(side, ImGuiDir_Up, 0.45f, nullptr, &controls);
    ImGui::DockBuilderDockWindow("source", source);
    ImGui::DockBuilderDockWindow("diagram", diagram);
    ImGui::DockBuilderDockWindow("help", help);
    ImGui::DockBuilderDockWindow("controls", controls);
    ImGui::DockBuilderFinish(dock);
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::println("usage: {} <file.dsp>", argv[0]);
        return 2;
    }

    const auto fail = [](const char *what) {
        std::println("{}: {}", what, SDL_GetError());
        return 1;
    };
    if (!SDL_Init(SDL_INIT_VIDEO)) return fail("SDL_Init");
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_Window *window = SDL_CreateWindow("FaustLens", int(1280 * scale), int(800 * scale), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) return fail("SDL_CreateWindow");
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, true, nullptr);
    if (!gpu || !SDL_ClaimWindowForGPUDevice(gpu, window)) return fail("SDL_GPU");
    SDL_SetGPUSwapchainParameters(gpu, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    // Disable ini output in the launch directory.
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo init{};
    init.Device = gpu;
    init.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu, window);
    init.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init);

    App app;
    app.Load(argv[1]);

    bool laid_out = false;
    for (bool done = false; !done;) {
        app.PollForEdits();
        // Keep SDL text input active for SDL_EVENT_TEXT_INPUT delivery.
        if (!SDL_TextInputActive(window)) SDL_StartTextInput(window);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window))) done = true;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiID dock = ImGui::DockSpaceOverViewport();
        if (!laid_out) {
            laid_out = true;
            BuildDefaultLayout(dock);
        }

        app.Sync();
        const FileView *f = app.Snap().File(app.Path);
        const FileView *src = app.Snap().File(app.Focus);
        if (f && src) {
            boxview::Layout layout(app.Terms(), boxview::Metrics{});
            layout.Expansions = app.View->Expanded;
            const boxview::Node root = layout.Run(f->Refs, ProcessBodyRef(app.Terms(), *f));
            if (app.ResolveSelection) {
                app.ResolveSelection = false;
                app.Sel = boxview::SelectAt(*src, root, src == f ? ProcessBodyRef(app.Terms(), *f) : NoRef, app.Sel.Caret);
            }

            ImGui::Begin("diagram");
            // Register shortcuts inside their owning window.
            Intent intent = HandleKeys(app, *src);
            if (!app.Refused.empty()) {
                ImGui::TextUnformatted(app.Refused.c_str());
                ImGui::Separator();
            }
            // Position hit regions after drawing the refusal message.
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float mx = m.x - at.x, my = m.y - at.y;
            if (app.Fresh(*f) && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // Test ports before their enclosing stages.
                const boxview::Layout::Endpoint end = boxview::Layout::PortAt(root, mx, my, boxview::PortReach);
                std::vector<uint32_t> hit;
                if (end) hit = PathToNode(root, *end.Node);
                if (end || boxview::Layout::HitPath(root, mx, my, hit)) {
                    app.OpenFile(app.Path);
                    src = f;
                    if (end) app.Drag = {true, hit, end.Port->Input, end.Port->Channel, m};
                    app.Sel = boxview::SelectPath(*f, root, ProcessBodyRef(app.Terms(), *f), hit);
                    app.Traced = {};
                    app.Reveal = true;
                }
            }
            // End drags released outside the window.
            if (app.Drag.Active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                const boxview::Layout::Endpoint end = boxview::Layout::PortAt(root, mx, my, boxview::PortReach);
                if (end && end.Port->Input != app.Drag.Input && PathToNode(root, *end.Node) == app.Drag.At) {
                    const uint32_t in = app.Drag.Input ? app.Drag.Channel : end.Port->Channel;
                    const uint32_t out = app.Drag.Input ? end.Port->Channel : app.Drag.Channel;
                    intent.Edit = app::RewireDrag(app.Terms(), *src, app.Sel, in, out);
                }
                app.Drag = {};
            }

            const ValueId selected = app.Sel.Value();
            const boxview::Node *here =
                selected == NoTerm || src != f ? nullptr : boxview::SelectedNode(*src, root, ProcessBodyRef(app.Terms(), *src), app.Sel);
            boxview::Draw(ImGui::GetWindowDrawList(), root, at.x, at.y, here);
            if (app.Drag.Active) ImGui::GetWindowDrawList()->AddLine(app.Drag.From, m, boxview::Palette{}.Link, 1.5f);
            ImGui::Dummy({root.Bounds.W, root.Bounds.H});
            ImGui::End();

            if (app.Fresh(*src))
                if (const std::optional<Edit> e = DrawField(app, *src, here, at)) intent.Edit = e;

            const std::vector<Span> marks = app.Traced ? app::TraceMarks(*src, app.Traced) : Marks(*src, selected);
            if (const auto clicked = SourcePane(app, src, marks, !app.Traced && boxview::SelectedRef(*src, app.Sel) != NoRef)) {
                app.Sel = boxview::SelectAt(*src, root, src == f ? ProcessBodyRef(app.Terms(), *f) : NoRef, *clicked);
                app.Traced = {};
                app.Reveal = false;
            }

            // Reject structural edits if typing changed the source this frame.
            if (intent.Undo || intent.Redo) app.Undo(intent.Redo);
            else if (intent.Edit) app.ApplyEdit(*intent.Edit);
        } else {
            SourcePane(app, src, {}, false);
            const auto keys = app::ReadWorkspaceKeys();
            if (keys.Save) app.Save();
            if (keys.Undo || keys.Redo) app.Undo(keys.Redo);
        }
        if (!app.WantFile.empty()) {
            app.OpenFile(app.WantFile);
            app.WantFile.clear();
        }
        HelpPane(app);
        ControlPane(app);

        ImGui::Render();
        ImDrawData *draw = ImGui::GetDrawData();
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
        SDL_GPUTexture *swap = nullptr;
        SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, nullptr, nullptr);
        if (swap && draw->DisplaySize.x > 0 && draw->DisplaySize.y > 0) {
            ImGui_ImplSDLGPU3_PrepareDrawData(draw, cmd);
            const SDL_GPUColorTargetInfo target{
                .texture = swap, .clear_color = {0.10f, 0.11f, 0.12f, 1.0f}, .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE
            };
            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(draw, cmd, pass);
            SDL_EndGPURenderPass(pass);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
    }

    // Stop callbacks before destroying the window and DSP artifacts.
    app.Compiler.Stop();
    app.Live.Host.Stop();

    SDL_WaitForGPUIdle(gpu);
    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
