// Intern equivalent environment layers for evaluation memo keys.
#pragma once

#include "box/Box.h"
#include "syntax/Term.h"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace faustlens {

inline constexpr EnvId NilEnv = 0;
inline constexpr uint32_t NoResolution = 0xFFFFFFFFu;

// Derive a definition's closure environment from its containing layer.
enum class BindKind : uint8_t {
    Definition,
    Closure, // `e[defs]`'s replacements
    Term, // an iteration index
    Value, // an already-evaluated box: a lambda parameter or a pattern variable
};

struct Binding {
    StrId Name = 0;
    BindKind Kind = BindKind::Definition;
    uint32_t Id = 0; // a ValueId or a BoxId, per `Kind`
    EnvId Env = NilEnv; // BindKind::Closure only
    // `declare name key "v"` pairs, published only if the definition is evaluated.
    uint32_t Meta = 0xFFFFFFFFu;
};

struct Envs {
    struct Node {
        EnvId Parent = NilEnv;
        uint32_t First = 0, Count = 0;
        uint32_t Resolution = NoResolution;
        bool Barrier = false;
        uint64_t Hash = 0;
    };
    std::vector<Node> Nodes;
    std::vector<Binding> Pool;
    std::unordered_map<uint64_t, std::vector<EnvId>> Buckets;
    // Index imported definitions to avoid scanning the full library on lookup.
    std::vector<std::unordered_map<StrId, uint32_t>> Index;

    Envs();

    EnvId Push(EnvId parent, std::span<const Binding> bindings, bool barrier = false, uint32_t resolution = NoResolution);
    EnvId PushValue(EnvId parent, StrId name, BindKind, uint32_t id, EnvId env = NilEnv);
    // Stop only stop_at_barrier lookups at this layer.
    EnvId PushBarrier(EnvId parent) { return Push(parent, {}, true); }

    EnvId Parent(EnvId e) const { return Nodes[e].Parent; }
    std::span<const Binding> Bindings(EnvId e) const {
        const Node &n = Nodes[e];
        return {Pool.data() + n.First, n.Count};
    }

    const Binding *Lookup(EnvId, StrId name, bool stop_at_barrier = false) const;
    const Binding *LookupLocal(EnvId, StrId name) const;

    // Nearest enclosing file for component and library resolution.
    uint32_t ResolutionOf(EnvId) const;

    EnvId ReplaceDefs(EnvId layer, std::span<const Binding> replacements);
};

} // namespace faustlens
