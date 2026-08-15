#include "syntax/Token.h"

#include <algorithm>
#include <array>

namespace faustlens {
namespace {

struct Row {
    std::string_view Name, Text;
};

constexpr std::array<Row, TokenKindCount> Rows = [] {
    std::array<Row, TokenKindCount> r{};
    auto set = [&r](Tok k, std::string_view name, std::string_view text = {}) { r[size_t(k)] = {name, text}; };
    set(Tok::Whitespace, "Whitespace");
    set(Tok::LineComment, "LineComment");
    set(Tok::BlockComment, "BlockComment");

    set(Tok::BDoc, "BDoc", "<mdoc>");
    set(Tok::EDoc, "EDoc", "</mdoc>");
    set(Tok::DocChar, "DocChar");
    set(Tok::BEqn, "BEqn", "<equation>");
    set(Tok::EEqn, "EEqn", "</equation>");
    set(Tok::BDgm, "BDgm", "<diagram>");
    set(Tok::EDgm, "EDgm", "</diagram>");
    set(Tok::BMetadata, "BMetadata", "<metadata>");
    set(Tok::EMetadata, "EMetadata", "</metadata>");
    set(Tok::Notice, "Notice", "<notice/>");
    set(Tok::BLst, "BLst", "<listing");
    set(Tok::ELst, "ELst", "/>");

    set(Tok::LstTrue, "LstTrue", "true");
    set(Tok::LstFalse, "LstFalse", "false");
    set(Tok::LstDependencies, "LstDependencies", "dependencies");
    set(Tok::LstMdoctags, "LstMdoctags", "mdoctags");
    set(Tok::LstDistributed, "LstDistributed", "distributed");
    set(Tok::LstEq, "LstEq", "=");
    set(Tok::LstQ, "LstQ", "\"");

    set(Tok::Int, "Int");
    set(Tok::Float, "Float");

    set(Tok::Seq, "Seq", ":");
    set(Tok::Par, "Par", ",");
    set(Tok::Split, "Split", "<:");
    set(Tok::Mix, "Mix", ":>");
    set(Tok::Rec, "Rec", "~");

    set(Tok::Add, "Add", "+");
    set(Tok::Sub, "Sub", "-");
    set(Tok::Mul, "Mul", "*");
    set(Tok::Div, "Div", "/");
    set(Tok::Mod, "Mod", "%");
    set(Tok::FDelay, "FDelay", "@");
    set(Tok::Delay1, "Delay1", "'");

    set(Tok::And, "And", "&");
    set(Tok::Or, "Or", "|");
    set(Tok::Xor, "Xor", "xor");

    set(Tok::Lsh, "Lsh", "<<");
    set(Tok::Rsh, "Rsh", ">>");

    set(Tok::Lt, "Lt", "<");
    set(Tok::Le, "Le", "<=");
    set(Tok::Gt, "Gt", ">");
    set(Tok::Ge, "Ge", ">=");
    set(Tok::Eq, "Eq", "==");
    set(Tok::Ne, "Ne", "!=");

    set(Tok::Wire, "Wire", "_");
    set(Tok::Cut, "Cut", "!");

    set(Tok::EndDef, "EndDef", ";");
    set(Tok::Def, "Def", "=");
    set(Tok::LPar, "LPar", "(");
    set(Tok::RPar, "RPar", ")");
    set(Tok::LBraq, "LBraq", "{");
    set(Tok::RBraq, "RBraq", "}");
    set(Tok::LCroc, "LCroc", "[");
    set(Tok::RCroc, "RCroc", "]");

    set(Tok::Lambda, "Lambda", "\\");
    set(Tok::Dot, "Dot", ".");
    set(Tok::With, "With", "with");
    set(Tok::LetRec, "LetRec", "letrec");
    set(Tok::Where, "Where", "where");

    set(Tok::Mem, "Mem", "mem");
    set(Tok::Prefix, "Prefix", "prefix");

    set(Tok::IntCast, "IntCast", "int");
    set(Tok::FloatCast, "FloatCast", "float");
    set(Tok::NoTypeCast, "NoTypeCast", "any");

    set(Tok::RdTbl, "RdTbl", "rdtable");
    set(Tok::RwTbl, "RwTbl", "rwtable");

    set(Tok::Select2, "Select2", "select2");
    set(Tok::Select3, "Select3", "select3");

    set(Tok::FFunction, "FFunction", "ffunction");
    set(Tok::FConstant, "FConstant", "fconstant");
    set(Tok::FVariable, "FVariable", "fvariable");

    set(Tok::Button, "Button", "button");
    set(Tok::Checkbox, "Checkbox", "checkbox");
    set(Tok::VSlider, "VSlider", "vslider");
    set(Tok::HSlider, "HSlider", "hslider");
    set(Tok::NEntry, "NEntry", "nentry");
    set(Tok::VGroup, "VGroup", "vgroup");
    set(Tok::HGroup, "HGroup", "hgroup");
    set(Tok::TGroup, "TGroup", "tgroup");
    set(Tok::VBargraph, "VBargraph", "vbargraph");
    set(Tok::HBargraph, "HBargraph", "hbargraph");
    set(Tok::Soundfile, "Soundfile", "soundfile");

    set(Tok::Attach, "Attach", "attach");
    set(Tok::Modulate, "Modulate", "minput");

    set(Tok::Acos, "Acos", "acos");
    set(Tok::Asin, "Asin", "asin");
    set(Tok::Atan, "Atan", "atan");
    set(Tok::Atan2, "Atan2", "atan2");

    set(Tok::Cos, "Cos", "cos");
    set(Tok::Sin, "Sin", "sin");
    set(Tok::Tan, "Tan", "tan");

    set(Tok::Exp, "Exp", "exp");
    set(Tok::Log, "Log", "log");
    set(Tok::Log10, "Log10", "log10");
    set(Tok::PowOp, "PowOp", "^");
    set(Tok::PowFun, "PowFun", "pow");
    set(Tok::Sqrt, "Sqrt", "sqrt");

    set(Tok::Abs, "Abs", "abs");
    set(Tok::Min, "Min", "min");
    set(Tok::Max, "Max", "max");

    set(Tok::Fmod, "Fmod", "fmod");
    set(Tok::Remainder, "Remainder", "remainder");

    set(Tok::Floor, "Floor", "floor");
    set(Tok::Ceil, "Ceil", "ceil");
    set(Tok::Rint, "Rint", "rint");
    set(Tok::Round, "Round", "round");

    set(Tok::ISeq, "ISeq", "seq");
    set(Tok::IPar, "IPar", "par");
    set(Tok::ISum, "ISum", "sum");
    set(Tok::IProd, "IProd", "prod");

    set(Tok::Inputs, "Inputs", "inputs");
    set(Tok::Outputs, "Outputs", "outputs");

    set(Tok::Import, "Import", "import");
    set(Tok::Component, "Component", "component");
    set(Tok::Library, "Library", "library");
    set(Tok::Environment, "Environment", "environment");

    set(Tok::Waveform, "Waveform", "waveform");
    set(Tok::Route, "Route", "route");
    set(Tok::Enable, "Enable", "enable");
    set(Tok::Control, "Control", "control");

    set(Tok::Declare, "Declare", "declare");

    set(Tok::Case, "Case", "case");
    set(Tok::Arrow, "Arrow", "=>");
    set(Tok::LApply, "LApply", "->");

    set(Tok::AssertBounds, "AssertBounds", "assertbounds");
    set(Tok::Lowest, "Lowest", "lowest");
    set(Tok::Highest, "Highest", "highest");

    set(Tok::FloatMode, "FloatMode", "singleprecision");
    set(Tok::DoubleMode, "DoubleMode", "doubleprecision");
    set(Tok::QuadMode, "QuadMode", "quadprecision");
    set(Tok::FixedPointMode, "FixedPointMode", "fixedpointprecision");

    set(Tok::Ident, "Ident");
    set(Tok::String, "String");
    set(Tok::FString, "FString");

    set(Tok::Unknown, "Unknown");
    set(Tok::Eof, "Eof");
    return r;
}();

// A maximal NSID looked up whole, so only lowercase letters and digits can spell one.
constexpr bool IsWord(std::string_view s) {
    if (s.empty() || s.front() < 'a' || s.front() > 'z') return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

// `dependencies`, `true` and their neighbours spell attributes inside `<listing`, and
// are words nowhere else.
constexpr bool IsListingOnly(Tok t) { return t >= Tok::LstTrue && t <= Tok::LstQ; }

constexpr bool IsKeyword(size_t i) { return IsWord(Rows[i].Text) && !IsListingOnly(Tok(i)); }

constexpr size_t KeywordCount = [] {
    size_t n = 0;
    for (size_t i = 0; i < Rows.size(); ++i) n += IsKeyword(i) ? 1 : 0;
    return n;
}();

constexpr std::array<Spelling, KeywordCount> KeywordTable = [] {
    std::array<Spelling, KeywordCount> out{};
    size_t n = 0;
    for (size_t i = 0; i < Rows.size(); ++i)
        if (IsKeyword(i)) out[n++] = {Rows[i].Text, Tok(i)};
    std::ranges::sort(out, [](const Spelling &a, const Spelling &b) { return a.Text < b.Text; });
    return out;
}();
static_assert(KeywordTable.size() == 73);

} // namespace

std::string_view TokenText(Tok k) { return Rows[size_t(k)].Text; }
std::string_view TokenName(Tok k) { return Rows[size_t(k)].Name; }

std::span<const Spelling> Keywords() { return KeywordTable; }

} // namespace faustlens
