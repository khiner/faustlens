#pragma once

#include "syntax/Term.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace faustlens {

struct SourceLink {
    std::vector<uint32_t> Path; // child indices from the replacement root
    RefId Source = NoRef;
};

struct Edit {
    RefId Target = NoRef;
    ValueId Value = NoTerm;
    // User-facing refusal reason when Target is NoRef.
    const char *Declined = nullptr;
    std::vector<SourceLink> Links;

    explicit operator bool() const { return Target != NoRef; }
};

enum class Side : uint8_t { Before, After };

bool IsComposition(Kind);

bool IsExpression(Kind);

// Retext labels through their containing widget node.
constexpr bool IsLabelled(Kind k) { return k >= Kind::Button && k <= Kind::SoundfileBox; }

struct EditContext {
    Terms &Terms;
    const RefTree &Refs;
    std::vector<RefId> ParentOf;

    EditContext(faustlens::Terms &, const RefTree &);

    RefId Parent(RefId r) const { return r < ParentOf.size() ? ParentOf[r] : NoRef; }

    // Combine the selection and stage under a connective, using the parent to avoid redundant grouping.
    Edit Compose(RefId sel, Kind comp, uint8_t form, Side, ValueId stage = NoTerm);

    // Replace the containing composition with the selection's sibling.
    Edit Delete(RefId sel);

    // Interpret text as source spelling, including quotes.
    Edit Retext(RefId sel, std::string_view text);

    // Add or remove one pair of 1-based source channels.
    Edit Connect(RefId route, uint32_t in, uint32_t out) { return Rewire(route, in, out, true); }
    Edit Disconnect(RefId route, uint32_t in, uint32_t out) { return Rewire(route, in, out, false); }

    std::vector<ValueId> Entries(RefId route) const;

    ValueId ValueOf(RefId r) const { return Refs.Refs[r].ValueId; }
    Kind KindAt(RefId r) const { return Terms.KindOf(ValueOf(r)); }
    Edit Rewire(RefId route, uint32_t in, uint32_t out, bool connect);
    std::vector<RefId> EntryRefs(RefId route) const;
};

// Return valid 1-based route channels and pairs, excluding entries that cannot be rewired.
struct Wiring {
    uint32_t Ins = 0, Outs = 0;
    std::vector<std::pair<uint32_t, uint32_t>> Pairs;

    bool Drawable() const { return Ins > 0 && Outs > 0; }
};
Wiring RouteWiring(const Terms &, ValueId);

} // namespace faustlens
