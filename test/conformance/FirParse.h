// Accept optional list commas in reference .fir notation.
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace faustlens::test {

// Call represents instructions and named or unnamed bracketed groups.
struct FirTerm {
    enum class Kind { Call, Name, Str, Num };

    Kind Kind = Kind::Name;
    // Preserve flag combinations such as StaticStruct|Const as one Name.
    std::string Name;
    double Num = 0;
    char Bracket = '('; // `Call`: one of `(`, `{`, `<`
    std::vector<FirTerm> Args;
    std::vector<FirTerm> Index;
};

// Use indentation to distinguish DeclareFunInst definitions from declarations.
struct FirStmt {
    FirTerm Term;
    std::vector<FirStmt> Body;
};

struct FirSection {
    std::string Name;
    std::vector<FirStmt> Stmts;
    std::vector<std::string> Notes; // unparsed lines in source order
};

struct FirFile {
    std::string Container;
    std::vector<FirSection> Sections;
};

// Return the offending line and reason on failure.
std::expected<FirFile, std::string> ParseFir(std::string_view text);

std::string PrintFirTerm(const FirTerm &);

} // namespace faustlens::test
