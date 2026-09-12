#pragma once

#include "box/Box.h"
#include "box/Label.h"
#include "signal/Signal.h"
#include "syntax/Diagnostic.h"
#include "syntax/Term.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace faustlens {

enum class UiKind : uint8_t { Button, Checkbox, VSlider, HSlider, NumEntry, VBargraph, HBargraph, Soundfile };

// Preserve group orientation separately from the interned label path; path segments are outermost first.
struct UiItem {
    std::vector<PathSeg> Path;
    UiKind Kind = UiKind::Button;
    double Init = 0, Min = 0, Max = 0, Step = 0;
    // Widget payload identity survives signal normalization.
    uint32_t Label = 0;
};

struct Propagator {
    const Boxes &Boxes;
    const Terms &Terms;
    Signals &Sigs;
    std::vector<Diagnostic> Diags;
    MathBindings Math{MathBindings::LateBound};
    uint64_t RequiredMath{0};

    // Scope each symbolic slot binding to its body.
    std::vector<std::pair<BoxId, SigId>> Slots;
    std::vector<PathSeg> Groups;
    std::vector<UiItem> Ui;

    Propagator(const faustlens::Boxes &b, const faustlens::Terms &t, Signals &s, MathBindings math = MathBindings::LateBound) : Boxes(b), Terms(t), Sigs(s), Math(math) {}

    std::vector<SigId> Run(BoxId box, int32_t inputs);

    struct MemoKey {
        BoxId Box = 0;
        // Intern slot environments by bindings.
        uint32_t Slots = 0;
        uint32_t Path = 0;
        std::vector<SigId> In;
        bool operator==(const MemoKey &) const = default;
    };
    struct MemoHash {
        size_t operator()(const MemoKey &) const;
    };
    std::unordered_map<MemoKey, std::vector<SigId>, MemoHash> Memo;

    std::vector<std::vector<PathSeg>> PathTable;
    std::vector<std::vector<std::pair<BoxId, SigId>>> SlotTable;
    uint32_t Widget(StrId label, UiKind, const Bounds *);
    uint32_t PathId();
    uint32_t SlotEnvId();

    // Fold literal-only arguments except Select2; apply other rules in Simplify.
    SigId Bin(BinOpCode, SigId, SigId);
    SigId Delay(SigId, SigId);
    SigId Delay1(SigId);
    SigId Select2(SigId sel, SigId, SigId);

    std::vector<SigId> Propagate(BoxId, std::vector<SigId> in);
    std::vector<SigId> Real(BoxId, std::vector<SigId> &in);
    std::vector<SigId> Fail(BoxId, std::string_view why);
};

} // namespace faustlens
