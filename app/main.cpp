#include "Edits.h"
#include "Expand.h"
#include "Live.h"
#include "Trace.h"
#include "boxview/Draw.h"
#include "boxview/Layout.h"
#include "boxview/Select.h"
#include "controls/Draw.h"
#include "editor/Workspace.h"
#include "query/Query.h"
#include "query/Snapshot.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "imgui_internal.h" // the docking builder, for the first-run layout only
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <vector>

using namespace faustlens;
using faustlens::app::Artifact;

namespace {

struct InlineField {
    bool Open = false;
    bool Focus = false;
    bool Armed = false; // has held focus, so losing it is a blur
    char Text[128] = {};
};

// `at` is a child-index path, not a value: two identically written routes are
// one interned value.
struct PortDrag {
    bool Active = false;
    std::vector<uint32_t> At;
    bool Input = false;
    uint32_t Channel = 0;
    ImVec2 From{};
};

struct App {
    Session Session;
    std::string Path; // compiled from and drawn
    std::string Focus; // what the source pane shows
    app::Workspace Ws;
    std::map<std::string, std::string> OnDisk;
    Snapshot Snap;
    boxview::Selection Sel;
    InlineField Field;
    PortDrag Drag;
    std::vector<ValueId> Open;
    bool ResolveSelection = false;
    // What the edit catalogue refused, and what an external write did.
    std::string Refused, Conflict;
    // Consumed at frame end: opening a file republishes the snapshot being drawn.
    std::string WantFile;
    // What the tab bar last agreed with, so a `focus` change elsewhere can force it.
    std::string TabFocus;
    // Not a `Selection`: a library slider is no stage of `process`.
    app::Trace Traced;
    bool Reveal = false;

    app::Live Live;
    // Why the audio is older than what is on screen.
    std::string Stale;
    // Why no device opened, which outlives the failed `Host::Start` that reported it.
    std::string AudioError;
    app::Live::Result Last;
    std::map<std::string, std::filesystem::file_time_type> Watching;

    void Load(const std::string &p) {
        Path = Focus = p;
        std::string text = ReadFile(Path).value_or(std::string());
        OnDisk[Path] = text;
        // Opening is not an edit, so it records no undo step.
        Ws.Open(Path, std::move(text));
        Session.SetBuffer(Path, Ws.Find(Path)->Text());
        Republish();
    }

    // On demand: publishing every import would copy `stdfaust.lib` per keystroke.
    void OpenFile(const std::string &p) {
        if (Ws.IsOpen(p)) {
            Focus = p;
            return;
        }
        {
            const std::optional<std::string_view> text = Session.Vfs.Read(p);
            if (!text) return;
            Ws.Open(p, std::string(*text));
            OnDisk[p] = std::string(*text);
        }
        Focus = p;
        Publish();
    }

    bool Dirty(const std::string &p) const {
        const app::Buffer *b = Ws.Find(p);
        const auto it = OnDisk.find(p);
        return b != nullptr && it != OnDisk.end() && b->Text() != it->second;
    }

    // Rebuilt after every compile: an edit can add or remove an import.
    void Watch() {
        Watching.clear();
        for (const std::string &p : Session.Parsed()) {
            std::error_code ec;
            const auto when = std::filesystem::last_write_time(p, ec);
            if (!ec) Watching.emplace(p, when);
        }
    }

    void Publish() {
        Session.Process(Path);
        Snap = ::Publish(Session, Ws.Paths());
    }

    // A splice takes the source its links were computed against, so a lagging
    // snapshot corrupts the file.
    void Sync() {
        for (const std::string &p : Ws.Paths()) {
            const FileView *f = Snap.File(p);
            const app::Buffer *b = Ws.Find(p);
            if (f == nullptr || b == nullptr || f->Text != b->Text()) {
                Publish();
                return;
            }
        }
    }

    void Republish() {
        Publish();
        Reload();
    }

