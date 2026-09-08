// Apply each rewrite once per original node in a bottom-up rebuild.
#pragma once

#include "signal/Signal.h"

#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace faustlens {

// Node receives the original id and rebuilt child ids; arena growth invalidates node references.
template<class Node> struct Rewriter {
    struct Carry {
        Signals &S;
        ValueId Saved;
        Carry(Signals &sigs, ValueId t) : S(sigs), Saved(sigs.OriginNow) { S.OriginNow = t; }
        ~Carry() { S.OriginNow = Saved; }
    };

    Signals &S;
    Node Rebuild;
    std::unordered_map<SigId, SigId> Done;

    Rewriter(Signals &sigs, Node n) : S(sigs), Rebuild(std::move(n)) {}

    SigId Go(SigId id) {
        if (const auto it = Done.find(id); it != Done.end()) return it->second;
        // Copy child ids before recursive construction can grow the arena.
        const std::vector<SigId> old(S.Children(id).begin(), S.Children(id).end());

        // Propagate source origins to all nodes created by the rewrite.
        const Carry carry(S, S.OriginOf(id));

        if (S.KindOf(id) == SigKind::Rec) {
            const SigId g = S.OpenRec();
            Done[id] = g;
            std::vector<SigId> branches;
            branches.reserve(old.size());
            for (const SigId b : old) branches.push_back(Go(b));
            const SigId closed = S.CloseRec(g, branches);
            Done[id] = closed;
            return closed;
        }

        std::vector<SigId> kids;
        kids.reserve(old.size());
        for (const SigId c : old) kids.push_back(Go(c));
        const SigId r = Rebuild(id, kids);
        Done[id] = r;
        return r;
    }
};

// Share the memo across roots.
template<class Node> std::vector<SigId> Rewrite(Signals &s, std::span<const SigId> roots, Node node) {
    Rewriter<Node> r(s, std::move(node));
    std::vector<SigId> out;
    out.reserve(roots.size());
    for (const SigId x : roots) out.push_back(r.Go(x));
    return out;
}

} // namespace faustlens
