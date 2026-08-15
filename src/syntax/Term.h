// Position-free interned values, plus a per-file positional ref tree over them.
#pragma once

#include "syntax/Token.h"

#include <cstdint>
#include <deque>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace faustlens {

using ValueId = uint32_t;
using StrId = uint32_t;
using RefId = uint32_t;

inline constexpr ValueId NoTerm = 0xFFFFFFFFu;
inline constexpr RefId NoRef = 0xFFFFFFFFu;

constexpr uint64_t Mix(uint64_t h, uint64_t x) { return h ^ (x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2)); }

// A deque, not a vector: the map's keys are views into these strings.
struct StringPool {
    std::deque<std::string> Strings;
    std::unordered_map<std::string_view, uint32_t> Ids;

    uint32_t Intern(std::string_view s) {
        if (const auto it = Ids.find(s); it != Ids.end()) return it->second;
        const auto id = uint32_t(Strings.size());
        Strings.emplace_back(s);
        Ids.emplace(std::string_view(Strings.back()), id);
        return id;
    }
    std::string_view At(uint32_t id) const { return Strings[id]; }
};

// Enforced by the parser and the printer, so deep recursion is a diagnostic, not a crash.
inline constexpr uint32_t MaxTermDepth = 2048;

enum class Kind : uint8_t {
    // statements
    Program,
    Import,
    Declare,
    DeclareDef,
    Definition,
    Clause,
    RecDef,
    MdocBlock,
    MdocProse,
    MdocEquation,
    MdocDiagram,
    MdocMetadata,
    MdocListing,
    MdocNotice,

    // expressions
    Seq,
    Par,
    Split,
    Merge,
    RecComp,
    // The only kind whose payload is a `Tok`, which is how `RowOf` recovers the operator.
    BinOp,
    Delay1,
    NegIdent,
    With,
    LetRec,
    ModifLocalDef,
    Apply,
    Access,
    Lambda,
    Case,
    Rule,
    Modulation,
    Modulator,
    Iterate,
    Inputs,
    Outputs,
    Environment,
    Component,
    Library,
    Waveform,
    Route,
    Hole,

    // leaves
    Int,
    Real,
    Ident,
    Str,
    Prim,
    Button,
    Checkbox,
    NumericWidget,
    Bargraph,
    Group,
    SoundfileBox,
    // Str(return type), 1-4 Str names, Str arg types, Str(include), Str(library). `Form`
    // is the name count, splitting the two runs.
    FFun,
    FConst,
    FVar,

    Count_
};

std::string_view KindName(Kind);

static_assert(int(Kind::Count_) <= 64);
constexpr uint64_t KindMask(std::initializer_list<Kind> ks) {
    uint64_t m = 0;
    for (const Kind k : ks) m |= uint64_t{1} << int(k);
    return m;
}

// Whether `Payload` is a string id, so `Lexeme` is safe to call.
bool HasLexeme(Kind);

enum class MergeSpelling : uint8_t { Colon, Plus }; // `:>` / `+>`
enum class PowSpelling : uint8_t { Caret, Fun }; // `^` / `pow`
enum class IterKind : uint8_t { Par, Seq, Sum, Prod };
enum class WidgetKind : uint8_t { VSlider, HSlider, NEntry };
enum class BargraphKind : uint8_t { VBargraph, HBargraph };
enum class GroupKind : uint8_t { VGroup, HGroup, TGroup };
enum class FType : uint8_t { Int, Float };

// Each attribute may repeat or be absent, so it gets a value bit and a presence bit.
enum ListingBits : uint8_t {
    LstDependencies = 1 << 0,
    LstMdoctags = 1 << 1,
    LstDistributed = 1 << 2,
    LstDependenciesSet = 1 << 3,
    LstMdoctagsSet = 1 << 4,
    LstDistributedSet = 1 << 5,
};

// Precision bitmask on a statement.
enum Variant : uint16_t {
    Single = 1,
    Double = 2,
    Quad = 4,
    FixedPoint = 8,
};

