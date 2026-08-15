#include "eval/Env.h"

#include <algorithm>

namespace faustlens {
namespace {

constexpr uint64_t Seed = 0x2545F4914F6CDD1Dull;

bool Same(const Binding &a, const Binding &b) { return a.Name == b.Name && a.Kind == b.Kind && a.Id == b.Id && a.Env == b.Env && a.Meta == b.Meta; }

} // namespace

Envs::Envs() {
    Nodes.push_back({}); // NilEnv
    Index.emplace_back();
}

EnvId Envs::Push(EnvId parent, std::span<const Binding> bindings, bool barrier, uint32_t resolution) {
    uint64_t h = Mix(Seed, Nodes[parent].Hash);
    h = Mix(h, barrier ? 1u : 0u);
    h = Mix(h, resolution);
    for (const Binding &b : bindings) {
        h = Mix(h, (uint64_t(b.Name) << 32) | uint32_t(b.Kind));
        h = Mix(h, (uint64_t(b.Id) << 32) | b.Env);
        h = Mix(h, b.Meta);
    }

    auto &bucket = Buckets[h];
    for (const EnvId id : bucket) {
        const Node &n = Nodes[id];
        if (n.Parent != parent || n.Barrier != barrier || n.Resolution != resolution || n.Count != bindings.size()) continue;
        if (std::equal(bindings.begin(), bindings.end(), Pool.begin() + n.First, Same)) return id;
    }

    const auto first = uint32_t(Pool.size());
    Pool.insert(Pool.end(), bindings.begin(), bindings.end());
    const auto id = EnvId(Nodes.size());
    Nodes.push_back({parent, first, uint32_t(bindings.size()), resolution, barrier, h});
    Index.emplace_back();
    // Later wins, matching a lookup that scans outward from the end.
    for (uint32_t i = 0; i < bindings.size(); ++i) Index[id][bindings[i].Name] = i;
    bucket.push_back(id);
    return id;
}

EnvId Envs::PushValue(EnvId parent, StrId name, BindKind kind, uint32_t id, EnvId env) {
    const Binding b{name, kind, id, env};
    return Push(parent, {&b, 1});
}

const Binding *Envs::LookupLocal(EnvId e, StrId name) const {
    const auto &ix = Index[e];
    const auto it = ix.find(name);
    return it == ix.end() ? nullptr : &Pool[Nodes[e].First + it->second];
}

const Binding *Envs::Lookup(EnvId e, StrId name, bool stop_at_barrier) const {
    for (; e != NilEnv; e = Nodes[e].Parent) {
        if (stop_at_barrier && Nodes[e].Barrier) return nullptr;
        if (const Binding *b = LookupLocal(e, name)) return b;
    }
    return nullptr;
}

uint32_t Envs::ResolutionOf(EnvId e) const {
    for (; e != NilEnv; e = Nodes[e].Parent)
        if (Nodes[e].Resolution != NoResolution) return Nodes[e].Resolution;
    return NoResolution;
}

EnvId Envs::ReplaceDefs(EnvId layer, std::span<const Binding> replacements) {
    // A definition names its own layer implicitly, so nothing needs re-pointing.
    std::vector<Binding> merged(Bindings(layer).begin(), Bindings(layer).end());
    for (const Binding &r : replacements) {
        const auto it = std::ranges::find_if(merged, [&](const Binding &b) { return b.Name == r.Name; });
        if (it != merged.end()) *it = r;
        else merged.push_back(r);
    }
    return Push(Nodes[layer].Parent, merged, Nodes[layer].Barrier, Nodes[layer].Resolution);
}

} // namespace faustlens
