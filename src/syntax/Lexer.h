#pragma once

#include "syntax/Diagnostic.h"
#include "syntax/Token.h"

#include <string>
#include <string_view>
#include <vector>

namespace faustlens {

// mdoc body tags push Default and their closers pop.
enum class LexMode : uint8_t { Default, Prose, Listing };

struct LexResult {
    TokenVector Tokens;
    std::vector<Diagnostic> Diags;
};

LexResult Lex(std::string_view src);

// True where joining moves a token boundary, as `3` beside `.name`. `left` must
// begin at a token boundary.
bool WouldFuse(std::string_view left, std::string_view right);

// Offset of the last token of `text`, which must begin at a token boundary.
size_t LastTokenBegin(std::string_view text);

// Appends `text` to `out`, spacing the seam where it would fuse. `anchor` is the start
// of `out`'s final token, carried forward from 0.
void AppendUnfused(std::string &out, size_t &anchor, std::string_view text);

} // namespace faustlens
