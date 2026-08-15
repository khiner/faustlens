#include "syntax/Lexer.h"

#include <algorithm>
#include <span>

namespace faustlens {
namespace {

constexpr bool IsDigit(char c) { return c >= '0' && c <= '9'; }
constexpr bool IsLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool IsIdBody(char c) { return IsLetter(c) || IsDigit(c) || c == '_'; }
constexpr bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Looked up on a whole maximal NSID, so `min` hits but `minimum` and `min::x` do not.
Tok LookupKeyword(std::string_view word) {
    const std::span<const Spelling> table = Keywords();
    const auto it = std::ranges::lower_bound(table, word, {}, &Spelling::Text);
    return (it != table.end() && it->Text == word) ? it->Kind : Tok::Ident;
}

struct Lexer {
    std::string_view Src;
    uint32_t I = 0;
    std::vector<LexMode> Modes{LexMode::Default};
    TokenVector Out;
    std::vector<Diagnostic> Diags;

    explicit Lexer(std::string_view s) : Src(s) {}

    LexResult Run() {
        while (I < Src.size()) {
            switch (Modes.back()) {
                case LexMode::Default: Default(); break;
                case LexMode::Prose: Prose(); break;
                case LexMode::Listing: Listing(); break;
            }
        }
        if (Modes.back() == LexMode::Prose || Modes.back() == LexMode::Listing || Modes.size() > 1) { Diag(Code::SynUnterminatedMdoc, I, I); }
        Push(Tok::Eof, I);
        return {std::move(Out), std::move(Diags)};
    }

    char At(uint32_t k) const { return k < Src.size() ? Src[k] : '\0'; }
    bool Has(std::string_view lit) const { return Src.compare(I, lit.size(), lit) == 0; }

    void Push(Tok kind, uint32_t end) {
        Out.push_back({kind, I, end});
        I = end;
    }
    bool TryTable(std::span<const Spelling> table) {
        for (const Spelling &t : table) {
            if (Has(t.Text)) {
                Push(t.Kind, I + uint32_t(t.Text.size()));
                return true;
            }
        }
        return false;
    }
    void PushMerged(Tok kind, uint32_t end) {
        if (!Out.empty() && Out.back().Kind == kind && Out.back().End == I) {
            Out.back().End = end;
            I = end;
        } else {
            Push(kind, end);
        }
    }
    void Diag(Code code, uint32_t begin, uint32_t end) { Diags.push_back({.Code = code, .Begin = begin, .End = end}); }
    bool Space() {
        if (!IsSpace(At(I))) return false;
        uint32_t j = I;
        while (j < Src.size() && IsSpace(Src[j])) ++j;
        Push(Tok::Whitespace, j);
        return true;
    }
    void PopMode() {
        if (Modes.size() > 1) Modes.pop_back();
    }

