#include "runtime/Migrate.h"

#include "runtime/Interp.h"

#include <algorithm>
#include <compare>
#include <map>

namespace faustlens {

namespace {

bool Carried(const Field &f) { return f.Loop == NoLoop && f.Sig != NoSig && (f.Kind == FieldKind::Delay || f.Kind == FieldKind::Perm); }

// The shared `IOTA`. A position, not a value, so it is read but never migrated.
bool IsIota(const Field &f) { return f.Kind == FieldKind::Perm && f.Sig == NoSig && f.Loop == NoLoop; }

int32_t IotaOf(const Plan &p, const Interp &in) {
    for (uint32_t f = 0; f < p.Fields.size(); ++f)
        if (IsIota(p.Fields[f])) return in.FieldState(f)[0].I;
    return 0;
}

// The slot `k` frames back, one formula for both line shapes so a resized delay migrates.
uint32_t Slot(const Field &f, int32_t iota, uint32_t k) {
    if (!f.Ring) return k;
    return uint32_t(iota - int32_t(k)) & (f.Extent - 1);
}

// Copies the common window relative to the write head, so a lengthened delay keeps its history.
bool Copy(const Field &of, std::span<const Scalar> from, int32_t old_iota, const Field &nf, std::span<Scalar> to, int32_t new_iota) {
    const uint32_t n = std::min(of.Extent, nf.Extent);
    for (uint32_t k = 0; k < n; ++k) to[Slot(nf, new_iota, k)] = from[Slot(of, old_iota, k)];
    if (nf.Extent <= of.Extent) return nf.Extent != of.Extent;
    // A rotated ring's untouched slots are not the ones `Clear` zeroed.
    for (uint32_t k = n; k < nf.Extent; ++k) to[Slot(nf, new_iota, k)] = Scalar{};
    return true;
}

// Ties break on offset, not field index, so the result is independent of lowering order.
struct Pair {
    uint32_t Distance = 0;
    uint32_t OldAt = 0, NewAt = 0;
    uint32_t OldField = 0, NewField = 0;

    // Not defaulted: the field indices must stay out of the order.
    std::strong_ordering operator<=>(const Pair &b) const {
        if (const auto c = Distance <=> b.Distance; c != 0) return c;
        if (const auto c = OldAt <=> b.OldAt; c != 0) return c;
        return NewAt <=> b.NewAt;
    }
    bool operator==(const Pair &b) const { return (*this <=> b) == 0; }
};

uint32_t Distance(uint32_t a, uint32_t b) { return a < b ? b - a : a - b; }

} // namespace

Migration
Migrate(const Plan &old_plan, const Interp &from, std::span<const uint32_t> old_at, const Plan &new_plan, Interp &to, std::span<const uint32_t> new_at) {
    Migration m;
    const int32_t old_iota = IotaOf(old_plan, from), new_iota = IotaOf(new_plan, to);

    // Keyed on hash and kind: one node can own both a delay line and a `Perm`.
    std::map<std::pair<uint64_t, FieldKind>, std::vector<uint32_t>> by_hash;
    std::map<std::pair<uint64_t, FieldKind>, std::vector<uint32_t>> by_shape;
    std::vector<uint8_t> old_taken(old_plan.Fields.size(), 0);
    for (uint32_t f = 0; f < old_plan.Fields.size(); ++f) {
        const Field &of = old_plan.Fields[f];
        if (!Carried(of)) continue;
        by_hash[{of.Hash, of.Kind}].push_back(f);
        by_shape[{of.Shape, of.Kind}].push_back(f);
    }

    std::vector<uint32_t> unmatched;
    for (uint32_t f = 0; f < new_plan.Fields.size(); ++f) {
        const Field &nf = new_plan.Fields[f];
        if (!Carried(nf)) continue;
        const auto it = by_hash.find({nf.Hash, nf.Kind});
        uint32_t pick = NoField;
        if (it != by_hash.end())
            for (const uint32_t o : it->second)
                if (!old_taken[o]) {
                    pick = o;
                    break;
                }
        if (pick == NoField) {
            unmatched.push_back(f);
            continue;
        }
        old_taken[pick] = 1;
        ++m.Exact;
        m.Resized += Copy(old_plan.Fields[pick], from.FieldState(pick), old_iota, nf, to.FieldState(f), new_iota);
    }

    std::vector<Pair> pairs;
    for (const uint32_t f : unmatched) {
        const Field &nf = new_plan.Fields[f];
        if (new_at[f] == Nowhere) continue;
        const auto it = by_shape.find({nf.Shape, nf.Kind});
        if (it == by_shape.end()) continue;
        for (const uint32_t o : it->second) {
            if (old_taken[o] || old_at[o] == Nowhere) continue;
            pairs.push_back({Distance(old_at[o], new_at[f]), old_at[o], new_at[f], o, f});
        }
    }
    std::ranges::sort(pairs);
    std::vector<uint8_t> new_taken(new_plan.Fields.size(), 0);
    for (const Pair &p : pairs) {
        if (old_taken[p.OldField] || new_taken[p.NewField]) continue;
        old_taken[p.OldField] = 1;
        new_taken[p.NewField] = 1;
        ++m.Shaped;
        m.Resized += Copy(old_plan.Fields[p.OldField], from.FieldState(p.OldField), old_iota, new_plan.Fields[p.NewField], to.FieldState(p.NewField), new_iota);
    }
    for (const uint32_t f : unmatched)
        if (!new_taken[f]) ++m.Fresh;
    return m;
}

} // namespace faustlens
