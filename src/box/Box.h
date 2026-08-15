// The evaluated diagram. Hash-consed, so equality is an integer comparison, and
// arity-checked at construction.
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
    // Normal form: what propagation dispatches on. A literal carries its value
    // and not its lexeme, so `100.0` and `1e+02` are one box.
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
    // children: ins, outs, entries, not a table since a `route` may hold pattern
    // variables. aux is nonzero only where all three folded.
    Route,
    Environment, // what an environment closure becomes once symbolic
    Slot,
    Symbolic, // children: slot, body. An abstraction applied to a slot
    Error, // poison: unconstrained arity, absorbs its neighbours

    // Values that are not circuits. form: TermClosure or EnvClosure. payload: the
    // abstraction's term, unset for an environment. aux: EnvId
    Closure,
    PatternMatcher, // payload: the `Case` term, aux: PMState index, children: args consumed
    PatternVar, // payload: the bound name

    Count_
};

std::string_view BoxKindName(BoxKind);

// The five composition operators are contiguous above.
constexpr bool IsComposition(BoxKind k) { return k >= BoxKind::Seq && k <= BoxKind::Rec; }

// How many inputs the primitive takes. Every primitive has one output.
uint8_t PrimArity(Prim);

// `known` is false where the arity is undetermined, which composition propagates
// outward.
struct Arity {
    int32_t Ins = 0, Outs = 0;
    bool Known = false;
};

using BoxNode = ArenaNode;

// A widget's four bounds, a bargraph's two.
struct Bounds {
    double Init = 0, Min = 0, Max = 0, Step = 0;
};

// A `route(ins, outs, pairs)` whose three parts folded to constants.
struct RouteTable {
    int32_t Ins = 0, Outs = 0;
    std::vector<int32_t> Pairs;
};

// An `ffunction`'s declared signature.
struct Signature {
    FType Result = FType::Float;
    std::vector<uint8_t> Args; // FType, or 2 for `any`
    StrId Include = 0, Library = 0;
};

// The live rules of a partially applied `case`, one environment each.
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
    // Ask before `Make`, which has no term to attribute a diagnostic to.
    bool Composable(BoxKind, BoxId a, BoxId b) const;

    // Side tables, addressed by a node's `Aux`.
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

    // Slot numbers are per-`Boxes` and monotonic, so two evaluations of one
    // program agree only up to a bijection. Compare graphs, not bytes.
    BoxId NewSlot(StrId name);

    Arity Infer(BoxKind, uint32_t payload, uint32_t aux, std::span<const BoxId> children) const;
};

} // namespace faustlens