enum class Prim : uint8_t {
    Wire,
    Cut,
    Mem,
    Prefix,
    IntCast,
    FloatCast,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    FDelay,
    And,
    Or,
    Xor,
    Lsh,
    Rsh,
    Lt,
    Le,
    Gt,
    Ge,
    Eq,
    Ne,
    Attach,
    Enable,
    Control,
    Acos,
    Asin,
    Atan,
    Atan2,
    Cos,
    Sin,
    Tan,
    Exp,
    Log,
    Log10,
    Pow,
    Sqrt,
    Abs,
    Min,
    Max,
    Fmod,
    Remainder,
    Floor,
    Ceil,
    Rint,
    Round,
    RdTable,
    RwTable,
    Select2,
    Select3,
    AssertBounds,
    Lowest,
    Highest,
    Count_
};

std::string_view PrimText(Prim);
// `Prim::Count_` where the token denotes no primitive.
Prim PrimForToken(Tok);

// One surrounding pair of quotes off a `String` lexeme, which is stored as written.
constexpr std::string_view StripQuotes(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

struct TermValue {
    uint8_t Kind = 0; // Kind
    uint8_t Form = 0; // per-kind discriminant
    uint16_t Variants = 0; // precision bitmask, 0 for non-statements
    uint32_t Payload = 0; // per-kind: an interned lexeme, a `Prim` or `Tok` code, or 0
    uint32_t Children = 0; // offset into the child pool
    uint32_t ChildCount = 0;
};
static_assert(sizeof(TermValue) == 16);

struct Terms {
    StringPool Strings;
    std::vector<TermValue> Values;
    std::vector<uint64_t> Hashes;
    std::vector<ValueId> ChildPool;
    std::unordered_map<uint64_t, std::vector<ValueId>> Buckets;

    Terms();

    StrId InternStr(std::string_view s) { return Strings.Intern(s); }
    std::string_view Str(StrId id) const { return Strings.At(id); }

    ValueId Make(Kind, uint8_t form, uint16_t variants, uint32_t payload, std::span<const ValueId> children);
    ValueId Make(Kind k, std::span<const ValueId> children) { return Make(k, 0, 0, 0, children); }
    ValueId Make(Kind k, uint8_t form, uint16_t variants, uint32_t payload, std::initializer_list<ValueId> children) {
        return Make(k, form, variants, payload, std::span<const ValueId>(children.begin(), children.size()));
    }
    ValueId Make(Kind k, std::initializer_list<ValueId> children) { return Make(k, 0, 0, 0, std::span<const ValueId>(children.begin(), children.size())); }
    ValueId MakeLeaf(Kind k, uint32_t payload) { return Make(k, 0, 0, payload, {}); }
    ValueId MakePrim(Prim p) { return MakeLeaf(Kind::Prim, uint32_t(p)); }

    const TermValue &Get(ValueId id) const { return Values[id]; }
    Kind KindOf(ValueId id) const { return Kind(Values[id].Kind); }
    std::span<const ValueId> Children(ValueId id) const {
        const TermValue &v = Values[id];
        return {ChildPool.data() + v.Children, v.ChildCount};
    }
    ValueId Child(ValueId id, uint32_t i) const { return Children(id)[i]; }
    // Valid only where `HasLexeme(KindOf(id))`.
    std::string_view Lexeme(ValueId id) const { return Str(Values[id].Payload); }
    size_t Size() const { return Values.size(); }
};

struct TermRef {
    ValueId ValueId = NoTerm;
    uint32_t SpanBegin = 0, SpanEnd = 0;
    // Covers grouping parens, equal to the span where there are none.
    uint32_t OuterBegin = 0, OuterEnd = 0;
    uint32_t FirstChild = 0; // offset into the ref child pool
    uint32_t ChildCount = 0;
};

// Per-file positional tree over values. Pre-order, children contiguous and in source order.
struct RefTree {
    std::vector<TermRef> Refs;
    std::vector<RefId> ChildPool;

    RefId Root() const { return Refs.empty() ? NoRef : 0; }
    std::span<const RefId> Children(RefId r) const {
        const TermRef &t = Refs[r];
        return {ChildPool.data() + t.FirstChild, t.ChildCount};
    }
    RefId Innermost(uint32_t offset) const;
    // Every ref containing `offset`, innermost first.
    std::vector<RefId> Chain(uint32_t offset) const;
};

} // namespace faustlens
