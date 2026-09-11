#include "signal/Differentiate.h"

#include "signal/Promote.h"
#include "signal/Propagate.h"
#include "signal/Simplify.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace faustlens {
namespace {

bool Continuous(SigKind k) { return k == SigKind::HSlider || k == SigKind::VSlider || k == SigKind::NumEntry; }
bool StructuralHandle(SigKind k) { return k == SigKind::Rec || k == SigKind::WRTbl || k == SigKind::Gen; }
bool Rounded(Ext e) { return e == Ext::Ceil || e == Ext::Floor || e == Ext::Rint || e == Ext::Round; }
double Coefficient(const DifferentiateRequest &request, size_t control, size_t direction) {
    return request.ExplicitDirections ? request.Directions[control * request.DirectionCount + direction] : double(control == direction);
}

// Projections depend on their own branch. Propagate through feedback to a fixed point.
std::vector<uint8_t> Dependence(const std::vector<std::vector<SigId>> &parents, std::span<const SigId> seeds) {
    std::vector<uint8_t> result(parents.size(), 0);
    std::vector<SigId> queue;
    for (const SigId s : seeds) {
        result[s] = 1;
        queue.push_back(s);
    }
    for (size_t at = 0; at < queue.size(); ++at)
        for (const SigId p : parents[queue[at]])
            if (!result[p]) {
                result[p] = 1;
                queue.push_back(p);
            }
    return result;
}

struct ConstructionError {
    const char *Reason;
};

struct Construction {
    Signals &S;
    const std::vector<Nature> &NatureOf;
    const std::vector<uint8_t> &Active;
    const std::vector<int32_t> &Seed;
    const std::vector<std::vector<SigId>> &Projections;
    const DifferentiateRequest &Request;
    const size_t Direction;
    const size_t OriginalSize = NatureOf.size();
    std::vector<SigId> Memo = std::vector<SigId>(OriginalSize, NoSig);
    SigId Zero = S.MakeReal(0);

    void CheckBudget() const {
        if (S.Size() - OriginalSize > Request.MaxNewNodes) throw ConstructionError{"tangent construction exceeds request node budget"};
    }
    SigId Real(SigId id) { return S.KindOf(id) == SigKind::Int || (id < OriginalSize && NatureOf[id] == Nature::Int) ? SimpFloatCast(S, id) : id; }
    SigId Num(double x) { return S.MakeReal(x); }
    SigId Bin(BinOpCode op, SigId a, SigId b) { return SimpBinOp(S, op, Real(a), Real(b)); }
    SigId Add(SigId a, SigId b) { return Bin(BinOpCode::Add, a, b); }
    SigId Sub(SigId a, SigId b) { return IsZeroNum(S, a) ? Mul(Num(-1), b) : Bin(BinOpCode::Sub, a, b); }
    SigId Mul(SigId a, SigId b) { return Bin(BinOpCode::Mul, a, b); }
    SigId Div(SigId a, SigId b) { return Bin(BinOpCode::Div, a, b); }
    SigId Neg(SigId a) { return Sub(Zero, a); }
    SigId Ext1(Ext e, SigId a) { return SimpExtended(S, e, {Real(a)}); }
    SigId Ext2(Ext e, SigId a, SigId b) { return SimpExtended(S, e, {Real(a), Real(b)}); }
    SigId Square(SigId a) { return Mul(a, a); }
    SigId Select(SigId c, SigId a, SigId b) { return SimpSelect2(S, c, a, b); }
    // Preserve the execution band when the tangent becomes a literal.
    // Equal arms preserve signed zero and nonfinite values.
    SigId PreserveRate(SigId tangent, SigId primal) {
        const SigId selector = S.MakeBin(BinOpCode::EQ, primal, primal);
        return S.Make(SigKind::Select2, {selector, tangent, tangent});
    }
    bool PrefixValue(SigId id) const {
        while (S.KindOf(id) == SigKind::Attach || S.KindOf(id) == SigKind::Control) id = S.Child(id, 0);
        return S.KindOf(id) == SigKind::Prefix;
    }
    SigId DelayInput(SigId primal) {
        const SigId tangent = Go(primal);
        // Delayed tangent projections already have their own producer identity and clock history.
        return S.KindOf(tangent) == SigKind::Proj && tangent >= OriginalSize ? tangent : PreserveRate(tangent, primal);
    }
    SigId Trunc(SigId a) { return Select(Bin(BinOpCode::LT, a, Zero), Ext1(Ext::Floor, a), Ext1(Ext::Ceil, a)); }

