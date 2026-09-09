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
inline constexpr uint32_t NoLoop = 0xFFFFFFFFu;
inline constexpr uint32_t NoDesc = 0xFFFFFFFFu;

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

    // LoopBegin counts Dst from zero to Imm - 1.
    LoopBegin,
    LoopEnd, // no operands
    // GuardBegin takes one condition register.
    GuardBegin,
    GuardEnd, // no operands

    Count_
};

std::string_view OpName(Op);

struct Instr {
    uint8_t Op = 0;
    uint8_t Form = 0; // per-op variant
    // Result and operand natures can differ for comparisons.
    Nature Nature = Nature::Real;
    Reg Dst = NoReg; // NoReg for instructions without a result
    uint32_t Imm = 0, Aux = 0;
    uint32_t Args = 0, ArgCount = 0;
};

enum class FieldKind : uint8_t {
    // Use IOTA indexing for rings and end-of-frame copies for short history.
    Delay,
    Table,
    Widget,
    Soundfile, // one per `soundfile`, written by the host
    // Scalar state for guards, prefix, and IOTA.
    Perm,
};

struct Field {
    FieldKind Kind = FieldKind::Perm;
    Nature Nature = Nature::Real;
    SigId Sig = NoSig;
    // Hash includes numeric literal values, and Shape omits them.
    uint64_t Hash = 0, Shape = 0;
    ValueId Origin = NoTerm;
    uint32_t Extent = 1; // slots, 1 for a scalar
    bool Ring = false;
    int32_t MaxDelay = 0;
    // Interned widget path resolved through Plan::Labels.
    uint32_t Label = 0;
    // Index into Waves or Soundfiles according to field kind.
    uint32_t Desc = NoDesc;
    uint32_t Loop = NoLoop;
};

// The host resolves an empty URL list by label.
struct SoundfileDesc {
    uint32_t Label = 0;
    uint32_t Channels = 0;
    std::vector<std::string> Urls;
};

// Read constants at initialization and variables once per block.
enum class ForeignKind : uint8_t { Constant, Variable, Function };

// Resolve `any` argument types from the signal type.
struct ForeignDesc {
    ForeignKind Kind = ForeignKind::Function;
    std::string Name;
    Nature Result = Nature::Real;
    std::vector<Nature> Args;
};

struct Plan {
    std::vector<Field> Fields;
    std::array<std::vector<Instr>, 3> Bands;
    std::vector<Reg> Operands;
    std::vector<std::vector<double>> Waves;
    std::vector<SoundfileDesc> Soundfiles;
    std::vector<ForeignDesc> Foreign;
    std::vector<std::string> Labels;
    uint32_t Regs = 0;
    // Inputs includes unused declared channels.
    int32_t Inputs = 0, Outputs = 0;

    std::string_view Label(uint32_t id) const { return id < Labels.size() ? std::string_view(Labels[id]) : std::string_view(); }

    std::span<const Reg> Args(const Instr &i) const { return {Operands.data() + i.Args, i.ArgCount}; }
    auto &Band(this auto &&self, faustlens::Band b) { return self.Bands[size_t(b)]; }
};

// Signals must outlive Graph.
struct Graph {
    Graph(Session &, const std::string &path, Signals &, bool add_normal_form = true);

    Propagator Prop;
    BoxId Box = NoBox;
    Arity Arity;
    std::vector<SigId> Outs;
    bool Ok = false;

    // Lower nodes reachable from Outs.
    std::expected<Plan, std::string> Lower() const;
    // Return widgets present after simplification.
    UiNode Ui(std::string_view root_name) const;
};

uint64_t Hash(const Plan &);

} // namespace faustlens
