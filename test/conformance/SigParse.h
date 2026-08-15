// The reference's `.sig` notation, not Faust source: a `// Size = N` header, one
// `ID_n = <op>` line per node, then `SIG = (...)`. Every shared subterm gets its own
// `ID`, so no line chains two same-priority operators.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace faustlens::test {

struct SigTerm {
    enum class Kind {
        Id, // `ID_7`, a reference to another line
        RecVar, // `W0`, a `letrec` variable, forward-referenced
        Input, // `IN[3]`
        Int, // `65536`
        Real, // `1.92e+05`
        Waveform, // `waveform{...}`, contents elided by the printer
        Name, // `fSamplingFreq`, a foreign constant or variable
        String, // a UI label, never escaped
        List, // `(a, b, c)`
        Op, // everything else, `Text` naming it: `+` `@` `'` `int` `proj0` ...
    };

    Kind Kind = Kind::Op;
    std::string Text; // the operator, function, name, or label
    int64_t I = 0; // Int, and the index for Id / RecVar / Input
    double D = 0; // Real
    std::vector<SigTerm> Args;
};

struct SigFile {
    int32_t Size = -1; // the `// Size = N` header, -1 if absent
    std::vector<SigTerm> Defs; // defs[n] is `ID_n`'s right-hand side
    SigTerm Outputs; // `SIG = (...)`, always a List
};

// Unexpected gets the offending line and a reason.
std::expected<SigFile, std::string> ParseSig(std::string_view text);

// The term as text, not the reference's notation.
std::string PrintSigTerm(const SigTerm &);

} // namespace faustlens::test
