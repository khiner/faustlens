#include "signal/Signal.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <unordered_map>

namespace faustlens {
namespace {

constexpr std::array<std::string_view, size_t(SigKind::Count_)> KindNames = {
    "Int",     "Real",      "Waveform", "FConst",    "FVar",      "Input",     "BinOp",           "Delay1",        "Delay",           "Prefix",
    "IntCast", "FloatCast", "BitCast",  "Select2",   "Select3",   "WRTbl",     "RDTbl",           "Gen",           "Button",          "Checkbox",
    "VSlider", "HSlider",   "NumEntry", "VBargraph", "HBargraph", "Soundfile", "SoundfileLength", "SoundfileRate", "SoundfileBuffer", "FFun",
    "Attach",  "Control",   "Enable",   "Rec",       "Proj",      "Extended",  "Error",
};

constexpr std::array<std::string_view, size_t(BinOpCode::Count_)> BinOpNames = {
    "+", "-", "*", "/", "%", "<<", ">>", ">>>", ">", "<", ">=", "<=", "==", "!=", "&", "|", "^",
};

constexpr std::array<std::string_view, size_t(Ext::Count_)> ExtNames = {
    "abs",   "acos", "acosh", "asin", "asinh",     "atan", "atan2", "atanh", "ceil", "cos",  "cosh", "exp",  "floor",        "fmod",   "log",
    "log10", "max",  "min",   "pow",  "remainder", "rint", "round", "sin",   "sinh", "sqrt", "tan",  "tanh", "assertbounds", "lowest", "highest",
};

// A self-reference Hashes as this, so a group Hashes by shape and not by its id.
constexpr uint64_t SelfMarker = 0xD1B54A32D192ED03ull;

// Its own seed, so a node's Merkle hash and its intern hash are never equal.
constexpr uint64_t MerkleSeed = 0xB5026F5AA96619E9ull;

} // namespace

std::string_view SigKindName(SigKind k) { return KindNames[size_t(k)]; }
std::string_view BinOpName(BinOpCode b) { return BinOpNames[size_t(b)]; }
std::string_view ExtName(Ext e) { return ExtNames[size_t(e)]; }

Signals::Signals() { Error = MakeLeaf(SigKind::Error); }

uint64_t Signals::HashOf(SigKind k, uint8_t form, uint32_t payload, uint32_t aux, std::span<const SigId> children, SigId self) const {
    uint64_t h = Mix(0x243f6a8885a308d3ull, uint64_t(k));
    h = Mix(h, form);
    h = Mix(h, payload);
    h = Mix(h, aux);
    // Child **ids**, not child Hashes, so this agrees with `Arena::Find`.
    for (const SigId c : children) h = Mix(h, c == self ? SelfMarker : c);
    return h;
}

SigId Signals::Make(SigKind k, uint8_t form, uint32_t payload, uint32_t aux, std::span<const SigId> children) {
    for (const SigId c : children)
        if (c == Error) return Error;
    const SigNode proto{uint8_t(k), form, payload, aux, 0, 0};
    const uint64_t h = HashOf(k, form, payload, aux, children, NoSig);
    if (const SigId id = Find(h, proto, children); id != NotFound) return id;
    return Commit(proto, h, children);
}

// Not interned: a `Rec` with no branches yet would equal every other open group.
SigId Signals::OpenRec() {
    const SigId id = SigId(Nodes.size());
    Nodes.push_back({uint8_t(SigKind::Rec), 0, 0, /*aux=*/1, 0, 0}); // "still open"
    Hashes.push_back(Mix(SelfMarker, id));
    Origin.push_back(OriginNow);
    return id;
}

SigId Signals::CloseRec(SigId reserved, std::span<const SigId> branches) {
    const uint64_t hash = HashOf(SigKind::Rec, 0, 0, 0, branches, reserved);

    // Same branch *ids* only. Structural equality merges groups the reference keeps apart.
    std::vector<SigId> &bucket = Buckets[hash];
    for (const SigId id : bucket) {
        const SigNode &m = Nodes[id];
        if (m.Kind != uint8_t(SigKind::Rec) || m.ChildCount != branches.size()) continue;
        const SigId *kids = ChildPool.data() + m.Children;
        bool same = true;
        for (size_t i = 0; same && i < branches.size(); ++i) {
            const SigId a = branches[i] == reserved ? NoSig : branches[i];
            const SigId b = kids[i] == id ? NoSig : kids[i];
            same = a == b;
        }
        if (same) return id;
    }

    SigNode &n = Nodes[reserved];
    n.Aux = 0;
    n.Children = uint32_t(ChildPool.size());
    n.ChildCount = uint32_t(branches.size());
    ChildPool.insert(ChildPool.end(), branches.begin(), branches.end());
    Hashes[reserved] = hash;
    bucket.push_back(reserved);
    return reserved;
}

namespace {

// A node reaching a back edge Hashes and memoizes per enclosing-group context, anything
// free of every open group in the arena cache.
struct Merkler {
    // `ref` is the shallowest open group reached, `Free` for none, and picks the cache.
    struct Hashed {
        uint64_t H = 0;
        size_t Ref = Free;
    };
    static constexpr size_t Free = size_t(-1);

    const Signals &S;
    const bool Shape;
    std::vector<uint64_t> &Global;
    std::vector<uint8_t> &Have;
    std::vector<SigId> Open; // innermost last
    std::vector<std::unordered_map<SigId, Hashed>> Local;

    uint64_t Bytes(uint64_t h, std::string_view text) const {
        h = Mix(h, text.size());
        for (const char c : text) h = Mix(h, uint8_t(c));
        return h;
    }