    void Reload() {
        const app::Live::Result r = Live.Reload(Session, Path, Ws.Controls);
        Last = r;
        if (r.Compiled && !Live.Host.Running && Live.Current.get()) {
            auto started = Live.Host.Start(*Live.Current->Dsp);
            AudioError = started ? std::string() : std::move(started).error();
        }
        Stale = r.Compiled ? std::string() : r.Why;
        Watch();
    }

    // A clean buffer takes an external write, a dirty one reports a conflict.
    void PollForEdits() {
        Live.Collect();
        bool moved = false;
        for (auto &[p, when] : Watching) {
            std::error_code ec;
            const auto now = std::filesystem::last_write_time(p, ec);
            if (ec || now == when) continue;
            if (Ws.IsOpen(p) && Dirty(p)) {
                when = now; // reported once, not once per frame
                Conflict = "changed on disk, and this buffer has unsaved edits";
                continue;
            }
            moved = true;
            if (Ws.IsOpen(p)) {
                if (const std::optional<std::string_view> t = Session.Vfs.Read(p)) {
                    Session.Touch(p);
                    Ws.Open(p, std::string(*t));
                    OnDisk[p] = Ws.Find(p)->Text();
                    Session.SetBuffer(p, Ws.Find(p)->Text());
                }
            } else {
                Session.Touch(p);
            }
        }
        if (!moved) return;
        Conflict.clear();
        Republish();
    }

    void ApplyEdit(const Edit &e) {
        const FileView *f = Snap.File(Focus);
        if (f == nullptr) return;
        if (e.Target == NoRef) {
            Refused = e.Declined == nullptr ? "" : e.Declined;
            return;
        }
        Refused.clear();
        // Expansion is keyed on a value id, and the rewrite replaces the value.
        std::erase(Open, f->Refs.Refs[e.Target].ValueId);
        // The cursor rides the script, which is how the selection survives.
        app::Buffer *b = Ws.Find(Focus);
        if (b == nullptr) return;
        b->SetCursor(Sel.Caret);
        if (!app::Apply(Session, Ws, Focus, *f, e)) return;
        Sel.Caret = b->Cursor;
        ResolveSelection = true;
        Republish();
    }

    void ToggleExpand() {
        const ValueId v = Sel.Value();
        if (v == NoTerm) return;
        if (std::erase(Open, v) != 0) {
            Refused.clear();
            return;
        }
        const app::Expansion e = app::Expand(Session, v);
        if (!e) {
            Refused = e.Declined == nullptr ? "" : e.Declined;
            return;
        }
        Refused = e.Ambiguous ? "this is evaluated in more than one context; showing the first" : std::string();
        Open.push_back(v);
    }

    void Undo(bool redo) {
        const app::Workspace::Step u = redo ? Ws.Redo() : Ws.Undo();
        if (!u) return;
        if (u.Texts.empty()) {
            // A control move derives nothing below text, so the store goes
            // straight in. `Apply` is total, so no half-written reset is audible.
            if (Artifact *a = Live.Current.get()) controls::Apply(Ws.Controls, a->Plan, a->Ui, *a->Dsp);
            return;
        }
        for (const std::string &p : u.Texts) Session.SetBuffer(p, Ws.Find(p)->Text());
        if (const app::Buffer *b = Ws.Find(Focus)) Sel.Caret = b->Cursor;
        ResolveSelection = true;
        Republish();
    }

