// Renders a new value tree into retained spans and printed text, against the file.
// Not a tree diff: interned values make matching a lookup.
#pragma once

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

// Disjoint, in source order, and never covering a retained span.
using EditScript = std::vector<Replacement>;

std::string ApplyScript(std::string_view src, const EditScript &);

// What a splice needs from the file rather than the edit. Built once per parse.
struct SpliceContext {
    const Terms &Terms;
    std::string_view Src;
    const RefTree &Refs;
    const TokenVector &Tokens;
    std::vector<Ctx> Ctxs;
    std::vector<uint32_t> LineStarts;
    std::unordered_map<ValueId, std::vector<RefId>> ByValue;

    SpliceContext(const faustlens::Terms &, std::string_view src, const RefTree &, const TokenVector &);

    // The print position of `target`, with the indent set to its start column.
    Ctx At(RefId target) const;

    // Every replacement lies inside `target`'s outer span.
    EditScript Splice(RefId target, ValueId new_root) const;
    EditScript Splice(RefId target, ValueId new_root, const Ctx &ctx0) const;

    // Every ref carrying this value, sorted by `OuterBegin`. Null where none.
    const std::vector<RefId> *Claims(ValueId) const;
};

} // namespace faustlens
