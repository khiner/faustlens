#pragma once

#include "syntax/Diagnostic.h"
#include "syntax/Lexer.h"
#include "syntax/Term.h"

#include <string_view>
#include <vector>

namespace faustlens {

struct ParseResult {
    ValueId Root = NoTerm; // a `Program`
    RefTree Refs;
    TokenVector Tokens; // tiles the file
    std::vector<Diagnostic> Diags;

    bool Ok() const { return Diags.empty(); }
};

ParseResult Parse(Terms &, std::string_view src, uint32_t file_index = 0);

} // namespace faustlens
