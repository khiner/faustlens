// The lowered program: three-address code over virtual registers, with all state
// reached through one `LoadField`/`StoreField` pair.
#pragma once

#include "signal/Promote.h"
#include "signal/Propagate.h"
#include "signal/Schedule.h"
#include "signal/Signal.h"
#include "signal/Ui.h"

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace faustlens {

struct Session;

using Reg = uint32_t;
inline constexpr Reg NoReg = 0xFFFFFFFFu;
inline constexpr uint32_t NoField = 0xFFFFFFFFu;
// A field owned by the instance rather than by a fill loop.
inline constexpr uint32_t NoLoop = 0xFFFFFFFFu;
inline constexpr uint32_t NoDesc = 0xFFFFFFFFu;

// `Form` is the sub-code where a kind covers a table. `Dst` is the result register,
// `NoReg` where there is no value.
enum class Op : uint8_t {
    ConstInt,
    ConstReal, // imm/aux: the double's low and high words
    Input, // imm: channel
    Output, // imm: channel. args: value

    BinOp,
    Extended,
    IntCast,
    FloatCast,
    BitCast,
    Select2, // args: selector, then the two branches
    Select3, // args: selector, then the three

    LoadField, // imm: field. args: [index]
    StoreField, // imm: field. args: [index,] value

    SoundfileLength,
    SoundfileRate, // imm: field. args: part
    SoundfileRead, // imm: field. args: channel, part, index

    FConst,
    FVar, // imm: the `foreign` entry
    FFun, // imm: the `foreign` entry. args: the call's

    // Init band only. `LoopBegin` counts `Dst` up to `Imm`, `LoopEnd` carries neither.
    LoopBegin,
    LoopEnd,
    // `GuardBegin` takes the guard register as its one operand, `GuardEnd` none.
    GuardBegin,
    GuardEnd,

    Count_
};

std::string_view OpName(Op);

struct Instr {
    uint8_t Op = 0;
    uint8_t Form = 0;
    // Not recoverable from the operands: `x > y` is an `int` over two `double`s.
    Nature Nature = Nature::Real;
    Reg Dst = NoReg;
    uint32_t Imm = 0, Aux = 0;
    uint32_t Args = 0, ArgCount = 0; // offset into the operand pool
};

enum class FieldKind : uint8_t {
    // A delayed signal's history. `ring` picks `IOTA` indexing over an end-of-frame copy.
    Delay,
    // Table storage or `waveform` samples. Recomputed rather than migrated.
    Table,
    // A UI control's zone, keyed on its label path so a slider stays moved across an edit.
    Widget,
    Soundfile, // one per `soundfile`, written by the host
    // A scalar carried across a frame that is not history: guards, `prefix`, `IOTA`.
    Perm,
};

struct Field {
    FieldKind Kind = FieldKind::Perm;
    Nature Nature = Nature::Real;
    // Migration keys. `Hash` is Merkle content, not the arena's intern hash. `Shape`
    // normalizes literals away.
    SigId Sig = NoSig;
    uint64_t Hash = 0, Shape = 0;
    // `NoTerm` is "no candidate", not position zero.
    ValueId Origin = NoTerm;
    uint32_t Extent = 1; // slots, 1 for a scalar
    bool Ring = false;
    int32_t MaxDelay = 0;
    // Widget: the interned path, good only in the interning arena. See `Plan::Labels`.
    uint32_t Label = 0;
    // Entry in `Waves` or in `Soundfiles`. A field is at most one of the two.
    uint32_t Desc = NoDesc;
    // Index of the owning `LoopBegin` in the init band.
    uint32_t Loop = NoLoop;
};

// The host resolves the URLs. Empty `urls` is a lookup by label, not an error.
struct SoundfileDesc {
    uint32_t Label = 0; // the interned label path, and this desc's identity
    uint32_t Channels = 0;
    std::vector<std::string> Urls;
};

// `fconstant` is read once at init, `fvariable` at block rate, `ffunction` is a call.
enum class ForeignKind : uint8_t { Constant, Variable, Function };

// `args` is what the call passes, in call order. Declared parameter types never reach
// here.
struct ForeignDesc {
    ForeignKind Kind = ForeignKind::Function;
    std::string Name;
    Nature Result = Nature::Real;
    std::vector<Nature> Args;
};

// Three bands over one register file that persists per instance, so an init-band value
// reaches the sample band without a field.
struct Plan {
    std::vector<Field> Fields;
    std::array<std::vector<Instr>, 3> Bands;
    std::vector<Reg> Operands;
    std::vector<std::vector<double>> Waves;
    std::vector<SoundfileDesc> Soundfiles;
    std::vector<ForeignDesc> Foreign;
    // Widget paths by `Field::label`. Ids differ across arenas, so migration compares
    // the paths.
    std::vector<std::string> Labels;
    uint32_t Regs = 0;
    // `inputs` has to be given: lowering sees only the inputs the program reads.
    int32_t Inputs = 0, Outputs = 0;

    std::string_view Label(uint32_t id) const { return id < Labels.size() ? std::string_view(Labels[id]) : std::string_view(); }

    std::span<const Reg> Args(const Instr &i) const { return {Operands.data() + i.Args, i.ArgCount}; }
    // One body for both constnesses: `self` carries the caller's.
    auto &Band(this auto &&self, faustlens::Band b) { return self.Bands[size_t(b)]; }
};

// Path to Signal graph, the steps shared by every compile. `sigs` is bound by
// reference and outlives the Graph.
struct Graph {
    Graph(Session &, const std::string &path, Signals &, bool add_normal_form = true);

    Propagator Prop;
    BoxId Box = NoBox;
    Arity Arity;
    std::vector<SigId> Outs;
    bool Ok = false;

    // Lowers everything reachable from `outs`, so the root set is load-bearing.
    // Unexpected where a construct has no lowering, saying which.
    std::expected<Plan, std::string> Lower() const;
    // The widgets still standing after simplification.
    UiNode Ui(std::string_view root_name) const;
};

// Equal hashes mean an instance swap can be skipped entirely.
uint64_t Hash(const Plan &);

} // namespace faustlens
