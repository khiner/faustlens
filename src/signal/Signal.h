#pragma once

#include "Arena.h"
#include "syntax/Term.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace faustlens {

using SigId = uint32_t;
inline constexpr SigId NoSig = 0xFFFFFFFFu;

enum class SigKind : uint8_t {
    Int,
    Real,
    Waveform, // form: 1 if not every element is integral. aux: waveform table index
    FConst, // payload: name. form: FType
    FVar, // payload: name. form: FType
    Input, // payload: channel index

    BinOp, // form: `BinOpCode`

    Delay1,
    Delay, // x, n
    Prefix, // Optional state tag: aux=1, payload=original state SigId.

    IntCast,
    FloatCast,
    BitCast,
    Select2, // selector, then the two branches
    Select3,

    WRTbl, // size, generator [, write index, write signal]; optional state tag as for Prefix
    RDTbl, // table, index
    Gen,

    // Payload stores the full interned label path.
    Button,
    Checkbox,
    VSlider,
    HSlider,
    NumEntry,
    VBargraph,
    HBargraph,

    Soundfile, // payload: label path. aux: channel count
    SoundfileLength, // soundfile, part
    SoundfileRate, // soundfile, part
    SoundfileBuffer, // soundfile, channel, part, index

    FFun, // payload: name. form: FType. aux: an index into the *box* arena
    Attach,
    Control, // x, condition
    Enable, // x, condition

    // Rec stores branch ids; Proj reads branch Payload.
    Rec,
    Proj,

    Extended, // form: `Ext`

    Error, // error propagated through the subgraph

    Count_
};

std::string_view SigKindName(SigKind);

// Preserve reference gBinOpTable order for .sig comparison.
enum class BinOpCode : uint8_t { Add, Sub, Mul, Div, Rem, LeftShift, RightShift, LRightShift, GT, LT, GE, LE, EQ, NE, AND, OR, XOR, Count_ };

std::string_view BinOpName(BinOpCode);

constexpr bool IsComparison(BinOpCode b) { return b >= BinOpCode::GT && b <= BinOpCode::NE; }

constexpr bool IsWidget(SigKind k) { return k >= SigKind::Button && k <= SigKind::HBargraph; }
constexpr bool IsLabelled(SigKind k) { return IsWidget(k) || k == SigKind::Soundfile; }

enum class Ext : uint8_t {
    Abs,
    Acos,
    Acosh,
    Asin,
    Asinh,
    Atan,
    Atan2,
    Atanh,
    Ceil,
    Cos,
    Cosh,
    Exp,
    Floor,
    Fmod,
    Log,
    Log10,
    Max,
    Min,
    Pow,
    Remainder,
    Rint,
    Round,
    Sin,
    Sinh,
    Sqrt,
    Tan,
    Tanh,
    AssertBounds,
    Lowest,
    Highest,
    Count_
};

std::string_view ExtName(Ext);

// Prefix/WRTbl state tags keep histories distinct when transformed values coincide.
// They affect interning and content/shape hashes without adding runtime dependencies.
// Origin nodes must remain in the same arena; hashes follow their content.
using SigNode = ArenaNode;

struct Signals : Arena<Signals, SigKind, SigId> {
    std::vector<std::vector<double>> Waveforms;
    mutable std::vector<int8_t> Orders;
    mutable std::vector<uint64_t> Content, Shape;
    mutable std::vector<uint8_t> HasContent, HasShape;
    StringPool Strings;

    Signals();

    using Arena::Make;
    SigId Make(SigKind, uint8_t form, uint32_t payload, uint32_t aux, std::span<const SigId> children);
    // Construct raw nodes; use SimpBinOp for folding.
    SigId MakeBin(BinOpCode op, SigId x, SigId y) { return Make(SigKind::BinOp, uint8_t(op), 0, 0, {x, y}); }
    bool IsInt(SigId s) const { return KindOf(s) == SigKind::Int; }

    // Compare content hashes across arenas; ShapeHash excludes numeric payloads.
    uint64_t ContentHash(SigId) const;
    uint64_t ShapeHash(SigId) const;

    // Reserve a Rec id before constructing projections; CloseRec may return an existing group.
    SigId OpenRec();
    SigId CloseRec(SigId reserved, std::span<const SigId> branches);

    uint32_t AddWaveform(std::vector<double>);
    const std::vector<double> &WaveformAt(uint32_t i) const { return Waveforms[i]; }

    // 0 literal, 1 `fconstant`, 2 control-rate reader, 3 anything reaching a sample.
    int Order(SigId) const;

    // Label paths are interned during propagation.
    uint32_t InternStr(std::string_view s) { return Strings.Intern(s); }
    std::string_view Str(uint32_t id) const { return Strings.At(id); }

    // Hash self-references as canonical back edges.
    uint64_t HashOf(SigKind, uint8_t form, uint32_t payload, uint32_t aux, std::span<const SigId> children, SigId self) const;
};

// Visit reachable nodes once; returning false prunes traversal at that node.
template<class Visit> void Reachable(const Signals &s, std::span<const SigId> roots, Visit visit) {
    std::vector<uint8_t> seen(s.Size(), 0);
    std::vector<SigId> stack(roots.begin(), roots.end());
    while (!stack.empty()) {
        const SigId id = stack.back();
        stack.pop_back();
        if (seen[id]) continue;
        seen[id] = 1;
        if (!visit(id)) continue;
        for (const SigId c : s.Children(id)) stack.push_back(c);
    }
}

} // namespace faustlens
