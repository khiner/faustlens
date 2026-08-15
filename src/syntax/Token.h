#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace faustlens {

enum class Tok : uint8_t {
    Whitespace,
    LineComment,
    BlockComment,

    BDoc,
    EDoc,
    DocChar, // one token per maximal prose run, not per character
    BEqn,
    EEqn,
    BDgm,
    EDgm,
    BMetadata,
    EMetadata,
    Notice,
    BLst,
    ELst,

    LstTrue,
    LstFalse,
    LstDependencies,
    LstMdoctags,
    LstDistributed,
    LstEq,
    LstQ,

    Int,
    Float,

    Seq,
    Par,
    Split,
    Mix, // both `:>` and `+>`
    Rec,

    Add,
    Sub,
    Mul,
    Div,
    Mod,
    FDelay,
    Delay1,

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

    Wire,
    Cut,

    EndDef,
    Def,
    LPar,
    RPar,
    LBraq,
    RBraq,
    LCroc,
    RCroc,

    Lambda,
    Dot,
    With,
    LetRec,
    Where,

    Mem,
    Prefix,

    IntCast,
    FloatCast,
    NoTypeCast,

    RdTbl,
    RwTbl,

    Select2,
    Select3,

    FFunction,
    FConstant,
    FVariable,

    Button,
    Checkbox,
    VSlider,
    HSlider,
    NEntry,
    VGroup,
    HGroup,
    TGroup,
    VBargraph,
    HBargraph,
    Soundfile,

    Attach,
    Modulate,

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
    PowOp,
    PowFun,
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

    ISeq,
    IPar,
    ISum,
    IProd,

    Inputs,
    Outputs,

    Import,
    Component,
    Library,
    Environment,

    Waveform,
    Route,
    Enable,
    Control,

    Declare,

    Case,
    Arrow,
    LApply,

    AssertBounds,
    Lowest,
    Highest,

    FloatMode,
    DoubleMode,
    QuadMode,
    FixedPointMode,

    Ident,
    String,
    FString,

    Unknown,
    Eof,

    Count_
};

inline constexpr int TokenKindCount = int(Tok::Count_);

// The fixed spelling, or "" for tokens carrying a lexeme.
std::string_view TokenText(Tok);
std::string_view TokenName(Tok);

struct Spelling {
    std::string_view Text;
    Tok Kind;
};

// Every token whose spelling is a word, minus the listing-mode attributes, sorted by
// text. Derived from `TokenText`.
std::span<const Spelling> Keywords();

constexpr bool IsTrivia(Tok k) { return k == Tok::Whitespace || k == Tok::LineComment || k == Tok::BlockComment; }

constexpr bool IsComment(Tok k) { return k == Tok::LineComment || k == Tok::BlockComment; }

struct Token {
    Tok Kind;
    uint32_t Begin, End; // byte offsets into the file, [begin, end)
};

// Tiles the file: every byte in exactly one token, trivia included. Only the
// terminating zero-width `Eof` sits outside the tiling.
using TokenVector = std::vector<Token>;

} // namespace faustlens