    SigId Extended(SigId id, Ext e, const std::vector<SigId> &k) {
        const SigId a = k[0], da = Go(a);
        const SigId one = Num(1), two = Num(2);
        switch (e) {
            case Ext::Abs: return Select(Bin(BinOpCode::LT, a, Zero), da, Neg(da));
            case Ext::Acos: return Neg(Div(da, Ext1(Ext::Sqrt, Sub(one, Square(a)))));
            case Ext::Acosh: return Div(Div(da, Ext1(Ext::Sqrt, Sub(a, one))), Ext1(Ext::Sqrt, Add(a, one)));
            case Ext::Asin: return Div(da, Ext1(Ext::Sqrt, Sub(one, Square(a))));
            case Ext::Asinh:
            case Ext::Atan: {
                // Scale before squaring to avoid overflow when the derivative is finite.
                const SigId scale = Ext2(Ext::Max, one, Ext1(Ext::Abs, a));
                const SigId unitA = Div(a, scale), unitOne = Div(one, scale);
                const SigId normSquared = Add(Square(unitA), Square(unitOne));
                const SigId scaledTangent = Div(da, scale);
                return e == Ext::Asinh ? Div(scaledTangent, Ext1(Ext::Sqrt, normSquared)) : Div(Div(scaledTangent, scale), normSquared);
            }
            case Ext::Atanh: return Div(da, Sub(one, Square(a)));
            case Ext::Cos: return Neg(Mul(da, Ext1(Ext::Sin, a)));
            case Ext::Cosh: return Mul(da, Ext1(Ext::Sinh, a));
            case Ext::Exp: return Mul(da, id);
            case Ext::Log: return Div(da, a);
            case Ext::Log10: return Div(Div(da, a), Num(std::log(10.0)));
            case Ext::Sin: return Mul(da, Ext1(Ext::Cos, a));
            case Ext::Sinh: return Mul(da, Ext1(Ext::Cosh, a));
            case Ext::Sqrt: return Div(da, Mul(two, id));
            case Ext::Tan: return Mul(da, Add(one, Square(id)));
            case Ext::Tanh: {
                // Scaled sech avoids cancellation when tanh(a) rounds to +/-1 and overflow in cosh(a)^2.
                const SigId decay = Ext1(Ext::Exp, Neg(Ext1(Ext::Abs, a)));
                const SigId sech = Div(Mul(two, decay), Add(one, Square(decay)));
                return Mul(Mul(da, sech), sech);
            }
            case Ext::AssertBounds: return Go(k[2]);
            case Ext::Ceil:
            case Ext::Floor:
            case Ext::Rint:
            case Ext::Round:
            case Ext::Lowest:
            case Ext::Highest: return Zero;
            default: break;
        }
        const SigId b = k[1], db = Go(b);
        switch (e) {
            case Ext::Atan2: {
                const SigId scale = Ext2(Ext::Max, Ext1(Ext::Abs, a), Ext1(Ext::Abs, b));
                const SigId unitA = Div(a, scale), unitB = Div(b, scale);
                SigId numerator = Zero;
                if (!IsZeroNum(S, da)) numerator = Mul(Div(da, scale), unitB);
                if (!IsZeroNum(S, db)) numerator = Sub(numerator, Mul(Div(db, scale), unitA));
                // The origin remains singular.
                return Div(numerator, Add(Square(unitA), Square(unitB)));
            }
            case Ext::Max: return Select(Bin(BinOpCode::LT, a, b), da, db);
            case Ext::Min: return Select(Bin(BinOpCode::LT, a, b), db, da);
            case Ext::Fmod: return IsZeroNum(S, db) ? da : Sub(da, Mul(Trunc(Div(a, b)), db));
            case Ext::Remainder: return IsZeroNum(S, db) ? da : Sub(da, Mul(Ext1(Ext::Rint, Div(a, b)), db));
            case Ext::Pow: {
                if (IsNum(S, b)) {
                    const double c = NumOf(S, b);
                    if (c == 0) return Zero;
                    if (c == 1) return da;
                    return Mul(Mul(Num(c), Ext2(Ext::Pow, a, Num(c - 1))), da);
                }
                SigId result = Zero;
                if (!IsZeroNum(S, da)) result = Mul(Mul(b, Ext2(Ext::Pow, a, Sub(b, one))), da);
                if (!IsZeroNum(S, db)) result = Add(result, Mul(Mul(id, Ext1(Ext::Log, a)), db));
                return result;
            }
            default: return S.Error;
        }
    }

