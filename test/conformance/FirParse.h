// Reader for the reference's `.fir` notation, the oracle for Plan. Its printer omits
// separators, so a list comma is optional.
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace faustlens::test {

// `Call` covers every `XxxInst(...)` and every bracketed group, named or not.
struct FirTerm {
    enum class Kind { Call, Name, Str, Num };

    Kind Kind = Kind::Name;
    // A flag word keeps its bar: `StaticStruct|Const` is one `Name`.
    std::string Name;
    double Num = 0; // `Num` only
    char Bracket = '('; // `Call`: one of `(`, `{`, `<`
    std::vector<FirTerm> Args; // `Call`
    std::vector<FirTerm> Index;
};

// `DeclareFunInst` brackets only when it is a definition, and only indentation tells it apart.
struct FirStmt {
    FirTerm Term;
    std::vector<FirStmt> Body;
};

struct FirSection {
    std::string Name; // as written between the `=` runs, less any ` begin` suffix
    std::vector<FirStmt> Stmts;
    std::vector<std::string> Notes; // lines the statement grammar has no shape for, in order
};

struct FirFile {
    std::string Container; // the `Container "mydsp"` marker's name
    std::vector<FirSection> Sections;
};

// Unexpected gets the offending line and a reason.
std::expected<FirFile, std::string> ParseFir(std::string_view text);

std::string PrintFirTerm(const FirTerm &);

} // namespace faustlens::test
