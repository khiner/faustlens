// Parse reference .sig files with a size header, node definitions, and a SIG output list.
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
    double D = 0;
    std::vector<SigTerm> Args;
};

struct SigFile {
    int32_t Size = -1; // the `// Size = N` header, -1 if absent
    std::vector<SigTerm> Defs; // defs[n] is `ID_n`'s right-hand side
    SigTerm Outputs; // `SIG = (...)`, always a List
};

// Return the offending line and reason on failure.
std::expected<SigFile, std::string> ParseSig(std::string_view text);

// Format the parsed term for diagnostics.
std::string PrintSigTerm(const SigTerm &);

} // namespace faustlens::test