    SigId Group(SigId id) {
        CheckBudget();
        if (Memo[id] != NoSig) return Memo[id];
        const std::vector<SigId> branches(S.Children(id).begin(), S.Children(id).end());
        const SigId reserved = S.OpenRec();
        Memo[id] = reserved;
        for (const SigId p : Projections[id]) {
            const uint32_t branch = S.Get(p).Payload;
            Memo[p] = Active[p] ? S.Make(SigKind::Proj, 0, branch, 0, {reserved}) : Zero;
        }
        std::vector<SigId> tangent;
        tangent.reserve(branches.size());
        for (const SigId b : branches) tangent.push_back(Go(b));
        const SigId closed = S.CloseRec(reserved, tangent);
        Memo[id] = closed;
        if (closed != reserved)
            for (const SigId p : Projections[id])
                if (Active[p]) Memo[p] = S.Make(SigKind::Proj, 0, S.Get(p).Payload, 0, {closed});
        CheckBudget();
        return closed;
    }

    SigId Go(SigId id) {
        CheckBudget();
        if (!Active[id]) return Zero;
        if (Memo[id] != NoSig) return Memo[id];
        const SigNode node = S.Get(id);
        const SigKind kind = S.KindOf(id);
        const std::vector<SigId> k(S.Children(id).begin(), S.Children(id).end());
        struct OriginScope {
            Signals &S;
            ValueId Saved;
            ~OriginScope() { S.OriginNow = Saved; }
        } origin{S, S.OriginNow};
        S.OriginNow = S.OriginOf(id);
        SigId out = Zero;
        if (Seed[id] >= 0) {
            out = Num(Coefficient(Request, size_t(Seed[id]), Direction));
        } else if (kind == SigKind::Rec) {
            return Group(id);
        } else if (kind == SigKind::Proj) {
            Group(k[0]);
            return Memo[id];
        } else {
            switch (kind) {
                case SigKind::BinOp: {
                    const SigId a = k[0], b = k[1], da = Go(a), db = Go(b);
                    switch (BinOpCode(node.Form)) {
                        case BinOpCode::Add: out = Add(da, db); break;
                        case BinOpCode::Sub: out = Sub(da, db); break;
                        case BinOpCode::Mul: out = Add(Mul(da, b), Mul(a, db)); break;
                        case BinOpCode::Div: out = Div(Sub(da, Mul(id, db)), b); break;
                        case BinOpCode::Rem: out = IsZeroNum(S, db) ? da : Sub(da, Mul(Trunc(Div(a, b)), db)); break;
                        default: break;
                    }
                    break;
                }
                case SigKind::FloatCast: out = Go(k[0]); break;
                case SigKind::Delay1: out = SimpDelay1(S, DelayInput(k[0])); break;
                case SigKind::Delay: out = SimpDelay(S, DelayInput(k[0]), k[1]); break;
                case SigKind::Prefix: out = S.Make(SigKind::Prefix, 0, id, 1, {Go(k[0]), Go(k[1])}); break;
                case SigKind::Select2: out = Select(k[0], Go(k[1]), Go(k[2])); break;
                case SigKind::Select3: out = S.Make(SigKind::Select3, {k[0], Go(k[1]), Go(k[2]), Go(k[3])}); break;
                case SigKind::VBargraph:
                case SigKind::HBargraph: out = Go(k[2]); break;
                case SigKind::Attach: out = S.Make(SigKind::Attach, {Go(k[0]), k[1]}); break;
                case SigKind::Control: {
                    const SigId tangent = Go(k[0]);
                    // Prefix guards updates but loads unconditionally. A guarded cache
                    // would delay the visible state by one enabled sample at clock transitions.
                    out = SimpControl(S, PrefixValue(k[0]) && PrefixValue(tangent) ? tangent : PreserveRate(tangent, k[0]), k[1]);
                    break;
                }
                case SigKind::Enable: out = S.Make(SigKind::Enable, {Go(k[0]), k[1]}); break;
                case SigKind::WRTbl: {
                    // Seed-independent initialization gives the tangent table zero initial values.
                    const SigId gen = S.Make(SigKind::Gen, {Zero});
                    // Preserve distinct state histories when initializers share a zero tangent.
                    if (k.size() == 4) out = S.Make(SigKind::WRTbl, 0, id, 1, {k[0], gen, k[2], PreserveRate(Go(k[3]), k[3])});
                    else out = S.Make(SigKind::WRTbl, 0, id, 1, {k[0], gen});
                    break;
                }
                case SigKind::RDTbl: out = S.Make(SigKind::RDTbl, {Go(k[0]), k[1]}); break;
                case SigKind::Gen: out = S.Make(SigKind::Gen, {Go(k[0])}); break;
                case SigKind::Extended: out = Extended(id, Ext(node.Form), k); break;
                default: break;
            }
        }
        CheckBudget();
        Memo[id] = out;
        return out;
    }
};

} // namespace