    // The file is requested rather than opened: opening republishes mid-frame.
    void TraceBack(uint32_t widget_label) {
        const Artifact *a = Live.Current.get();
        if (a == nullptr) return;
        Traced = app::TraceControl(Session, a->Plan, widget_label);
        if (!Traced) return;
        WantFile = Traced.Path;
        Reveal = true;
    }
};

// Taken at the node's centre: a port sits *on* the edge.
std::vector<uint32_t> PathToNode(const boxview::Node &root, const boxview::Node &n) {
    std::vector<uint32_t> path;
    boxview::Layout::HitPath(root, n.Bounds.X + n.Bounds.W / 2, n.Bounds.Y + n.Bounds.H / 2, path);
    return path;
}

std::optional<uint32_t> SourcePane(App &app, const FileView &f, std::span<const Span> marks, std::optional<Span> here) {
    std::optional<uint32_t> clicked;
    ImGui::Begin("source");
    std::string want;
    const bool moved = app.TabFocus != app.Focus;
    app.TabFocus = app.Focus;
    if (ImGui::BeginTabBar("##files")) {
        for (const std::string &p : app.Ws.Paths()) {
            const std::string label = std::filesystem::path(p).filename().string() + (app.Dirty(p) ? " *" : "");
            bool open = true;
            const ImGuiTabItemFlags flags = moved && p == app.Focus ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem((label + "###" + p).c_str(), p == app.Path ? nullptr : &open, flags)) {
                if (app.Focus != p && !moved) want = p;
                ImGui::EndTabItem();
            }
            // Closing the program's own file would close the program.
            if (!open && p != app.Path) want = app.Path;
        }
        if (ImGui::BeginTabItem("+")) {
            for (const std::string &p : app.Session.Parsed()) {
                if (app.Ws.IsOpen(p)) continue;
                if (ImGui::Selectable(p.c_str())) want = p;
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!want.empty() && want != app.Focus) app.WantFile = want;
    ImGui::Text("%s%s", f.Path.c_str(), app.Dirty(f.Path) ? " *" : "");
    if (!app.Conflict.empty()) ImGui::TextWrapped("%s", app.Conflict.c_str());
    ImGui::Separator();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float line_h = ImGui::GetTextLineHeight();
    std::optional<uint32_t> target;
    if (app.Reveal && !marks.empty()) target = here ? here->Begin : marks.front().Begin;
    uint32_t offset = 0;
    while (offset <= f.Text.size()) {
        const size_t nl = f.Text.find('\n', offset);
        const size_t end = nl == std::string::npos ? f.Text.size() : nl;
        const std::string_view row(f.Text.data() + offset, end - offset);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        for (const Span &sp : marks) {
            if (sp.End <= offset || sp.Begin > end) continue;
            const uint32_t b = std::max<uint32_t>(sp.Begin, offset);
            const uint32_t e = std::min<uint32_t>(sp.End, uint32_t(end));
            const float x0 = ImGui::CalcTextSize(f.Text.data() + offset, f.Text.data() + b).x;
            const float x1 = ImGui::CalcTextSize(f.Text.data() + offset, f.Text.data() + e).x;
            // Only one occurrence is where a key press lands, so it is brighter.
            const bool selected = !here || (here->Begin == sp.Begin && here->End == sp.End);
            dl->AddRectFilled({at.x + x0, at.y}, {at.x + x1, at.y + line_h}, selected ? 0x552F5A80 : 0x222F5A80);
        }
        ImGui::TextUnformatted(row.data(), row.data() + row.size());
        // Scrolled against the line just drawn: computing from `line_h` lands short.
        if (target && *target >= offset && *target <= end) {
            target.reset();
            app.Reveal = false;
            ImGui::SetScrollHereY(0.35f);
        }
        if (ImGui::IsItemClicked()) {
            const float dx = ImGui::GetIO().MousePos.x - at.x;
            uint32_t col = 0;
            while (col < row.size() && ImGui::CalcTextSize(row.data(), row.data() + col + 1).x < dx) ++col;
            clicked = offset + col;
        }
        if (nl == std::string::npos) break;
        offset = uint32_t(nl) + 1;
    }
    ImGui::End();
    return clicked;
}

// Returned rather than applied: applying republishes the snapshot being drawn.
struct Intent {
    std::optional<Edit> Edit;
    bool Undo = false, Redo = false;
};

// Edit keys route globally, the selection being shared, arrows to the focused window.
Intent HandleKeys(App &app, const FileView &f) {
    Intent in;
    // Ahead of routing: while the field is up, a bare `M` is a character it wants.
    if (app.Field.Open) return in;

    constexpr ImGuiInputFlags Global = ImGuiInputFlags_RouteGlobal;
    // Never short-circuited: `Shortcut` registers the route as a side effect, so
    // a call skipped by `||` lapses for the frame.
    const bool undo = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, Global);
    const bool redo = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, Global);
    const bool out = ImGui::Shortcut(ImGuiKey_UpArrow, ImGuiInputFlags_RouteFocused);
    const bool into = ImGui::Shortcut(ImGuiKey_DownArrow, ImGuiInputFlags_RouteFocused);
    const bool expand = ImGui::Shortcut(ImGuiKey_Space, Global);
    const bool materialize = ImGui::Shortcut(ImGuiKey_M, Global);
    const bool remove = ImGui::Shortcut(ImGuiKey_Delete, Global) | ImGui::Shortcut(ImGuiKey_Backspace, Global);
    const bool edit_text = ImGui::Shortcut(ImGuiKey_Enter, Global) | ImGui::Shortcut(ImGuiKey_KeypadEnter, Global);