    void Default() {
        if (Space()) return;
        const char c = At(I);
        if (c == '/' && At(I + 1) == '/') {
            uint32_t j = I + 2;
            while (j < Src.size() && Src[j] != '\n' && Src[j] != '\r') ++j;
            Push(Tok::LineComment, j);
            return;
        }
        if (c == '/' && At(I + 1) == '*') {
            const auto close = Src.find("*/", I + 2);
            if (close == std::string_view::npos) {
                Diag(Code::SynUnterminatedComment, I, uint32_t(Src.size()));
                Push(Tok::BlockComment, uint32_t(Src.size()));
            } else {
                Push(Tok::BlockComment, uint32_t(close + 2));
            }
            return;
        }
        if (c == '"') {
            const auto close = Src.find('"', I + 1);
            if (close == std::string_view::npos) {
                Diag(Code::SynUnterminatedString, I, I + 1);
                PushMerged(Tok::Unknown, I + 1);
            } else {
                Push(Tok::String, uint32_t(close + 1));
            }
            return;
        }
        if (IsDigit(c) || (c == '.' && IsDigit(At(I + 1)))) {
            Number();
            return;
        }
        if (c == '<') {
            // `<mdoc>` and FSTRING match the same six bytes, so `<mdoc>` is tested first.
            if (Has("<mdoc>")) {
                Push(Tok::BDoc, I + 6);
                Modes.push_back(LexMode::Prose);
                return;
            }
            if (uint32_t const end = ScanFString(); end != I) {
                Push(Tok::FString, end);
                return;
            }
            // Body closers pop back to prose. Below FSTRING, which a `</` never starts.
            static constexpr Spelling Closers[] = {
                {"</equation>", Tok::EEqn},
                {"</diagram>", Tok::EDgm},
                {"</metadata>", Tok::EMetadata},
            };
            if (TryTable(Closers)) {
                PopMode();
                return;
            }
        }
        if (c == ':' && At(I + 1) == ':') {
            if (const uint32_t end = ScanNsid(I + 2); end != I + 2) {
                Push(Tok::Ident, end); // a leading `::` is part of the identifier
                return;
            }
        }
        if (IsLetter(c) || c == '_') {
            if (const uint32_t end = ScanNsid(I); end != I) {
                Push(LookupKeyword(Src.substr(I, end - I)), end);
                return;
            }
        }
        if (ScanOperator()) return;
        PushMerged(Tok::Unknown, I + 1);
    }

    void Number() {
        uint32_t j = I;
        bool is_float = false;
        if (Src[j] == '.') {
            is_float = true;
            ++j;
            while (j < Src.size() && IsDigit(Src[j])) ++j;
        } else {
            while (j < Src.size() && IsDigit(Src[j])) ++j;
            if (At(j) == '.') {
                is_float = true;
                ++j;
                while (j < Src.size() && IsDigit(Src[j])) ++j;
            }
        }
        // An exponent counts only whole: `3e` is Int `3` then the ident `e`.
        if (At(j) == 'e') {
            uint32_t k = j + 1;
            if (At(k) == '-' || At(k) == '+') ++k;
            if (IsDigit(At(k))) {
                while (k < Src.size() && IsDigit(Src[k])) ++k;
                j = k;
                is_float = true;
            }
        }
        if (At(j) == 'f') {
            ++j;
            is_float = true;
        }
        Push(is_float ? Tok::Float : Tok::Int, j);
    }

    uint32_t ScanNsid(uint32_t from) const {
        uint32_t j = ScanId(from);
        if (j == from) return from;
        while (At(j) == ':' && At(j + 1) == ':') {
            const uint32_t next = ScanId(j + 2);
            if (next == j + 2) break; // a trailing `::` is not part of the NSID
            j = next;
        }
        return j;
    }

    uint32_t ScanId(uint32_t from) const {
        uint32_t j = from;
        while (At(j) == '_') ++j;
        if (!IsLetter(At(j))) return from; // `_` alone is WIRE, not an ID
        ++j;
        while (j < Src.size() && IsIdBody(Src[j])) ++j;
        return j;
    }

    // `<letters>` or `<letters.letter>`, beating `<` as less-than so `<b>` is an FString.
    uint32_t ScanFString() const {
        uint32_t j = I + 1;
        while (j < Src.size() && IsLetter(Src[j])) ++j;
        if (At(j) == '>') return j + 1;
        if (At(j) == '.' && IsLetter(At(j + 1)) && At(j + 2) == '>') return j + 3;
        return I;
    }