DifferentiateResult Differentiate(Signals &s, std::span<const SigId> roots, const DifferentiateRequest &request, std::span<const UiItem> ui) {
    DifferentiateResult result;
    const auto fail = [&](std::string error) {
        result.Error = std::move(error);
        return std::move(result);
    };
    const size_t n = s.Size(), p = request.Controls.size();
    const size_t d = request.ExplicitDirections ? request.DirectionCount : p;
    result.DirectionCount = d;
    if (d > request.MaxDirections) return fail("direction count exceeds request budget");
    if (p && d > std::numeric_limits<size_t>::max() / p) return fail("direction matrix dimensions overflow");
    if ((request.ExplicitDirections && request.Directions.size() != p * d) ||
        (!request.ExplicitDirections && (!request.Directions.empty() || request.DirectionCount != 0)))
        return fail("invalid direction matrix dimensions or selected-column request");
    if (d && roots.size() > std::numeric_limits<size_t>::max() / d) return fail("tangent output dimensions overflow");
    for (const double coefficient : request.Directions)
        if (!std::isfinite(coefficient)) return fail("direction coefficients must be finite");
    std::unordered_map<std::string_view, size_t> selected;
    for (size_t j = 0; j < p; ++j) {
        if (!selected.emplace(request.Controls[j], j).second) return fail("duplicate selected control: " + request.Controls[j]);
        result.Controls.push_back({request.Controls[j]});
    }
    for (const SigId root : roots)
        if (root >= n) return fail("invalid primal signal root");
    // Return before interning literals so zero directions leave the arena unchanged.
    if (d == 0) return result;

    // Estimate vector capacities for reverse edges, activity, construction, and traversal.
    const size_t edges = s.ChildPool.size();
    const size_t perNode = 4 * sizeof(std::vector<SigId>) + 12 * sizeof(SigId) + 16;
    if (p > request.MaxWorkspaceBytes || n > request.MaxWorkspaceBytes / (perNode + p) ||
        edges > (request.MaxWorkspaceBytes - n * (perNode + p)) / (4 * sizeof(SigId)))
        return fail("estimated derivative workspace exceeds request budget");
    result.EstimatedWorkspaceBytes = n * (perNode + p) + edges * 4 * sizeof(SigId);
    if (roots.size() * d > (request.MaxWorkspaceBytes - result.EstimatedWorkspaceBytes) / sizeof(SigId))
        return fail("tangent output storage exceeds request workspace budget");
    result.EstimatedWorkspaceBytes += roots.size() * d * sizeof(SigId);
    // Allow up to two diagnostics per node/control, capped to avoid dense string storage.
    const size_t diagnosticCount = p && n > request.MaxDiagnostics / p / 2 ? request.MaxDiagnostics : std::min(request.MaxDiagnostics, n * p * 2);
    constexpr size_t diagnosticBytes = 2 * sizeof(DifferentiationDiagnostic) + 128;
    if (diagnosticCount > (request.MaxWorkspaceBytes - result.EstimatedWorkspaceBytes) / diagnosticBytes)
        return fail("derivative diagnostics exceed request workspace budget");
    result.EstimatedWorkspaceBytes += diagnosticCount * diagnosticBytes;
    if (n > request.MaxAnalysisVisits || edges > request.MaxAnalysisVisits - n || (p && n + edges > request.MaxAnalysisVisits / p / 4))
        return fail("derivative dependency analysis exceeds request work budget");
    std::vector<uint8_t> reachable(n, 0);
    std::vector<SigId> pending(roots.begin(), roots.end());
    while (!pending.empty()) {
        const SigId id = pending.back();
        pending.pop_back();
        if (reachable[id]) continue;
        reachable[id] = 1;
        if (s.KindOf(id) == SigKind::Proj) {
            const SigId group = s.Child(id, 0);
            if (s.KindOf(group) != SigKind::Rec || s.Get(id).Payload >= s.Get(group).ChildCount) return fail("invalid recursive projection");
            reachable[group] = 1;
            pending.push_back(s.Child(group, s.Get(id).Payload));
        } else {
            for (const SigId child : s.Children(id)) pending.push_back(child);
        }
    }
    const std::vector<Nature> nature = InferNatures(s);
    std::vector<int32_t> seed(n, -1);
    std::vector<std::vector<SigId>> seedNodes(p), structuralParents(n), activeParents(n);
    std::vector<const UiItem *> descriptions(p, nullptr);
    for (const UiItem &item : ui) {
        const auto found = selected.find(s.Str(item.Label));
        if (found == selected.end()) continue;
        const size_t j = found->second;
        if (item.Kind == UiKind::HBargraph || item.Kind == UiKind::VBargraph) continue;
        result.Controls[j].Present = true;
        if (item.Kind != UiKind::HSlider && item.Kind != UiKind::VSlider && item.Kind != UiKind::NumEntry)
            return fail("selected control is not continuous: " + request.Controls[j]);
        if (const UiItem *old = descriptions[j]; old && (old->Init != item.Init || old->Min != item.Min || old->Max != item.Max || old->Step != item.Step))
            return fail("inconsistent descriptions for selected control: " + request.Controls[j]);
        descriptions[j] = &item;
    }
    std::vector<SigId> previousWidget(p, NoSig);
    for (SigId id = 0; id < n; ++id) {
        if (!reachable[id]) continue;
        const SigKind kind = s.KindOf(id);
        const SigNode node = s.Get(id);
        const auto k = s.Children(id);
        if (kind == SigKind::Error) return fail("primal graph contains an error signal");
        if (Continuous(kind) || kind == SigKind::Button || kind == SigKind::Checkbox) {
            const auto found = selected.find(s.Str(node.Payload));
            if (found == selected.end()) continue;
            const size_t j = found->second;
            if (!Continuous(kind)) return fail("selected control is not continuous: " + request.Controls[j]);
            result.Controls[j].Present = true;
            if (previousWidget[j] != NoSig && !std::ranges::equal(s.Children(previousWidget[j]), k))
                return fail("inconsistent signal widgets for selected control: " + request.Controls[j]);
            previousWidget[j] = id;
            seed[id] = int32_t(j);
            seedNodes[j].push_back(id);
            continue;
        }
        const auto edge = [&](SigId child, bool active) {
            structuralParents[child].push_back(id);
            if (active) activeParents[child].push_back(id);
        };
        if (kind == SigKind::Proj) {
            const SigId group = k[0];
            edge(s.Child(group, node.Payload), nature[id] == Nature::Real);
            continue;
        }
        const bool real = nature[id] == Nature::Real || StructuralHandle(kind);
        for (size_t i = 0; i < k.size(); ++i) {
            bool active = real;
            switch (kind) {
                case SigKind::IntCast:
                case SigKind::BitCast:
                case SigKind::SoundfileLength:
                case SigKind::SoundfileRate:
                case SigKind::SoundfileBuffer: active = false; break;
                case SigKind::Select2:
                case SigKind::Select3: active &= i != 0; break;
                case SigKind::Delay:
                case SigKind::Attach:
                case SigKind::Control:
                case SigKind::Enable:
                case SigKind::RDTbl: active &= i == 0; break;
                case SigKind::WRTbl: active &= i == 1 || i == 3; break;
                case SigKind::VBargraph:
                case SigKind::HBargraph: active &= i == 2; break;
                case SigKind::Extended: {
                    const Ext ext = Ext(node.Form);
                    if (Rounded(ext) || ext == Ext::Lowest || ext == Ext::Highest) active = false;
                    if (ext == Ext::AssertBounds) active &= i == 2;
                    break;
                }
                default: break;
            }
            edge(k[i], active);
        }
    }
    std::vector<std::vector<uint8_t>> activity;
    activity.reserve(p);
    for (size_t j = 0; j < p; ++j) {
        const auto structural = Dependence(structuralParents, seedNodes[j]);
        activity.push_back(Dependence(activeParents, seedNodes[j]));
        const auto &active = activity.back();
        const auto report = [&](DerivativeDiagnosticKind kind, SigId id, const char *why) {
            if (result.Diagnostics.size() >= request.MaxDiagnostics) {
                result.Error = "derivative diagnostics exceed request count budget";
                return;
            }
            result.Diagnostics.push_back({kind, j, id, id == NoSig ? NoTerm : s.OriginOf(id), why});
            if (kind == DerivativeDiagnosticKind::Unsupported && result.Error.empty()) result.Error = request.Controls[j] + ": " + why;
        };
        if (seedNodes[j].empty()) {
            report(
                DerivativeDiagnosticKind::Absent, NoSig,
                result.Controls[j].Present ? "control was eliminated from normalized outputs" : "control label is absent"
            );
            if (!result.Controls[j].Present && result.Error.empty()) result.Error = "selected control label is absent: " + request.Controls[j];
        }
        for (const SigId root : roots) {
            result.Controls[j].Structural |= structural[root];
            result.Controls[j].Active |= active[root];
        }
        for (SigId id = 0; id < n; ++id) {
            if (!reachable[id] || !structural[id] || seed[id] >= 0) continue;
            const SigKind kind = s.KindOf(id);
            const auto k = s.Children(id);
            const auto depends = [&](size_t i) { return i < k.size() && structural[k[i]]; };
            const auto moving = [&](size_t i) { return i < k.size() && active[k[i]]; };
            if (kind == SigKind::FFun && std::any_of(k.begin(), k.end(), [&](SigId c) { return active[c]; }))
                report(DerivativeDiagnosticKind::Unsupported, id, "active foreign function requires a derivative rule");
            else if (kind == SigKind::Enable)
                report(DerivativeDiagnosticKind::Unsupported, id, "raw Enable must be normalized to Control before differentiation");
            else if (kind == SigKind::BitCast) report(DerivativeDiagnosticKind::Unsupported, id, "active bit reinterpretation is unsupported");
            else if (kind == SigKind::Gen || (kind == SigKind::Prefix && depends(0)))
                report(DerivativeDiagnosticKind::Unsupported, id, "seed-dependent initialization is unsupported");
            else if ((kind == SigKind::Select2 || kind == SigKind::Select3) && depends(0))
                report(DerivativeDiagnosticKind::Barrier, id, "selector changes use the executed-branch convention");
            else if (kind == SigKind::Delay && depends(1)) report(DerivativeDiagnosticKind::Barrier, id, "integer delay amount has zero local derivative");
            else if (kind == SigKind::RDTbl && depends(1)) report(DerivativeDiagnosticKind::Barrier, id, "table read index has zero local derivative");
            else if (kind == SigKind::WRTbl && (depends(0) || depends(2)))
                report(DerivativeDiagnosticKind::Barrier, id, "table size or write index has zero local derivative");
            else if ((kind == SigKind::Control || kind == SigKind::Enable) && depends(1))
                report(DerivativeDiagnosticKind::Barrier, id, "clock changes use the executed-clock convention");
            else if (kind == SigKind::SoundfileBuffer || kind == SigKind::SoundfileLength || kind == SigKind::SoundfileRate)
                report(DerivativeDiagnosticKind::Barrier, id, "soundfile addressing has zero local derivative");
            else if (kind == SigKind::IntCast || (!StructuralHandle(kind) && nature[id] == Nature::Int && kind != SigKind::Proj && kind != SigKind::FloatCast))
                report(DerivativeDiagnosticKind::Barrier, id, "integer-valued operation has zero local derivative");
            else if (kind == SigKind::Extended) {
                const Ext ext = Ext(s.Get(id).Form);
                if (Rounded(ext)) report(DerivativeDiagnosticKind::Barrier, id, "rounding has zero local derivative away from jumps");
                if (ext == Ext::Abs || ext == Ext::Min || ext == Ext::Max || ext == Ext::Fmod || ext == Ext::Remainder)
                    report(DerivativeDiagnosticKind::Barrier, id, "piecewise derivative uses the documented branch convention");
                if (ext == Ext::Pow && moving(1) && IsNum(s, k[0]) && NumOf(s, k[0]) <= 0)
                    report(DerivativeDiagnosticKind::Unsupported, id, "active exponent at a nonpositive base has no supported real derivative");
            }
        }
    }
    if (!result.Ok()) return result;
    // Share original projection adjacency across directions.
    std::vector<std::vector<SigId>> projections(n);
    for (SigId id = 0; id < n; ++id)
        if (s.KindOf(id) == SigKind::Proj) projections[s.Child(id, 0)].push_back(id);
    result.Tangents.resize(roots.size() * d);
    try {
        for (size_t direction = 0; direction < d; ++direction) {
            std::vector<uint8_t> active(n, 0);
            for (size_t j = 0; j < p; ++j) {
                if (Coefficient(request, j, direction) != 0)
                    for (size_t id = 0; id < n; ++id) active[id] |= activity[j][id];
            }
            Construction construction{s, nature, active, seed, projections, request, direction};
            for (size_t output = 0; output < roots.size(); ++output) {
                const SigId tangent = construction.Go(roots[output]);
                if (s.IsError(tangent)) throw ConstructionError{"failed to construct tangent signal"};
                result.Tangents[output * d + direction] = tangent;
            }
        }
    } catch (const ConstructionError &error) {
        result.Error = error.Reason;
        result.Tangents.clear();
    }
    result.AllocatedNodes = s.Size() - n;
    return result;
}

} // namespace faustlens
