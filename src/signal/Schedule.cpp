#include "signal/Schedule.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <format>
#include <optional>
#include <span>

namespace faustlens {

namespace {

constexpr int32_t MaxCopyDelay = 16;

// Floor of 2, not 1.
uint32_t Pow2Limit(int32_t x) {
    uint32_t n = 2;
    while (n < uint32_t(x)) n *= 2;
    return n;
}

// The delay a parent imposes on child `i`. Nothing but a delay imposes any.
std::expected<int32_t, std::string> EdgeDelay(const Signals &s, SigId parent, uint32_t i, std::span<const Interval> iv) {
    switch (s.KindOf(parent)) {
        // Only the delayed operand: the index expression itself is read this frame.
        case SigKind::Delay: {
            if (i != 0) return 0;
            const Interval &n = iv[s.Child(parent, 1)];
            if (n.IsEmpty() || n.Lo < 0 || !(n.Hi < double(INT_MAX)))
                return std::unexpected(std::format("delay index is not in [0, INT_MAX): [{}, {}]", n.Lo, n.Hi));
            return int32_t(std::lround(n.Hi));
        }
        // `prefix(x, y)` delays its second operand, `x` being the value at time 0.
        case SigKind::Delay1: return i == 0 ? 1 : 0;
        case SigKind::Prefix: return i == 1 ? 1 : 0;
        default: return 0;
    }
}

} // namespace

std::vector<Band> AssignBands(std::span<const Variability> var) {
    std::vector<Band> out(var.size(), Band::Init);
    for (size_t i = 0; i < var.size(); ++i) out[i] = var[i] == Variability::Konst ? Band::Init : var[i] == Variability::Block ? Band::Control : Band::Sample;
    return out;
}

std::expected<std::vector<int32_t>, std::string> MaxDelays(const Signals &s, std::span<const Interval> iv, std::span<const SigId> roots) {
    std::vector<int32_t> out(s.Size(), 0);
    // Per edge and not accumulated, so the order the walk meets a node in does not matter.
    // The walk cannot return early, so the first reason is carried out to the caller.
    std::optional<std::string> why;
    Reachable(s, roots, [&](SigId id) {
        if (why || s.KindOf(id) == SigKind::Gen) return false;
        const std::span<const SigId> kids = s.Children(id);
        for (uint32_t i = 0; i < kids.size(); ++i) {
            const auto d = EdgeDelay(s, id, i, iv);
            if (!d) {
                why = d.error();
                return false;
            }
            out[kids[i]] = std::max(*d, out[kids[i]]);
        }
        return true;
    });
    if (why) return std::unexpected(*std::move(why));
    return out;
}

std::vector<DelayLine> DelayLines(const Signals &s, std::span<const int32_t> max_delay, std::span<const Nature> nat, std::span<const SigId> roots) {
    std::vector<DelayLine> out;
    std::vector<SigId> live;
    Reachable(s, roots, [&](SigId id) {
        live.push_back(id);
        return s.KindOf(id) != SigKind::Gen;
    });
    std::ranges::sort(live);

    for (const SigId id : live) {
        const DelayLine l = LineFor(s, id, max_delay[id], nat[id]);
        if (l.Extent > 0) out.push_back(l);
    }
    return out;
}

DelayLine LineFor(const Signals &s, SigId id, int32_t max_delay, Nature nature) {
    DelayLine l;
    if (max_delay <= 0) return l;
    // A `prefix` already holds one sample on its state field, so it needs no array.
    if (s.KindOf(id) == SigKind::Prefix && max_delay == 1) return l;
    l.Sig = id;
    l.Nature = nature;
    l.MaxDelay = max_delay;
    l.Ring = max_delay >= MaxCopyDelay;
    l.Extent = l.Ring ? Pow2Limit(max_delay + 1) : uint32_t(max_delay + 1);
    return l;
}

} // namespace faustlens
