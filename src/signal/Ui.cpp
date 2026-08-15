#include "signal/Ui.h"

#include "eval/Eval.h"

#include <algorithm>
#include <format>
#include <span>

namespace faustlens {

namespace {

// Separate from `UiNode`: insertion order is not emission order, so the tree sorts once.
struct Folder {
    uint8_t Orient = 0;
    std::string Raw;
    // A group already present is entered, where a widget is always added.
    std::vector<std::pair<std::string, size_t>> Order; // raw label -> index
    std::vector<Folder> Groups;
    std::vector<const UiItem *> Widgets;
    std::vector<bool> IsGroup; // parallel to `order`
};

Folder *Descend(Folder &f, const PathSeg &seg) {
    for (size_t i = 0; i < f.Order.size(); ++i)
        if (f.IsGroup[i] && f.Order[i].first == seg.Name) return &f.Groups[f.Order[i].second];
    f.Groups.push_back(Folder{seg.Group, seg.Name, {}, {}, {}, {}});
    f.Order.emplace_back(seg.Name, f.Groups.size() - 1);
    f.IsGroup.push_back(true);
    return &f.Groups.back();
}

UiNode Finish(const Folder &f, bool root, std::string_view root_name) {
    UiNode n;
    n.IsGroup = true;
    n.Orient = f.Orient;
    n.Raw = f.Raw;
    ExtractMetadata(f.Raw, n.Label, n.Meta);
    if (root && n.Label.empty()) n.Label = std::string(root_name);

    struct Entry {
        std::string Key;
        UiNode Node;
    };
    std::vector<Entry> entries;
    for (size_t i = 0; i < f.Order.size(); ++i) {
        const size_t at = f.Order[i].second;
        if (f.IsGroup[i]) {
            entries.push_back({f.Order[i].first, Finish(f.Groups[at], false, root_name)});
        } else {
            const UiItem &w = *f.Widgets[at];
            UiNode c;
            c.Kind = w.Kind;
            c.Raw = w.Path.back().Name;
            ExtractMetadata(c.Raw, c.Label, c.Meta);
            c.Init = w.Init;
            c.Min = w.Min;
            c.Max = w.Max;
            c.Step = w.Step;
            c.WidgetLabel = w.Label;
            entries.push_back({f.Order[i].first, std::move(c)});
        }
    }
    // Byte order on the raw label, orientation dropped, so groups and widgets sort as one.
    std::ranges::stable_sort(entries, [](const Entry &a, const Entry &b) { return a.Key < b.Key; });
    for (Entry &e : entries) n.Children.push_back(std::move(e.Node));
    return n;
}

} // namespace

// A second pass, so the bargraph counter follows the emission order `Finish` settles.
static void NameTheUnnamed(UiNode &n, int &h, int &v) {
    if (n.Label.empty()) {
        if (n.IsGroup) n.Label = "0x00";
        else if (n.Kind == UiKind::HBargraph) n.Label = std::format("hbargraph{}", h++);
        else if (n.Kind == UiKind::VBargraph) n.Label = std::format("vbargraph{}", v++);
        else n.Label = "0x00";
    }
    for (UiNode &c : n.Children) NameTheUnnamed(c, h, v);
}

std::map<uint32_t, int> KeepCounts(const Signals &sigs, std::span<const SigId> roots) {
    std::map<uint32_t, int> keep;
    Reachable(sigs, roots, [&](SigId id) {
        if (IsLabelled(sigs.KindOf(id))) ++keep[sigs.Get(id).Payload];
        return true;
    });
    return keep;
}

UiNode BuildUiTree(std::span<const UiItem> items, std::string_view root_name, const std::map<uint32_t, int> &keep) {
    Folder root;
    for (const UiItem &w : items) {
        const auto it = keep.find(w.Label);
        if (it == keep.end()) continue;
        Folder *f = &root;
        for (size_t i = 0; i + 1 < w.Path.size(); ++i) f = Descend(*f, w.Path[i]);
        // One entry per widget node, counted from the graph: one box met twice is one node.
        int already = 0;
        for (size_t i = 0; i < f->Order.size(); ++i)
            if (!f->IsGroup[i] && f->Widgets[f->Order[i].second]->Label == w.Label) ++already;
        for (int n = already; n < it->second; ++n) {
            f->Widgets.push_back(&w);
            f->Order.emplace_back(w.Path.back().Name, f->Widgets.size() - 1);
            f->IsGroup.push_back(false);
        }
    }

    // The fake root drops out where one folder encloses everything, else it takes the name.
    UiNode out = (root.Order.size() == 1 && root.IsGroup[0]) ? Finish(root.Groups[root.Order[0].second], true, root_name) : Finish(root, true, root_name);
    int h = 0, v = 0;
    NameTheUnnamed(out, h, v);
    return out;
}

std::string RootLabel(const MetaSet &meta) {
    std::string root;
    for (const auto &[k, v] : meta.Entries)
        if (k == "name") root = StripQuotes(v);
    return root;
}

namespace {

// The path a host addresses a widget by: root, each group, then its own, all cleaned.
void Walk(const UiNode &n, const std::string &prefix, std::map<std::string, std::pair<int, int>> &seen) {
    const std::string path = prefix + "/" + n.Label;
    if (n.IsGroup) {
        for (const UiNode &c : n.Children) Walk(c, path, seen);
        return;
    }
    // A `soundfile` holds no value a host writes or reads.
    if (n.Kind == UiKind::Soundfile) return;
    const bool bargraph = n.Kind == UiKind::VBargraph || n.Kind == UiKind::HBargraph;
    auto &[inputs, bargraphs] = seen[path];
    (bargraph ? bargraphs : inputs) += 1;
}

} // namespace

std::vector<Diagnostic> CheckPaths(const UiNode &tree) {
    std::map<std::string, std::pair<int, int>> seen; // path -> {inputs, bargraphs}
    for (const UiNode &c : tree.Children) Walk(c, "/" + tree.Label, seen);

    std::vector<Diagnostic> out;
    for (const auto &[path, counts] : seen) {
        const auto [inputs, bargraphs] = counts;
        Diagnostic d;
        // No subject: naming either term marks every occurrence, where the path marks one.
        if (inputs > 1) {
            d.Severity = Severity::Error;
            d.Code = Code::PropDuplicatePath;
            d.Payload = std::format("control path '{}' is used by {} input controls", path, inputs);
        } else if (inputs == 1 && bargraphs > 0) {
            d.Severity = Severity::Error;
            d.Code = Code::PropDuplicatePath;
            d.Payload = "control path '" + path + "' is used by an input control and a bargraph";
        } else if (bargraphs > 1) {
            d.Severity = Severity::Warning;
            d.Code = Code::PropDuplicateBargraphPath;
            d.Payload = std::format("bargraph path '{}' is used by {} bargraphs", path, bargraphs);
        } else {
            continue;
        }
        out.push_back(std::move(d));
    }
    return out;
}

} // namespace faustlens
