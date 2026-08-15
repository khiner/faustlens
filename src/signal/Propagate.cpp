#include "signal/Propagate.h"

#include "signal/Simplify.h"

#include <format>
#include <utility>

namespace faustlens {
namespace {

constexpr std::pair<Prim, BinOpCode> BinOps[] = {
    {Prim::Add, BinOpCode::Add}, {Prim::Sub, BinOpCode::Sub},       {Prim::Mul, BinOpCode::Mul},        {Prim::Div, BinOpCode::Div},
    {Prim::Mod, BinOpCode::Rem}, {Prim::Lsh, BinOpCode::LeftShift}, {Prim::Rsh, BinOpCode::RightShift}, {Prim::And, BinOpCode::AND},
    {Prim::Or, BinOpCode::OR},   {Prim::Xor, BinOpCode::XOR},       {Prim::Lt, BinOpCode::LT},          {Prim::Le, BinOpCode::LE},
    {Prim::Gt, BinOpCode::GT},   {Prim::Ge, BinOpCode::GE},         {Prim::Eq, BinOpCode::EQ},          {Prim::Ne, BinOpCode::NE},
};

BinOpCode BinOpFor(Prim p) {
    for (const auto &[q, b] : BinOps)
        if (q == p) return b;
    return BinOpCode::Count_;
}

constexpr std::pair<Prim, Ext> Exts[] = {
    {Prim::Abs, Ext::Abs},
    {Prim::Acos, Ext::Acos},
    {Prim::Asin, Ext::Asin},
    {Prim::Atan, Ext::Atan},
    {Prim::Atan2, Ext::Atan2},
    {Prim::Cos, Ext::Cos},
    {Prim::Sin, Ext::Sin},
    {Prim::Tan, Ext::Tan},
    {Prim::Exp, Ext::Exp},
    {Prim::Log, Ext::Log},
    {Prim::Log10, Ext::Log10},
    {Prim::Pow, Ext::Pow},
    {Prim::Sqrt, Ext::Sqrt},
    {Prim::Min, Ext::Min},
    {Prim::Max, Ext::Max},
    {Prim::Fmod, Ext::Fmod},
    {Prim::Remainder, Ext::Remainder},
    {Prim::Floor, Ext::Floor},
    {Prim::Ceil, Ext::Ceil},
    {Prim::Rint, Ext::Rint},
    {Prim::Round, Ext::Round},
    {Prim::AssertBounds, Ext::AssertBounds},
    {Prim::Lowest, Ext::Lowest},
    {Prim::Highest, Ext::Highest},
};

Ext ExtFor(Prim p) {
    for (const auto &[q, e] : Exts)
        if (q == p) return e;
    return Ext::Count_;
}

} // namespace

size_t Propagator::MemoHash::operator()(const MemoKey &k) const {
    uint64_t h = Mix(0x9e3779b9ull, k.Box);
    h = Mix(h, k.Slots);
    h = Mix(h, k.Path);
    for (const SigId s : k.In) h = Mix(h, s);
    return size_t(h);
}

namespace {

template<typename T> uint32_t InternInto(std::vector<T> &table, const T &v) {
    for (size_t i = 0; i < table.size(); ++i)
        if (table[i] == v) return uint32_t(i);
    table.push_back(v);
    return uint32_t(table.size() - 1);
}

} // namespace

uint32_t Propagator::PathId() { return InternInto(PathTable, Groups); }
uint32_t Propagator::SlotEnvId() { return InternInto(SlotTable, Slots); }

// Appends the widget's label to the enclosing groups, `../` popping one and `/` all.
// The interned text is innermost first, the `UiItem`'s path the reverse.
uint32_t Propagator::Widget(StrId label, UiKind kind, const Bounds *b) {
    UiItem w;
    w.Path = Groups; // outermost first
    for (const PathSeg &seg : LabelToPath(Terms.Str(label))) {
        if (seg.Root()) w.Path.clear();
        else if (seg.Parent()) {
            if (!w.Path.empty()) w.Path.pop_back();
        } else w.Path.push_back(seg);
    }
    w.Kind = kind;
    if (b != nullptr) {
        w.Init = b->Init;
        w.Min = b->Min;
        w.Max = b->Max;
        w.Step = b->Step;
    }
    std::vector<PathSeg> const printed(w.Path.rbegin(), w.Path.rend());
    w.Label = Sigs.InternStr(PathText(printed));
    Ui.push_back(std::move(w));
    return Ui.back().Label;
}

std::vector<SigId> Propagator::Fail(BoxId b, std::string_view why) {
    Diagnostic d;
    d.Severity = Severity::Error;
    d.Code = Code::PropUnsupported;
    d.Payload = std::format("{}: {}", BoxKindName(Boxes.KindOf(b)), why);
    Diags.push_back(std::move(d));
    return {Sigs.Error};
}

SigId Propagator::Bin(BinOpCode b, SigId x, SigId y) {
    if (IsNum(Sigs, x) && IsNum(Sigs, y)) return SimpBinOp(Sigs, b, x, y);
    return Sigs.MakeBin(b, x, y);
}

SigId Propagator::Delay(SigId x, SigId d) {
    if (IsNum(Sigs, x) && IsNum(Sigs, d)) return SimpDelay(Sigs, x, d);
    return Sigs.Make(SigKind::Delay, {x, d});
}

SigId Propagator::Delay1(SigId x) {
    if (IsNum(Sigs, x)) return SimpDelay1(Sigs, x);
    return Sigs.Make(SigKind::Delay1, {x});
}

SigId Propagator::Select2(SigId sel, SigId a, SigId b) { return Sigs.Make(SigKind::Select2, {sel, a, b}); }

std::vector<SigId> Propagator::Run(BoxId box, int32_t inputs) {
    std::vector<SigId> in;
    in.reserve(inputs);
    for (int32_t i = 0; i < inputs; ++i) in.push_back(Sigs.MakeLeaf(SigKind::Input, uint32_t(i)));
    return Propagate(box, std::move(in));
}

std::vector<SigId> Propagator::Propagate(BoxId box, std::vector<SigId> in) {
    MemoKey key{box, SlotEnvId(), PathId(), in};
    if (const auto it = Memo.find(key); it != Memo.end()) return it->second;
    const ValueId saved = Sigs.OriginNow;
    Sigs.OriginNow = Boxes.OriginOf(box);
    std::vector<SigId> out = Real(box, in);
    Sigs.OriginNow = saved;
    Memo.emplace(std::move(key), out);
    return out;
}

std::vector<SigId> Propagator::Real(BoxId box, std::vector<SigId> &in) {
    const BoxNode &n = Boxes.Get(box);
    const auto kids = Boxes.Children(box);
    const auto one = [&](SigId s) { return std::vector<SigId>{s}; };

    switch (Boxes.KindOf(box)) {
        case BoxKind::Int: return one(Sigs.MakeInt(Boxes.IntValue(box)));
        case BoxKind::Real: return one(Sigs.MakeReal(Boxes.RealValue(box)));

        // Two outputs: the size, and a signal cycling the contents.
        case BoxKind::Waveform: {
            const std::vector<double> &w = Boxes.WaveformAt(n.Aux);
            return {Sigs.MakeInt(int32_t(w.size())), Sigs.Make(SigKind::Waveform, n.Form, 0, Sigs.AddWaveform(w), {})};
        }

        // The declared type rides on `Form`, which `MakeLeaf` fixes at 0.
        case BoxKind::FConst: return one(Sigs.Make(SigKind::FConst, n.Form, Sigs.InternStr(Terms.Str(n.Payload)), 0, {}));
        case BoxKind::FVar: return one(Sigs.Make(SigKind::FVar, n.Form, Sigs.InternStr(Terms.Str(n.Payload)), 0, {}));

        case BoxKind::Cut: return {};
        case BoxKind::Wire: return in;

        case BoxKind::Slot: {
            for (size_t i = Slots.size(); i-- > 0;)
                if (Slots[i].first == box) return one(Slots[i].second);
            return Fail(box, "unbound slot");
        }

        case BoxKind::Symbolic: {
            Slots.emplace_back(kids[0], in[0]);
            std::vector<SigId> rest(in.begin() + 1, in.end());
            std::vector<SigId> out = Propagate(kids[1], std::move(rest));
            Slots.pop_back();
            return out;
        }

        case BoxKind::Prim: {
            const Prim p = Prim(n.Payload);
            if (p == Prim::Mem) return one(Delay1(in[0]));
            if (p == Prim::FDelay) return one(Delay(in[0], in[1]));
            if (p == Prim::Prefix) return one(Sigs.Make(SigKind::Prefix, {in[0], in[1]}));
            if (p == Prim::IntCast) return one(SimpIntCast(Sigs, in[0]));
            if (p == Prim::FloatCast) return one(SimpFloatCast(Sigs, in[0]));
            if (p == Prim::Select2) return one(Select2(in[0], in[1], in[2]));
            // `select3` is not a node: it lowers to nested `select2`.
            if (p == Prim::Select3) {
                const SigId is0 = Bin(BinOpCode::EQ, in[0], Sigs.MakeInt(0));
                const SigId is1 = Bin(BinOpCode::EQ, in[0], Sigs.MakeInt(1));
                return one(Select2(is0, Select2(is1, in[3], in[2]), in[1]));
            }
            if (p == Prim::Attach) return one(Sigs.Make(SigKind::Attach, in));
            // `enable(X,Y)` is `control(X*Y, Y!=0)` and `control(X,Y)` is `control(X, Y!=0)`.
            if (p == Prim::Enable || p == Prim::Control) {
                const SigId zero = Sigs.MakeReal(0.0);
                const SigId cond = Bin(BinOpCode::NE, in[1], zero);
                const SigId x = p == Prim::Enable ? Bin(BinOpCode::Mul, in[0], in[1]) : in[0];
                return one(Sigs.Make(SigKind::Control, {x, cond}));
            }
            // A writable table adds the write pair and pushes the read index by two.
            if (p == Prim::RdTable || p == Prim::RwTable) {
                const bool rw = p == Prim::RwTable;
                const SigId gen = Sigs.Make(SigKind::Gen, {in[1]});
                const SigId tbl = rw ? Sigs.Make(SigKind::WRTbl, {in[0], gen, in[2], in[3]}) : Sigs.Make(SigKind::WRTbl, {in[0], gen});
                return one(Sigs.Make(SigKind::RDTbl, {tbl, rw ? in[4] : in[2]}));
            }
            if (const BinOpCode b = BinOpFor(p); b != BinOpCode::Count_) return one(Bin(b, in[0], in[1]));
            // The one class propagation rewrites, even on non-literal arguments.
            if (const Ext e = ExtFor(p); e != Ext::Count_) return one(SimpExtended(Sigs, e, in));
            if (p == Prim::Wire) return in;
            if (p == Prim::Cut) return {};
            return Fail(box, "no signal for this primitive");
        }

        // `Form` is the declared *result* type: `int isnanf(float)` returns an int.
        case BoxKind::FFun: return one(Sigs.Make(SigKind::FFun, uint8_t(Boxes.SignatureAt(n.Aux).Result), Sigs.InternStr(Terms.Str(n.Payload)), n.Aux, in));

        case BoxKind::Button:
        case BoxKind::Checkbox: {
            const bool check = n.Kind == uint8_t(BoxKind::Checkbox);
            const uint32_t path = Widget(n.Payload, check ? UiKind::Checkbox : UiKind::Button, nullptr);
            return one(Sigs.MakeLeaf(check ? SigKind::Checkbox : SigKind::Button, path));
        }

        // The bounds ride as *child signals* rather than a side table, all of them reals.
        case BoxKind::NumericWidget: {
            static constexpr SigKind ByForm[] = {SigKind::VSlider, SigKind::HSlider, SigKind::NumEntry};
            if (n.Form > 2) return Fail(box, "unknown widget");
            static constexpr UiKind UiByForm[] = {UiKind::VSlider, UiKind::HSlider, UiKind::NumEntry};
            const Bounds &b = Boxes.BoundsAt(n.Aux);
            const uint32_t path = Widget(n.Payload, UiByForm[n.Form], &b);
            return one(Sigs.Make(ByForm[n.Form], 0, path, 0, {Sigs.MakeReal(b.Init), Sigs.MakeReal(b.Min), Sigs.MakeReal(b.Max), Sigs.MakeReal(b.Step)}));
        }
        case BoxKind::Bargraph: {
            const SigKind k = n.Form == 0 ? SigKind::VBargraph : SigKind::HBargraph;
            const Bounds &b = Boxes.BoundsAt(n.Aux);
            const uint32_t path = Widget(n.Payload, n.Form == 0 ? UiKind::VBargraph : UiKind::HBargraph, &b);
            return one(Sigs.Make(k, 0, path, 0, {Sigs.MakeReal(b.Min), Sigs.MakeReal(b.Max), in[0]}));
        }

        // The read clamp `int(max(0, min(ridx, length-1)))`, built here to stay visible.
        case BoxKind::Soundfile: {
            const uint32_t chans = n.Aux;
            const SigId sf = Sigs.MakeLeaf(SigKind::Soundfile, Widget(n.Payload, UiKind::Soundfile, nullptr), chans);
            const SigId part = in[0];
            const SigId len = Sigs.Make(SigKind::SoundfileLength, {sf, part});
            const SigId rate = Sigs.Make(SigKind::SoundfileRate, {sf, part});
            const SigId last = Bin(BinOpCode::Sub, len, Sigs.MakeInt(1));
            const SigId lo = SimpExtended(Sigs, Ext::Min, {in[1], last});
            const SigId ridx = SimpExtended(Sigs, Ext::Max, {Sigs.MakeInt(0), lo});
            std::vector<SigId> out{len, rate};
            for (uint32_t c = 0; c < chans; ++c) out.push_back(Sigs.Make(SigKind::SoundfileBuffer, {sf, Sigs.MakeInt(int32_t(c)), part, ridx}));
            return out;
        }

        case BoxKind::Group: {
            Groups.push_back({std::string(Terms.Str(n.Payload)), n.Form, true});
            std::vector<SigId> out = Propagate(kids[0], in);
            Groups.pop_back();
            return out;
        }

        case BoxKind::Seq: return Propagate(kids[1], Propagate(kids[0], in));

        case BoxKind::Par: {
            const int32_t in1 = Boxes.ArityOf(kids[0]).Ins;
            std::vector<SigId> a(in.begin(), in.begin() + in1);
            std::vector<SigId> b(in.begin() + in1, in.end());
            std::vector<SigId> l = Propagate(kids[0], std::move(a));
            const std::vector<SigId> r = Propagate(kids[1], std::move(b));
            l.insert(l.end(), r.begin(), r.end());
            return l;
        }

        case BoxKind::Split: {
            const std::vector<SigId> l = Propagate(kids[0], in);
            const size_t nbus = size_t(Boxes.ArityOf(kids[1]).Ins);
            std::vector<SigId> fan(nbus);
            for (size_t b = 0; b < nbus; ++b) fan[b] = l[b % l.size()];
            return Propagate(kids[1], std::move(fan));
        }

        case BoxKind::Merge: {
            const std::vector<SigId> l = Propagate(kids[0], in);
            const size_t nbus = size_t(Boxes.ArityOf(kids[1]).Ins);
            std::vector<SigId> mixed(nbus);
            for (size_t b = 0; b < nbus; ++b) {
                SigId t = b < l.size() ? l[b] : Sigs.MakeInt(0);
                for (size_t i = b + nbus; i < l.size(); i += nbus) t = Bin(BinOpCode::Add, t, l[i]);
                mixed[b] = t;
            }
            return Propagate(kids[1], std::move(mixed));
        }

        // The feedback path reads `Delay1(Proj(i, g))`, the outputs `Delay(Proj(i, g), 0)`,
        // which puts a recursive output on the current sample.
        case BoxKind::Rec: {
            const int32_t out1 = Boxes.ArityOf(kids[0]).Outs;
            const int32_t in2 = Boxes.ArityOf(kids[1]).Ins;
            const SigId g = Sigs.OpenRec();

            std::vector<SigId> back(static_cast<size_t>(in2));
            for (int32_t i = 0; i < in2; ++i) back[i] = Delay1(Sigs.Make(SigKind::Proj, 0, uint32_t(i), 0, {g}));

            std::vector<SigId> l1 = Propagate(kids[1], std::move(back));
            l1.insert(l1.end(), in.begin(), in.end());
            const std::vector<SigId> body = Propagate(kids[0], std::move(l1));

            // Unlike the reference, a branch with no self-reference is still a projection.
            const SigId group = Sigs.CloseRec(g, body);
            std::vector<SigId> out(static_cast<size_t>(out1));
            for (int32_t p = 0; p < out1; ++p) out[p] = Delay(Sigs.Make(SigKind::Proj, 0, uint32_t(p), 0, {group}), Sigs.MakeInt(0));
            return out;
        }

        case BoxKind::Environment: return {};

        case BoxKind::Route: {
            if (n.Aux == 0) return Fail(box, "route without a constant table");
            const RouteTable &t = Boxes.RouteAt(n.Aux - 1);
            const SigId zero = Sigs.MakeInt(0);
            std::vector<SigId> out(size_t(t.Outs), zero);
            for (size_t i = 0; i + 1 < t.Pairs.size(); i += 2) {
                const int32_t src = t.Pairs[i], dst = t.Pairs[i + 1];
                if (dst <= 0 || dst > t.Outs || src <= 0 || src > t.Ins) continue;
                // The first contribution replaces the zero, so a plain route holds no `+`.
                const SigId acc = out[dst - 1], add = in[src - 1];
                out[dst - 1] = acc == zero ? add : add == zero ? acc : SimpBinOp(Sigs, BinOpCode::Add, acc, add);
            }
            return out;
        }

        case BoxKind::Error: return {Sigs.Error};

        default: return Fail(box, "not a circuit");
    }
}

} // namespace faustlens