    if (undo || redo) {
        (redo ? in.Redo : in.Undo) = true;
        return in;
    }
    if (out) boxview::SelectOut(app.Sel, f);
    if (into) boxview::SelectIn(app.Sel, f);
    if (expand) app.ToggleExpand();
    if (materialize) in.Edit = app::Materialize(app.Session, f, app.Sel);
    if (remove) in.Edit = app::EditFor(app.Session.Terms, f, app.Sel, app::Key::Remove);
    if (edit_text) {
        const std::string_view text = app::TextOf(app.Session.Terms, f, app.Sel);
        if (!text.empty() && text.size() < sizeof app.Field.Text) {
            app.Field = {};
            app.Field.Open = app.Field.Focus = true;
            text.copy(app.Field.Text, text.size());
        }
    }
    // The connectives are characters, not chords, so `WantTextInput` stands in for routing.
    const ImGuiIO &io = ImGui::GetIO();
    if (io.WantTextInput) return in;
    for (int i = 0; i < io.InputQueueCharacters.Size && !in.Edit; ++i) {
        const app::Key key = app::KeyForChar(io.InputQueueCharacters[i]);
        if (key != app::Key::None) in.Edit = app::EditFor(app.Session.Terms, f, app.Sel, key);
    }
    return in;
}

// What `HandleKeys` binds, said out loud. Nothing checks that the two agree.
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
        row(key, app::ComposeExample(app.Session.Terms, c.Edit).c_str());
    }

    ImGui::SeparatorText("edit");
    row("delete", "remove the stage");
    row("enter", "a literal or label");
    row("cmd-z", "undo / redo, moves included");

    ImGui::SeparatorText("evaluate");
    row("space", "expand, read-only");
    row("m", "materialize");

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Every edit is a term rewrite plus a splice, so the source changes on each "
        "and the box view has no private edit path."
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
    const Edit e = app::EditForText(app.Session.Terms, f, app.Sel, app.Field.Text);
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
    if (app.Last.Compiled) ImGui::Text("%.1f ms to reload%s", app.Last.Timings.Total, app.Last.Unchanged ? ", unchanged" : "");
    // The channel mapping is positional, so a mismatched count is silence on one
    // side or a dropped channel on the other.
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
    // One gesture is one undo entry: pushed mid-drag it records a stale value.
    const controls::Report r = controls::Draw(art->Plan, art->Ui, *art->Dsp, app.Ws.Controls);
    if (r.Ended) app.Ws.CommitGesture(app.Ws.Controls);
    if (r.Traced) app.TraceBack(*r.Traced);
    ImGui::End();
}

