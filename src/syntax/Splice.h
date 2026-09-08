#pragma once

#include "syntax/Edit.h"
#include "syntax/Printer.h"
#include "syntax/Term.h"
#include "syntax/Token.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace faustlens {

struct Replacement {
    uint32_t Begin = 0, End = 0;
    std::string Text;
};

// Replacements are disjoint and source-ordered; relocated spans are copied.
using EditScript = std::vector<Replacement>;

std::string ApplyScript(std::string_view src, const EditScript &);

// Source context reusable across edits to one parse.
struct SpliceContext {
    const Terms &Terms;
    std::string_view Src;
    const RefTree &Refs;
    const TokenVector &Tokens;
    std::vector<Ctx> Ctxs;
    std::vector<uint32_t> LineStarts;
    std::unordered_map<ValueId, std::vector<RefId>> ByValue;

    SpliceContext(const faustlens::Terms &, std::string_view src, const RefTree &, const TokenVector &);

    // Return the target's print context with its starting column as indent.
    Ctx At(RefId target) const;

    // Keep replacements within the target outer span.
    EditScript Splice(RefId target, ValueId new_root) const;
    EditScript Splice(RefId target, ValueId new_root, const Ctx &ctx0) const;
    // Use explicit links to retain, relocate, or copy source occurrences.
    EditScript Splice(const Edit &) const;

    // Return matching refs sorted by OuterBegin, or null.
    const std::vector<RefId> *Claims(ValueId) const;
};

} // namespace faustlens
