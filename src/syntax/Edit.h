// Term rewrites, never bytes. Each answers with one ref and one value, or declines.
#pragma once

#include "syntax/Term.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace faustlens {

struct Edit {
    RefId Target = NoRef;
    ValueId Value = NoTerm;
    // Why, where `Target` is `NoRef`. Shown to the user as-is.
    const char *Declined = nullptr;

    explicit operator bool() const { return Target != NoRef; }
};

// Which side of the selection the new stage goes on.
enum class Side : uint8_t { Before, After };

bool IsComposition(Kind);

bool IsExpression(Kind);

// A label is a payload rather than a child, so a retext targets the widget itself.
constexpr bool IsLabelled(Kind k) { return k >= Kind::Button && k <= Kind::SoundfileBox; }

// Built once per parse.
struct EditContext {
    Terms &Terms;
    const RefTree &Refs;
    std::vector<RefId> ParentOf;

    EditContext(faustlens::Terms &, const RefTree &);

    // The parent link the ref tree does not store.
    RefId Parent(RefId r) const { return r < ParentOf.size() ? ParentOf[r] : NoRef; }

    // The selection and `stage` (default `_`) under a new connective. Built at the parent
    // where that avoids parens.
    Edit Compose(RefId sel, Kind comp, uint8_t form, Side, ValueId stage = NoTerm);

    // The composition holding the selection becomes its sibling.
    Edit Delete(RefId sel);

    // `text` is the source spelling, quotes included. The kind follows the bytes.
    Edit Retext(RefId sel, std::string_view text);

    // One (input, output) pair added or removed, both 1-based as in the source.
    Edit Connect(RefId route, uint32_t in, uint32_t out);
    Edit Disconnect(RefId route, uint32_t in, uint32_t out);

    // The route's entries as the flat channel list they denote.
    std::vector<ValueId> Entries(RefId route) const;

    ValueId ValueOf(RefId r) const { return Refs.Refs[r].ValueId; }
    Kind KindAt(RefId r) const { return Terms.KindOf(ValueOf(r)); }
    // Entries as the flat channel list, or the reason there is no rewire to be had.
    const char *PairedEntries(RefId route, std::vector<ValueId> &out) const;
    // Folded in the connective's own associativity, so a reparse round-trips.
    ValueId Fold(Kind, uint8_t form, std::span<const ValueId>);
    Edit Rewire(RefId route, std::span<const ValueId> entries);
};

// A route's channel counts and (input, output) pairs, all 1-based. What the rewires
// refuse is zeroed or dropped here too.
struct Wiring {
    uint32_t Ins = 0, Outs = 0;
    std::vector<std::pair<uint32_t, uint32_t>> Pairs;

    bool Drawable() const { return Ins > 0 && Outs > 0; }
};
Wiring RouteWiring(const Terms &, ValueId);

} // namespace faustlens
