#include "controls/Draw.h"

#include "controls/Style.h"
#include "runtime/Interp.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace faustlens::controls {

namespace {

constexpr float KnobDiameter = 46.0f, VerticalHeight = 120.0f;

// The value text is handed to ImGui as a `printf` format, so `%` must escape.
std::string AsFormat(const std::string &s) {
    std::string out;
    for (const char c : s) {
        out += c;
        if (c == '%') out += c;
    }
    return out;
}

struct Turn {
    bool Moved = false;
    bool Released = false;
};

// A rotary, since ImGui has none. `*t` is a normalized position.
Turn Knob(const char *id, float *t, const char *text) {
    const float d = KnobDiameter * ImGui::GetFontSize() / 13.0f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, {d, d});
    const bool active = ImGui::IsItemActive();
    // Before the label, since `IsItemDeactivated` answers about the last item.
    Turn turn;
    turn.Released = ImGui::IsItemDeactivated();
    if (active) {
        const float dy = ImGui::GetIO().MouseDelta.y;
        if (dy != 0) {
            const float span = ImGui::GetIO().KeyShift ? 1000.0f : 200.0f;
            *t = std::clamp(*t - dy / span, 0.0f, 1.0f);
            turn.Moved = true;
        }
    }

    // 3pi/4 and 3pi/2: three quarters of a turn, opening downward.
    constexpr float Start = 2.356194f, Sweep = 4.712389f;
    const ImVec2 c{at.x + d / 2, at.y + d / 2};
    const float r = d / 2 - 2;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 track = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 fill = ImGui::GetColorU32(active ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab);
    dl->PathArcTo(c, r, Start, Start + Sweep, 32);
    dl->PathStroke(track, 0, 3.0f);
    dl->PathArcTo(c, r, Start, Start + Sweep * *t, 32);
    dl->PathStroke(fill, 0, 3.0f);
    const float a = Start + Sweep * *t;
    dl->AddLine(c, {c.x + std::cos(a) * r, c.y + std::sin(a) * r}, fill, 2.0f);

    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(text);
    ImGui::EndGroup();
    return turn;
}

struct Surface {
    const Plan &Plan;
    Interp &Dsp;
    Values &Store;
    Report Report;
};

void DrawWidget(const UiNode &n, Surface &s) {
    const Style st = StyleOf(n);
    if (st.Hidden) return;
    const float h = VerticalHeight * ImGui::GetFontSize() / 13.0f;

    ImGui::PushID(int(n.WidgetLabel));
    // Grouped so the trace-back and tooltip below get one rect for every kind.
    ImGui::BeginGroup();
    Interp &dsp = s.Dsp;
    const std::string_view path = s.Plan.Label(n.WidgetLabel);
    const double value = dsp.Control(n.WidgetLabel);
    const std::string text = Format(n, st, value);
    // Every write goes through here, so the store cannot fall behind.
    const auto write = [&](double v) {
        dsp.SetControl(n.WidgetLabel, v);
        Record(s.Store, path, n, v);
    };

    switch (n.Kind) {
        case UiKind::Button: {
            ImGui::Button(n.Label.c_str());
            // Held, not toggled: 1 only while the mouse is down, so never stored.
            dsp.SetControl(n.WidgetLabel, ImGui::IsItemActive() ? 1.0 : 0.0);
            break;
        }
        case UiKind::Checkbox: {
            bool on = value != 0;
            if (ImGui::Checkbox(n.Label.c_str(), &on)) write(on ? 1.0 : 0.0);
            s.Report.Ended |= ImGui::IsItemDeactivated();
            break;
        }
        case UiKind::VSlider:
        case UiKind::HSlider:
        case UiKind::NumEntry: {
            // Normalized, so `[scale:log]` is the program's curve not ImGui's.
            float t = float(ToPosition(n, st.Scale, value));
            Turn turn;
            if (st.Knob) {
                turn = Knob("##k", &t, (n.Label + "\n" + text).c_str());
            } else if (n.Kind == UiKind::VSlider) {
                turn.Moved = ImGui::VSliderFloat("##v", {ImGui::GetFrameHeight(), h}, &t, 0.0f, 1.0f, AsFormat(text).c_str());
                turn.Released = ImGui::IsItemDeactivated();
                ImGui::SameLine();
                ImGui::TextUnformatted(n.Label.c_str());
            } else {
                ImGui::TextUnformatted(n.Label.c_str());
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-FLT_MIN);
                turn.Moved = ImGui::SliderFloat("##h", &t, 0.0f, 1.0f, AsFormat(text).c_str());
                turn.Released = ImGui::IsItemDeactivated();
            }
            if (turn.Moved) write(Quantize(n, ToValue(n, st.Scale, t)));
            s.Report.Ended |= turn.Released;
            break;
        }
        case UiKind::VBargraph:
        case UiKind::HBargraph: {
            const float t = float(ToPosition(n, st.Scale, value));
            if (n.Kind == UiKind::VBargraph) {
                const float w = ImGui::GetFrameHeight();
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::Dummy({w, h});
                ImDrawList *dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(at, {at.x + w, at.y + h}, ImGui::GetColorU32(ImGuiCol_FrameBg));
                dl->AddRectFilled({at.x, at.y + h * (1 - t)}, {at.x + w, at.y + h}, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
                ImGui::SameLine();
                ImGui::Text("%s\n%s", n.Label.c_str(), text.c_str());
            } else {
                ImGui::TextUnformatted(n.Label.c_str());
                ImGui::SameLine();
                ImGui::ProgressBar(t, {-FLT_MIN, 0}, text.c_str()); // an overlay, not a format
            }
            break;
        }
        case UiKind::Soundfile:
            ImGui::BeginDisabled();
            ImGui::Text("%s (soundfile)", n.Label.c_str());
            ImGui::EndDisabled();
            break;
    }
    ImGui::EndGroup();
    // Trace-back on the right button, since a left press moves the widget.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) s.Report.Traced = n.WidgetLabel;
    if (!st.Tooltip.empty()) ImGui::SetItemTooltip("%s", st.Tooltip.c_str());
    ImGui::PopID();
}

void DrawNode(const UiNode &n, Surface &s, bool root);

// A tab group is not a layout: each child is a page.
void DrawChildren(const UiNode &n, Surface &s) {
    if (n.Orient == 2) {
        if (ImGui::BeginTabBar("##t")) {
            for (const UiNode &c : n.Children) {
                if (!c.IsGroup && StyleOf(c).Hidden) continue;
                if (ImGui::BeginTabItem(c.Label.empty() ? "##" : c.Label.c_str())) {
                    DrawNode(c, s, true);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        return;
    }
    bool first = true;
    for (const UiNode &c : n.Children) {
        if (!c.IsGroup && StyleOf(c).Hidden) continue;
        if (n.Orient == 1 && !first) ImGui::SameLine();
        first = false;
        ImGui::BeginGroup();
        DrawNode(c, s, false);
        ImGui::EndGroup();
    }
}

void DrawNode(const UiNode &n, Surface &s, bool root) {
    if (!n.IsGroup) {
        DrawWidget(n, s);
        return;
    }
    // The root's label is the program name the window already shows.
    if (!root && !n.Label.empty()) ImGui::SeparatorText(n.Label.c_str());
    ImGui::PushID(n.Label.c_str());
    DrawChildren(n, s);
    ImGui::PopID();
}

} // namespace

Report Draw(const Plan &plan, const UiNode &ui, Interp &dsp, Values &store) {
    Surface s{plan, dsp, store};
    DrawNode(ui, s, true);
    return s.Report;
}

} // namespace faustlens::controls
