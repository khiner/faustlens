#include "syntax/Term.h"

#include <algorithm>
#include <array>

namespace faustlens {
namespace {

// Fixed, for reproducible builds.
constexpr uint64_t Seed = 0x9E3779B97F4A7C15ull;

constexpr std::array<std::string_view, size_t(Kind::Count_)> KindNames = {
    "Program",     "Import",       "Declare",     "DeclareDef", "Definition",    "Clause",      "RecDef",    "MdocBlock", "MdocProse",     "MdocEquation",
    "MdocDiagram", "MdocMetadata", "MdocListing", "MdocNotice", "Seq",           "Par",         "Split",     "Merge",     "RecComp",       "BinOp",
    "Delay1",      "NegIdent",     "With",        "LetRec",     "ModifLocalDef", "Apply",       "Access",    "Lambda",    "Case",          "Rule",
    "Modulation",  "Modulator",    "Iterate",     "Inputs",     "Outputs",       "Environment", "Component", "Library",   "Waveform",      "Route",
    "Hole",        "Int",          "Real",        "Ident",      "Str",           "Prim",        "Button",    "Checkbox",  "NumericWidget", "Bargraph",
    "Group",       "SoundfileBox", "FFun",        "FConst",     "FVar",
};

constexpr std::array<std::string_view, size_t(Prim::Count_)> PrimTexts = {
    "_",       "!",       "mem",     "prefix",       "int",    "float",   "+",         "-",     "*",    "/",    "%",     "@",
    "&",       "|",       "xor",     "<<",           ">>",     "<",       "<=",        ">",     ">=",   "==",   "!=",    "attach",
    "enable",  "control", "acos",    "asin",         "atan",   "atan2",   "cos",       "sin",   "tan",  "exp",  "log",   "log10",
    "^",       "sqrt",    "abs",     "min",          "max",    "fmod",    "remainder", "floor", "ceil", "rint", "round", "rdtable",
    "rwtable", "select2", "select3", "assertbounds", "lowest", "highest",
};

constexpr std::array<Prim, TokenKindCount> TokenPrim = [] {
    std::array<Prim, TokenKindCount> t{};
    t.fill(Prim::Count_);
    auto set = [&t](Tok tok, Prim p) { t[size_t(tok)] = p; };
    set(Tok::Wire, Prim::Wire);
    set(Tok::Cut, Prim::Cut);
    set(Tok::Mem, Prim::Mem);
    set(Tok::Prefix, Prim::Prefix);
    set(Tok::IntCast, Prim::IntCast);
    set(Tok::FloatCast, Prim::FloatCast);
    set(Tok::Add, Prim::Add);
    set(Tok::Sub, Prim::Sub);
    set(Tok::Mul, Prim::Mul);
    set(Tok::Div, Prim::Div);
    set(Tok::Mod, Prim::Mod);
    set(Tok::FDelay, Prim::FDelay);
    set(Tok::And, Prim::And);
    set(Tok::Or, Prim::Or);
    set(Tok::Xor, Prim::Xor);
    set(Tok::Lsh, Prim::Lsh);
    set(Tok::Rsh, Prim::Rsh);
    set(Tok::Lt, Prim::Lt);
    set(Tok::Le, Prim::Le);
    set(Tok::Gt, Prim::Gt);
    set(Tok::Ge, Prim::Ge);
    set(Tok::Eq, Prim::Eq);
    set(Tok::Ne, Prim::Ne);
    set(Tok::Attach, Prim::Attach);
    set(Tok::Enable, Prim::Enable);
    set(Tok::Control, Prim::Control);
    set(Tok::Acos, Prim::Acos);
    set(Tok::Asin, Prim::Asin);
    set(Tok::Atan, Prim::Atan);
    set(Tok::Atan2, Prim::Atan2);
    set(Tok::Cos, Prim::Cos);
    set(Tok::Sin, Prim::Sin);
    set(Tok::Tan, Prim::Tan);
    set(Tok::Exp, Prim::Exp);
    set(Tok::Log, Prim::Log);
    set(Tok::Log10, Prim::Log10);
    set(Tok::PowOp, Prim::Pow);
    set(Tok::PowFun, Prim::Pow);
    set(Tok::Sqrt, Prim::Sqrt);
    set(Tok::Abs, Prim::Abs);
    set(Tok::Min, Prim::Min);
    set(Tok::Max, Prim::Max);
    set(Tok::Fmod, Prim::Fmod);
    set(Tok::Remainder, Prim::Remainder);
    set(Tok::Floor, Prim::Floor);
    set(Tok::Ceil, Prim::Ceil);
    set(Tok::Rint, Prim::Rint);
    set(Tok::Round, Prim::Round);
    set(Tok::RdTbl, Prim::RdTable);
    set(Tok::RwTbl, Prim::RwTable);
    set(Tok::Select2, Prim::Select2);
    set(Tok::Select3, Prim::Select3);
    set(Tok::AssertBounds, Prim::AssertBounds);
    set(Tok::Lowest, Prim::Lowest);
    set(Tok::Highest, Prim::Highest);
    return t;
}();

constexpr uint64_t LexemeKinds = KindMask({
    Kind::Import,    Kind::Declare,  Kind::DeclareDef,    Kind::Definition, Kind::RecDef, Kind::MdocProse,    Kind::MdocMetadata, Kind::NegIdent, Kind::Access,
    Kind::Modulator, Kind::Iterate,  Kind::Component,     Kind::Library,    Kind::Hole,   Kind::Int,          Kind::Real,         Kind::Ident,    Kind::Str,
    Kind::Button,    Kind::Checkbox, Kind::NumericWidget, Kind::Bargraph,   Kind::Group,  Kind::SoundfileBox, Kind::FConst,       Kind::FVar,
});

} // namespace

std::string_view KindName(Kind k) { return KindNames[size_t(k)]; }

bool HasLexeme(Kind k) { return ((LexemeKinds >> int(k)) & 1) != 0; }
std::string_view PrimText(Prim p) { return PrimTexts[size_t(p)]; }

Prim PrimForToken(Tok t) { return TokenPrim[size_t(t)]; }

Terms::Terms() {
    Values.reserve(4096);
    InternStr(""); // id 0 is the empty lexeme, so payload 0 reads as "none"
}

ValueId Terms::Make(Kind kind, uint8_t form, uint16_t variants, uint32_t payload, std::span<const ValueId> children) {
    uint64_t h = Mix(Seed, (uint64_t(kind) << 32) | (uint64_t(form) << 24) | variants);
    h = Mix(h, payload);
    // Merkle: over child hashes, so a value covers its whole subtree.
    for (const ValueId c : children) h = Mix(h, Hashes[c]);

    auto &bucket = Buckets[h];
    for (const ValueId id : bucket) {
        const TermValue &v = Values[id];
        if (v.Kind != uint8_t(kind) || v.Form != form || v.Variants != variants || v.Payload != payload || v.ChildCount != children.size()) continue;
        if (std::equal(children.begin(), children.end(), ChildPool.begin() + v.Children)) return id;
    }

    // Only a miss commits the children, so a hit leaves nothing to undo.
    const auto offset = uint32_t(ChildPool.size());
    ChildPool.insert(ChildPool.end(), children.begin(), children.end());

    const auto id = ValueId(Values.size());
    Values.push_back({uint8_t(kind), form, variants, payload, offset, uint32_t(children.size())});
    Hashes.push_back(h);
    bucket.push_back(id);
    return id;
}

namespace {

// The empty visitor inlines away, keeping `Innermost` allocation-free.
template<class Visit> RefId Descend(const RefTree &t, uint32_t offset, Visit visit) {
    if (t.Refs.empty()) return NoRef;
    if (offset < t.Refs[0].OuterBegin || offset >= t.Refs[0].OuterEnd) return NoRef;
    RefId cur = 0;
    for (;;) {
        visit(cur);
        const auto kids = t.Children(cur);
        // Spans are disjoint: the candidate is the last child starting at or before `offset`.
        const auto it = std::upper_bound(kids.begin(), kids.end(), offset, [&t](uint32_t off, RefId r) { return off < t.Refs[r].OuterBegin; });
        if (it == kids.begin()) return cur;
        const RefId kid = *(it - 1);
        if (offset >= t.Refs[kid].OuterEnd) return cur;
        cur = kid;
    }
}

} // namespace

RefId RefTree::Innermost(uint32_t offset) const {
    return Descend(*this, offset, [](RefId) {});
}

std::vector<RefId> RefTree::Chain(uint32_t offset) const {
    std::vector<RefId> out;
    Descend(*this, offset, [&out](RefId r) { out.push_back(r); });
    std::ranges::reverse(out);
    return out;
}

} // namespace faustlens
