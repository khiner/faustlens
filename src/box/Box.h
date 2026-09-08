// Interned evaluated diagrams with arity checked at construction.
#pragma once

#include "Arena.h"
#include "syntax/Term.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace faustlens {

using BoxId = uint32_t;
using EnvId = uint32_t;

inline constexpr BoxId NoBox = 0xFFFFFFFFu;

enum class BoxKind : uint8_t {
    // Numeric literals store values, so equivalent spellings share a Box.
    Int,
    Real,
    Wire, // `_`
    Cut, // `!`
    Prim,
    FFun, // payload: interned selected name
    FConst, // form: FType, payload: name, aux: the include file
    FVar, // form: FType, payload: name, aux: the include file
    Button, // payload: label
    Checkbox, // payload: label
    NumericWidget, // form: WidgetKind, payload: label, aux: bounds index
    Bargraph, // form: BargraphKind, payload: label, aux: bounds index
    Group, // form: GroupKind, payload: label, child: body
    Soundfile, // payload: label
    Waveform, // form: 0 all elements integral, 1 not. aux: waveform table index
    Seq,
    Par,
    Split,
    Merge,
    Rec, // lhs, rhs
    // Children: ins, outs, entries, including pattern variables.
    // Aux is nonzero when all three are constant.
    Route,
    Environment, // symbolic environment closure
    Slot,
    Symbolic, // children: slot, body
    Error, // unconstrained arity; propagates through composition

    // Non-circuit values: form is TermClosure or EnvClosure, payload is the abstraction term, and aux is EnvId.
    Closure,
    PatternMatcher, // payload: the `Case` term, aux: PMState index, children: args consumed
    PatternVar, // payload: the bound name

    Count_
};

std::string_view BoxKindName(BoxKind);

constexpr bool IsComposition(BoxKind k) { return k >= BoxKind::Seq && k <= BoxKind::Rec; }

// Return the primitive's input count; each primitive has one output.
uint8_t PrimArity(Prim);

// Unknown arity propagates through composition.
struct Arity {
    int32_t Ins = 0, Outs = 0;
    bool Known = false;
};

using BoxNode = ArenaNode;

struct Bounds {
    double Init = 0, Min = 0, Max = 0, Step = 0;
};

struct RouteTable {
    int32_t Ins = 0, Outs = 0;
    std::vector<int32_t> Pairs;
};

struct Signature {
    FType Result = FType::Float;
    std::vector<uint8_t> Args; // FType, or 2 for `any`
    StrId Include = 0, Library = 0;
};

// Partially applied case rules with one environment per rule.
struct PMState {
    std::vector<std::vector<BoxId>> Patterns; // per rule, evaluated once
    std::vector<EnvId> RuleEnvs;
    std::vector<uint8_t> Live;
};

struct Boxes : Arena<Boxes, BoxKind, BoxId> {
    std::vector<Arity> Arities; // parallel to `Nodes`

    std::vector<Bounds> Bounds;
    std::vector<RouteTable> Routes;
    std::vector<Signature> Signatures;
    std::vector<std::vector<double>> Waveforms;
    std::vector<PMState> PmStates;

    uint32_t NextSlot = 0;
    BoxId Wire = NoBox;

    Boxes();

    using Arena::Make;
    BoxId Make(BoxKind, uint8_t form, uint32_t payload, uint32_t aux, std::span<const BoxId> children);
    BoxId MakePrim(Prim p) { return MakeLeaf(BoxKind::Prim, uint32_t(p)); }

    const Arity &ArityOf(BoxId b) const { return Arities[b]; }
    // Check before Make to attribute arity failures to a source term.
    bool Composable(BoxKind, BoxId a, BoxId b) const;

    uint32_t AddBounds(const faustlens::Bounds &);
    const faustlens::Bounds &BoundsAt(uint32_t i) const { return Bounds[i]; }
    uint32_t AddRoute(RouteTable);
    const RouteTable &RouteAt(uint32_t i) const { return Routes[i]; }
    uint32_t AddSignature(Signature);
    const Signature &SignatureAt(uint32_t i) const { return Signatures[i]; }
    uint32_t AddWaveform(std::vector<double>);
    const std::vector<double> &WaveformAt(uint32_t i) const { return Waveforms[i]; }
    uint32_t AddPMState(PMState);
    const PMState &PMStateAt(uint32_t i) const { return PmStates[i]; }

    // Slot numbers are local to Boxes; compare graphs with a slot bijection across evaluations.
    BoxId NewSlot(StrId name);

    Arity Infer(BoxKind, uint32_t payload, uint32_t aux, std::span<const BoxId> children) const;
};

} // namespace faustlens