    // An already-open group Hashes as how far up it is rather than as itself.
    bool BackEdge(SigId c, Hashed &out) const {
        for (size_t i = Open.size(); i-- > 0;)
            if (Open[i] == c) {
                out = {Mix(SelfMarker, Open.size() - 1 - i), i};
                return true;
            }
        return false;
    }

    bool Look(SigId id, Hashed &out) const {
        if (Have[id]) {
            out = {Global[id], Free};
            return true;
        }
        if (Local.empty()) return false;
        const auto it = Local.back().find(id);
        if (it == Local.back().end()) return false;
        out = it->second;
        return true;
    }

    void Store(SigId id, const Hashed &h) {
        if (h.Ref == Free) {
            Global[id] = h.H;
            Have[id] = 1;
        } else if (!Local.empty()) {
            Local.back()[id] = h;
        }
    }

    Hashed Of(SigId id) {
        Hashed h;
        if (Look(id, h)) return h;
        if (S.KindOf(id) == SigKind::Rec) {
            // Its own pass, with itself open, so the answer ignores the caller.
            const size_t mine = Open.size();
            Open.push_back(id);
            Local.emplace_back();
            h = Fold(id);
            Local.pop_back();
            Open.pop_back();
            if (h.Ref >= mine) h.Ref = Free;
            Store(id, h);
            return h;
        }
        h = Fold(id);
        Store(id, h);
        return h;
    }

    Hashed Fold(SigId id) {
        const SigNode &n = S.Get(id);
        const SigKind k = SigKind(n.Kind);
        uint64_t h = Mix(MerkleSeed, n.Kind);
        h = Mix(h, n.Form);
        if (k == SigKind::Waveform) {
            // `Aux` indexes this arena's waveform table, so hash the samples.
            for (const double v : S.WaveformAt(n.Aux)) h = Mix(h, BitsOf(v));
        } else if (IsLabelled(k) || k == SigKind::FConst || k == SigKind::FVar || k == SigKind::FFun) {
            // The name, not the id: interning order is this arena's. An `FFun`'s `Aux`
            // indexes another arena, a `Soundfile`'s is a channel count.
            h = Bytes(h, S.Str(n.Payload));
            if (k == SigKind::Soundfile) h = Mix(h, n.Aux);
        } else if (!Shape || (k != SigKind::Int && k != SigKind::Real)) {
            // All of the shape normalization. The *kind* stays, so `0.5` and `0.6` share
            // a shape where `0.5` and `1` do not.
            h = Mix(h, n.Payload);
            h = Mix(h, n.Aux);
        }
        size_t ref = Free;
        for (const SigId c : S.Children(id)) {
            Hashed ch;
            if (!BackEdge(c, ch)) ch = Of(c);
            h = Mix(h, ch.H);
            ref = std::min(ref, ch.Ref);
        }
        return {h, ref};
    }
};

} // namespace

uint64_t Signals::ContentHash(SigId id) const {
    if (Content.size() < Nodes.size()) {
        Content.resize(Nodes.size(), 0);
        HasContent.resize(Nodes.size(), 0);
    }
    return Merkler{*this, false, Content, HasContent}.Of(id).H;
}

uint64_t Signals::ShapeHash(SigId id) const {
    if (Shape.size() < Nodes.size()) {
        Shape.resize(Nodes.size(), 0);
        HasShape.resize(Nodes.size(), 0);
    }
    return Merkler{*this, true, Shape, HasShape}.Of(id).H;
}

int Signals::Order(SigId id) const {
    if (Orders.size() < Nodes.size()) Orders.resize(Nodes.size(), -1);
    if (Orders[id] >= 0) return Orders[id];
    Orders[id] = 3; // the answer for a cycle through a recursive group
    int r;
    const SigNode &n = Nodes[id];
    const auto kids = Children(id);
    const auto maxkid = [&](int floor) {
        int m = floor;
        for (const SigId c : kids) m = std::max(m, Order(c));
        return m;
    };
    switch (SigKind(n.Kind)) {
        case SigKind::Int:
        case SigKind::Real: r = 0; break;
        case SigKind::FConst: r = 1; break;
        case SigKind::FVar:
        case SigKind::Button:
        case SigKind::Checkbox:
        case SigKind::VSlider:
        case SigKind::HSlider:
        case SigKind::NumEntry:
        case SigKind::SoundfileLength:
        case SigKind::SoundfileRate: r = 2; break;
        case SigKind::VBargraph:
        case SigKind::HBargraph: r = maxkid(2); break;
        // Flat 3 however built: `rdtable` at a constant index is still audio order.
        case SigKind::Waveform:
        case SigKind::Input:
        case SigKind::Delay1:
        case SigKind::Delay:
        case SigKind::Prefix:
        case SigKind::Rec:
        case SigKind::Proj:
        case SigKind::Select2:
        case SigKind::Gen:
        case SigKind::RDTbl:
        case SigKind::WRTbl:
        case SigKind::SoundfileBuffer: r = 3; break;
        // The attached signal alone. The second argument is kept alive, not used.
        case SigKind::Attach: r = std::max(1, Order(Child(id, 0))); break;
        case SigKind::FFun: r = kids.empty() ? 3 : maxkid(1); break;
        default: r = maxkid(0); break;
    }
    Orders[id] = int8_t(r);
    return r;
}

uint32_t Signals::AddWaveform(std::vector<double> w) {
    Waveforms.push_back(std::move(w));
    return uint32_t(Waveforms.size() - 1);
}

} // namespace faustlens
