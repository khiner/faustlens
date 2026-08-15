// The storage a hash-consed graph needs: nodes by id, an interning table and one child
// pool. Box and Signal are these, `Terms` is not.
#pragma once

#include "syntax/Term.h" // ValueId, NoTerm

#include <algorithm>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <unordered_map>
#include <vector>

namespace faustlens {

// A number rides in `Payload`, a double taking `Aux` for its high word.
constexpr uint32_t BitsOf(int32_t v) { return std::bit_cast<uint32_t>(v); }
constexpr int32_t IntOf(uint32_t bits) { return std::bit_cast<int32_t>(bits); }
constexpr uint64_t BitsOf(double v) { return std::bit_cast<uint64_t>(v); }
constexpr double RealOf(uint32_t lo, uint32_t hi) { return std::bit_cast<double>(lo | (uint64_t(hi) << 32)); }

// `Kind` and `Form` discriminate, `Payload` and `Aux` are the per-kind words.
struct ArenaNode {
    uint8_t Kind = 0;
    uint8_t Form = 0;
    uint32_t Payload = 0;
    uint32_t Aux = 0;
    uint32_t Children = 0; // offset into the child pool
    uint32_t ChildCount = 0;
};

// Interning is `Find` then `Commit`, not one call: what a miss costs is the derived
// arena's business, Box inferring an arity there and possibly refusing. Every node
// routes through `Derived::Make`.
template<class Derived, class Kind, class Id> struct Arena {
    static constexpr Id NotFound = Id(-1);

    std::vector<ArenaNode> Nodes;
    std::vector<uint64_t> Hashes;
    std::vector<Id> ChildPool;
    std::unordered_map<uint64_t, std::vector<Id>> Buckets;
    // Provenance beside the node, not in it: folding it in would split two occurrences of
    // one expression and lose the free CSE. `OriginNow` is the cursor, first writer wins.
    std::vector<ValueId> Origin;
    ValueId OriginNow = NoTerm;
    // Poison, interned first by the derived constructor.
    Id Error = NotFound;

    Arena() { Nodes.reserve(4096); }

    Derived &Self() { return static_cast<Derived &>(*this); }

    Id Make(Kind k, uint8_t form, uint32_t payload, uint32_t aux, std::initializer_list<Id> children) {
        return Self().Make(k, form, payload, aux, std::span<const Id>(children.begin(), children.size()));
    }
    // The common shape: a kind and its children, every other word zero.
    Id Make(Kind k, std::span<const Id> children) { return Self().Make(k, 0, 0, 0, children); }
    Id Make(Kind k, std::initializer_list<Id> children) { return Self().Make(k, 0, 0, 0, std::span<const Id>(children.begin(), children.size())); }
    Id MakeLeaf(Kind k, uint32_t payload = 0, uint32_t aux = 0) { return Self().Make(k, 0, payload, aux, std::span<const Id>{}); }
    Id Rebuild(Id id, std::span<const Id> children) {
        const ArenaNode n = Nodes[id]; // by value: `Make` grows the arena
        return Self().Make(Kind(n.Kind), n.Form, n.Payload, n.Aux, children);
    }
    Id Rebuild(Id id, std::initializer_list<Id> children) { return Rebuild(id, std::span<const Id>(children.begin(), children.size())); }
    Id MakeInt(int32_t v) { return MakeLeaf(Kind::Int, BitsOf(v)); }
    Id MakeReal(double v) {
        const uint64_t bits = BitsOf(v);
        return MakeLeaf(Kind::Real, uint32_t(bits), uint32_t(bits >> 32));
    }

    bool IsError(Id id) const { return id == Error; }

    const ArenaNode &Get(Id id) const { return Nodes[id]; }
    Kind KindOf(Id id) const { return Kind(Nodes[id].Kind); }
    std::span<const Id> Children(Id id) const {
        const ArenaNode &n = Nodes[id];
        return {ChildPool.data() + n.Children, n.ChildCount};
    }
    Id Child(Id id, uint32_t i) const { return Children(id)[i]; }
    uint64_t Hash(Id id) const { return Hashes[id]; }
    size_t Size() const { return Nodes.size(); }
    ValueId OriginOf(Id id) const { return id < Origin.size() ? Origin[id] : NoTerm; }

    int32_t IntValue(Id id) const { return IntOf(Nodes[id].Payload); }
    double RealValue(Id id) const { return RealOf(Nodes[id].Payload, Nodes[id].Aux); }

    // `NotFound` where nothing equal has been interned under `hash`.
    Id Find(uint64_t hash, const ArenaNode &proto, std::span<const Id> children) const {
        const auto at = Buckets.find(hash);
        if (at == Buckets.end()) return NotFound;
        for (const Id id : at->second) {
            const ArenaNode &n = Nodes[id];
            if (n.Kind != proto.Kind || n.Form != proto.Form || n.Payload != proto.Payload || n.Aux != proto.Aux || n.ChildCount != children.size()) continue;
            if (std::equal(children.begin(), children.end(), ChildPool.begin() + n.Children)) return id;
        }
        return NotFound;
    }

    // Only a miss commits the children, so a hit leaves nothing to undo.
    Id Commit(ArenaNode n, uint64_t hash, std::span<const Id> children) {
        n.Children = uint32_t(ChildPool.size());
        n.ChildCount = uint32_t(children.size());
        ChildPool.insert(ChildPool.end(), children.begin(), children.end());
        const auto id = Id(Nodes.size());
        Nodes.push_back(n);
        Hashes.push_back(hash);
        Origin.push_back(OriginNow);
        Buckets[hash].push_back(id);
        return id;
    }
};

} // namespace faustlens
