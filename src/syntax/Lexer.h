#pragma once

#include "syntax/Diagnostic.h"
#include "syntax/Token.h"

#include <string>
#include <string_view>
#include <vector>

namespace faustlens {

// Mdoc body tags push Default mode; closing tags restore the enclosing mode.
enum class LexMode : uint8_t { Default, Prose, Listing };

struct LexResult {
    TokenVector Tokens;
    std::vector<Diagnostic> Diags;
};

LexResult Lex(std::string_view src);

// Return whether joining moves a token boundary; left must start at a token boundary.
bool WouldFuse(std::string_view left, std::string_view right);

// Return the last token's offset; text must start at a token boundary.
size_t LastTokenBegin(std::string_view text);

// Append text with spacing to preserve token boundaries; anchor tracks the final token from an initial zero.
void AppendUnfused(std::string &out, size_t &anchor, std::string_view text);

} // namespace faustlens