// Built once per run: otherwise the diagram opens behind the source.
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
    // No `imgui.ini`: it would land in whatever directory the app was launched from.
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
        // SDL3 delivers `SDL_EVENT_TEXT_INPUT` only while text input is active, which the
        // ImGui backend keeps turning off.
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
        // Usually the same file: `f` is compiled and drawn, `src` is shown.
        const FileView *f = app.Snap.File(app.Path);
        const FileView *src = app.Snap.File(app.Focus);
        if (f && src) {
            const ValueId body = ProcessBody(app.Session.Terms, f->Root);
            boxview::Layout layout(app.Session.Terms, boxview::Metrics{});
            layout.Expansions = app::ExpandAll(app.Session, app.Open);
            const boxview::Node root = layout.Run(body);
            // A byte offset resolves against the drawn tree, so re-resolve here.
            if (app.ResolveSelection) {
                app.ResolveSelection = false;
                app.Sel = boxview::SelectAt(*src, root, app.Sel.Caret);
            }

            ImGui::Begin("diagram");
            // Inside the window, since a key route is owned by one.
            Intent intent = HandleKeys(app, *src);
            if (!app.Refused.empty()) {
                ImGui::TextUnformatted(app.Refused.c_str());
                ImGui::Separator();
            }
            // After the message above, or a refusal offsets every click by its height.
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float mx = m.x - at.x, my = m.y - at.y;
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // A port is inside a stage, so it is asked first.
                const boxview::Layout::Endpoint end = boxview::Layout::PortAt(root, mx, my, boxview::PortReach);
                std::vector<uint32_t> hit;
                if (end) hit = PathToNode(root, *end.Node);
                if (end || boxview::Layout::HitPath(root, mx, my, hit)) {
                    app.OpenFile(app.Path);
                    src = f;
                    if (end) app.Drag = {true, hit, end.Port->Input, end.Port->Channel, m};
                    app.Sel = boxview::SelectPath(*f, root, ProcessBodyRef(app.Session.Terms, *f), hit);
                    app.Traced = {};
                    app.Reveal = true;
                }
            }
            // Not guarded by hover: a drag let go outside the window has ended.
            if (app.Drag.Active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                const boxview::Layout::Endpoint end = boxview::Layout::PortAt(root, mx, my, boxview::PortReach);
                if (end && end.Port->Input != app.Drag.Input && PathToNode(root, *end.Node) == app.Drag.At) {
                    const uint32_t in = app.Drag.Input ? app.Drag.Channel : end.Port->Channel;
                    const uint32_t out = app.Drag.Input ? end.Port->Channel : app.Drag.Channel;
                    intent.Edit = app::RewireDrag(app.Session.Terms, *src, app.Sel, in, out);
                }
                app.Drag = {};
            }

            const std::vector<ValueId> path = boxview::Layout::PathTo(root, app.Sel.Value());
            const ValueId selected = path.empty() ? NoTerm : app.Sel.Value();
            // The occurrence, not the value: one box of many is where an edit lands.
            const boxview::Node *here = path.empty() ? nullptr : boxview::SelectedNode(*src, root, ProcessBodyRef(app.Session.Terms, *src), app.Sel);
            boxview::Draw(ImGui::GetWindowDrawList(), root, at.x, at.y, here, path);
            if (app.Drag.Active) ImGui::GetWindowDrawList()->AddLine(app.Drag.From, m, boxview::Palette{}.Link, 1.5f);
            ImGui::Dummy({root.Bounds.W, root.Bounds.H});
            ImGui::End();

            if (const std::optional<Edit> e = DrawField(app, *src, here, at)) intent.Edit = e;

            // `marks` is every occurrence, `at_ref` the one an edit rewrites.
            const RefId sr = boxview::SelectedRef(*src, app.Sel);
            std::optional<Span> at_ref;
            if (!app.Traced && sr != NoRef) at_ref = Span{src->Refs.Refs[sr].SpanBegin, src->Refs.Refs[sr].SpanEnd};
            const std::vector<Span> marks = app.Traced ? app::TraceMarks(*src, app.Traced) : Marks(*src, selected);
            if (const auto clicked = SourcePane(app, *src, marks, at_ref)) {
                app.Sel = boxview::SelectAt(*src, root, *clicked);
                app.Traced = {};
                app.Reveal = false;
            }

            // Last, because each of these republishes and invalidates `f`.
            if (intent.Undo || intent.Redo) app.Undo(intent.Redo);
            else if (intent.Edit) app.ApplyEdit(*intent.Edit);
            else if (!app.WantFile.empty()) {
                app.OpenFile(app.WantFile);
                // The caret's byte belongs to the file that is no longer shown.
                app.Sel = {};
            }
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

    // Before the window goes, while the instance it runs is still alive.
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