    // Two-byte spellings come first, so the scan is maximal-munch.
    bool ScanOperator() {
        static constexpr Spelling Ops[] = {
            {"<:", Tok::Split},  {"+>", Tok::Mix}, {":>", Tok::Mix},   {"<<", Tok::Lsh},    {">>", Tok::Rsh},  {"<=", Tok::Le},    {">=", Tok::Ge},
            {"==", Tok::Eq},     {"!=", Tok::Ne},  {"=>", Tok::Arrow}, {"->", Tok::LApply}, {":", Tok::Seq},   {",", Tok::Par},    {"~", Tok::Rec},
            {"+", Tok::Add},     {"-", Tok::Sub},  {"*", Tok::Mul},    {"/", Tok::Div},     {"%", Tok::Mod},   {"@", Tok::FDelay}, {"'", Tok::Delay1},
            {"&", Tok::And},     {"|", Tok::Or},   {"<", Tok::Lt},     {">", Tok::Gt},      {"_", Tok::Wire},  {"!", Tok::Cut},    {";", Tok::EndDef},
            {"=", Tok::Def},     {"(", Tok::LPar}, {")", Tok::RPar},   {"{", Tok::LBraq},   {"}", Tok::RBraq}, {"[", Tok::LCroc},  {"]", Tok::RCroc},
            {"\\", Tok::Lambda}, {".", Tok::Dot},  {"^", Tok::PowOp},
        };
        return TryTable(Ops);
    }

    void Prose() {
        struct Tag {
            std::string_view Text;
            Tok Kind;
            int8_t Mode; // -1 pop, 0 none, 1 push Default, 2 push Listing
        };
        static constexpr Tag Tags[] = {
            {"<notice />", Tok::Notice, 0}, {"<notice/>", Tok::Notice, 0},     {"<listing", Tok::BLst, 2}, {"<equation>", Tok::BEqn, 1},
            {"<diagram>", Tok::BDgm, 1},    {"<metadata>", Tok::BMetadata, 1}, {"</mdoc>", Tok::EDoc, -1},
        };
        for (const Tag &t : Tags) {
            if (Has(t.Text)) {
                Push(t.Kind, I + uint32_t(t.Text.size()));
                if (t.Mode == -1) PopMode();
                if (t.Mode == 1) Modes.push_back(LexMode::Default);
                if (t.Mode == 2) Modes.push_back(LexMode::Listing);
                return;
            }
        }
        PushMerged(Tok::DocChar, I + 1);
    }

    void Listing() {
        if (Space()) return;
        if (Has("/>")) {
            Push(Tok::ELst, I + 2);
            PopMode();
            return;
        }
        static constexpr Spelling Attrs[] = {
            {"dependencies", Tok::LstDependencies},
            {"distributed", Tok::LstDistributed},
            {"mdoctags", Tok::LstMdoctags},
            {"false", Tok::LstFalse},
            {"true", Tok::LstTrue},
            {"=", Tok::LstEq},
            {"\"", Tok::LstQ},
        };
        if (TryTable(Attrs)) return;
        PushMerged(Tok::Unknown, I + 1);
    }
};

} // namespace

LexResult Lex(std::string_view src) { return Lexer(src).Run(); }

size_t LastTokenBegin(std::string_view text) {
    size_t begin = 0;
    for (const Token &t : Lex(text).Tokens) {
        if (t.Kind == Tok::Eof) break;
        begin = t.Begin;
    }
    return begin;
}

bool WouldFuse(std::string_view left, std::string_view right) {
    if (left.empty() || right.empty()) return false;
    if (IsSpace(left.back()) || IsSpace(right.front())) return false;

    // Windowing `right` is safe: bytes past it cannot reopen a token closed before the
    // seam.
    constexpr size_t Window = 256;
    const size_t seam = left.size();
    std::string joined;
    joined.reserve(seam + Window);
    joined.append(left);
    joined.append(right.substr(0, std::min(Window, right.size())));

    for (const Token &t : Lex(joined).Tokens) {
        if (t.Kind == Tok::Eof || t.Begin >= seam) break;
        if (t.End > seam) return true;
    }
    return false;
}

void AppendUnfused(std::string &out, size_t &anchor, std::string_view text) {
    if (text.empty()) return;
    if (WouldFuse(std::string_view(out).substr(anchor), text)) out += ' ';
    out.append(text);
    anchor += LastTokenBegin(std::string_view(out).substr(anchor));
}

} // namespace faustlens
