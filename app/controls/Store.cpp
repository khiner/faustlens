#include "controls/Store.h"

#include "controls/Style.h"
#include "runtime/Interp.h"

namespace faustlens::controls {

Restorable RestorableAs(UiKind k) {
    switch (k) {
        case UiKind::HSlider:
        case UiKind::VSlider:
        case UiKind::NumEntry: return Restorable::Continuous;
        case UiKind::Checkbox: return Restorable::Toggle;
        case UiKind::Button:
        case UiKind::VBargraph:
        case UiKind::HBargraph:
        case UiKind::Soundfile: break;
    }
    return Restorable::No;
}

void Record(Values &store, std::string_view path, const UiNode &n, double v) {
    const Restorable as = RestorableAs(n.Kind);
    if (as == Restorable::No || path.empty()) return;
    store.insert_or_assign(std::string(path), Value{v, as});
}

void Apply(const Values &store, const Plan &p, const UiNode &tree, Interp &dsp) {
    ForEachWidget(tree, [&](const UiNode &n) {
        const Restorable as = RestorableAs(n.Kind);
        if (as == Restorable::No) return true;
        const std::string_view path = p.Label(n.WidgetLabel);
        if (path.empty()) return true;
        const auto it = store.find(path);
        // A class mismatch counts as nothing stored, not as nothing to write.
        if (it == store.end() || it->second.As != as) {
            dsp.SetControl(n.WidgetLabel, n.Init);
            return true;
        }
        // `checkbox` carries no `min`/`max`, so `Quantize` would clamp it to 0.
        dsp.SetControl(n.WidgetLabel, as == Restorable::Toggle ? (it->second.V != 0 ? 1.0 : 0.0) : Quantize(n, it->second.V));
        return true;
    });
}

} // namespace faustlens::controls
