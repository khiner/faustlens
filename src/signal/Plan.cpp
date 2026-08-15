#include "signal/Plan.h"

#include "query/Query.h"
#include "signal/Simplify.h"
#include "signal/Type.h"
#include "signal/Ui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

namespace faustlens {

namespace {

constexpr std::array<std::string_view, size_t(Op::Count_)> OpNames = {
    "const.i", "const.f",   "input",   "output",  "binop",  "ext",  "int",  "float",      "bitcast",  "select2",     "select3",   "load",
    "store",   "sf.length", "sf.rate", "sf.read", "fconst", "fvar", "ffun", "loop.begin", "loop.end", "guard.begin", "guard.end",
};

// DNF is what makes a node reached both under a guard and outside it come out unguarded.
using Clause = std::vector<SigId>; // sorted. the atoms ANDed
using Dnf = std::vector<Clause>; // sorted. the clauses ORed, empty being `true`

Clause SetUnion(const Clause &a, const Clause &b) {
    Clause r;
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(r));
    return r;
}

Clause SetIntersection(const Clause &a, const Clause &b) {
    Clause r;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(r));
    return r;
}

void AddClause(Dnf &d, const Clause &c) {
    const auto at = std::ranges::lower_bound(d, c);
    if (at == d.end() || *at != c) d.insert(at, c);
}

struct Conditions {
    std::vector<Dnf> Table;
    std::map<Dnf, uint32_t> Ids;

    Conditions() { Table.emplace_back(); } // id 0 is `true`

    uint32_t Atom(SigId c) { return Intern(Dnf{Clause{c}}); }

    // The pairwise pass keeps the more general of two comparable clauses: `a` over `a & b`.
    uint32_t Or(uint32_t x, uint32_t y) {
        if (x == 0 || y == 0) return 0;
        Dnf a = Table[x], b = Table[y];
        for (Clause &ai : a)
            for (Clause &bj : b) {
                const Clause ii = SetIntersection(ai, bj);
                if (bj == ii) ai = bj;
                else if (ai == ii) bj = ai;
            }
        Dnf out;
        for (const Clause &c : a) AddClause(out, c);
        for (const Clause &c : b) AddClause(out, c);
        return Intern(std::move(out));
    }

    uint32_t And(uint32_t x, uint32_t y) {
        if (x == 0) return y;
        if (y == 0) return x;
        std::vector<Clause> a;
        for (const Clause &ai : Table[x])
            for (const Clause &bj : Table[y]) a.push_back(SetUnion(ai, bj));
        for (size_t i = 0; i < a.size(); ++i)
            for (size_t j = i + 1; j < a.size(); ++j) {
                const Clause ii = SetIntersection(a[i], a[j]);
                if (a[j] == ii) a[i] = a[j];
                else if (a[i] == ii) a[j] = a[i];
            }
        Dnf out;
        for (const Clause &c : a) AddClause(out, c);
        return Intern(std::move(out));
    }

    const Dnf &At(uint32_t i) const { return Table[i]; }

    uint32_t Intern(Dnf d) {
        if (const auto it = Ids.find(d); it != Ids.end()) return it->second;
        const uint32_t id = uint32_t(Table.size());
        Ids.emplace(d, id);
        Table.push_back(std::move(d));
        return id;
    }
};

// Hash consing overlaps the graphs, so a node emitted in two scopes gets two registers.
struct Scope {
    std::vector<Instr> *Target[3] = {nullptr, nullptr, nullptr};
    uint32_t Loop = NoLoop; // the `LoopBegin` that owns this scope's fields

    std::unordered_map<SigId, Reg> Val;
    std::unordered_map<SigId, uint32_t> Line, Perm, Table, Widget;
    std::unordered_map<SigId, uint8_t> Rec; // 0 unseen, 1 in flight, 2 done
    std::unordered_map<int32_t, Reg> Ints;
    std::unordered_map<uint32_t, Reg> GuardReg;
    std::vector<int32_t> Maxd;
    uint32_t Iota = NoField;
    Reg IotaReg = NoReg;
    uint32_t Guard = 0; // the condition currently bracketed, 0 for none

    struct Epilogue {
        enum class Kind : uint8_t { Shift, Iota, WaveIndex };
        Kind Kind = Kind::Shift;
        uint32_t Field = NoField;
        uint32_t Size = 0;
        int32_t MaxDelay = 0;
        uint32_t Cond = 0;
    };
    std::vector<Epilogue> Epilogues;
};

// `Open` settles all three before the instruction, outside the bracket it opens.
struct Slot {
    uint32_t Field = NoField;
    Reg At = NoReg;
    uint32_t Cond = 0;
};

struct Lowering {
    const Signals &Sigs;
    const std::vector<SigId> &Roots;
    Plan &Plan;
    // Set once by `Fail`, and reported by `Run`.
    std::string Why;

    std::vector<Nature> Nat;
    std::vector<Band> BandOf;
    std::vector<Interval> Iv;
    Conditions Conds;
    std::unordered_map<SigId, uint32_t> CondAt;
    // Which projection of a group is read, and by which node. An unread one is dropped.
    std::map<std::pair<SigId, uint32_t>, SigId> ProjAt;
    std::unordered_map<SigId, uint8_t> Reached;
    Scope *Sc = nullptr;
    bool Failed = false;

    std::expected<void, std::string> Run();

    bool Fail(std::string_view what, SigId id);

    void Reach(SigId);
    void Annotate(SigId, uint32_t nc);
    uint32_t CondOf(SigId id) const {
        const auto it = CondAt.find(id);
        return it == CondAt.end() ? 0 : it->second;
    }
    // A guard only ever brackets the sample band.
    uint32_t GuardOf(SigId id) const { return BandOf[id] == Band::Sample ? CondOf(id) : 0; }

    Reg NewReg() { return Plan.Regs++; }
    uint32_t PushArgs(std::span<const Reg>);
    void Push(Band, Instr) const;
    void Push(Band b, Op op, Reg dst, uint32_t imm, std::initializer_list<Reg> args) {
        Instr i;
        i.Op = uint8_t(op);
        i.Dst = dst;
        i.Imm = imm;
        i.Nature = op == Op::LoadField ? Plan.Fields[imm].Nature : Nature::Int;
        i.Args = PushArgs(std::span<const Reg>(args.begin(), args.size()));
        i.ArgCount = uint32_t(args.size());
        Push(b, i);
    }
    Reg Load(Band b, uint32_t field, std::initializer_list<Reg> at) {
        const Reg r = NewReg();
        Push(b, Op::LoadField, r, field, at);
        return r;
    }
    Reg Bin(BinOpCode, Reg, Reg);

    uint32_t AddField(FieldKind, SigId, Nature, uint32_t extent);
    uint32_t WidgetField(SigId, uint32_t label);
    uint32_t SoundfileDescOf(SigId);
    uint32_t ForeignDescOf(ForeignKind, uint32_t name, uint8_t ftype, std::span<const SigId> args);
    uint32_t LineOf(SigId);
    uint32_t IotaField();

    Reg IntReg(int32_t);
    Reg IotaReg();
    // The current sample's slot: 0 on a copy line, `IOTA & (extent - 1)` on a ring.
    Reg WriteIndex(uint32_t field);
    // The slot `delay` back. `zero` is a literal-0 delay, whose ring read is the write index.
    Reg ReadIndex(uint32_t field, Reg delay, bool zero);

    Reg GuardReg(uint32_t cond);
    void SetGuard(uint32_t cond, Reg g);

    // Emitting operands can compile a group body that reaches back here, staling
    // `Emit`'s opening memo check.
    bool Already(SigId id, Reg &out) const {
        const auto it = Sc->Val.find(id);
        if (it == Sc->Val.end()) return false;
        out = it->second;
        return true;
    }

    Slot Open(SigId);
    Reg Close(SigId, const Slot &, Reg dst);
    Reg LoadValue(SigId, Band, uint32_t field, std::initializer_list<Reg> at);

    Reg Emit(SigId);
    bool EmitRec(SigId rec);
    uint32_t EmitTable(SigId wrtbl);
    void EmitEpilogue();
};

bool Lowering::Fail(std::string_view what, SigId id) {
    if (!Failed) {
        Failed = true;
        Why = std::format("{}: {} #{}", what, SigKindName(Sigs.KindOf(id)), id);
    }
    return false;
}

uint32_t Lowering::PushArgs(std::span<const Reg> args) {
    const uint32_t at = uint32_t(Plan.Operands.size());
    Plan.Operands.insert(Plan.Operands.end(), args.begin(), args.end());
    return at;
}

void Lowering::Push(Band b, Instr i) const { Sc->Target[size_t(b)]->push_back(i); }

Reg Lowering::Bin(BinOpCode op, Reg a, Reg b) {
    const Reg r = NewReg();
    Instr i;
    i.Op = uint8_t(Op::BinOp);
    i.Form = uint8_t(op);
    i.Nature = Nature::Int; // every caller is integer: index arithmetic and the guard fold
    i.Dst = r;
    const Reg args[] = {a, b};
    i.Args = PushArgs(args);
    i.ArgCount = 2;
    Push(Band::Sample, i);
    return r;
}

uint32_t Lowering::AddField(FieldKind k, SigId sig, Nature n, uint32_t extent) {
    Field f;
    f.Kind = k;
    f.Sig = sig;
    f.Nature = n;
    f.Extent = extent;
    f.Loop = Sc->Loop;
    if (sig != NoSig) {
        f.Hash = Sigs.ContentHash(sig);
        f.Shape = Sigs.ShapeHash(sig);
        f.Origin = Sigs.OriginOf(sig);
    }
    Plan.Fields.push_back(f);
    return uint32_t(Plan.Fields.size() - 1);
}

uint32_t Lowering::WidgetField(SigId id, uint32_t label) {
    if (const auto it = Sc->Widget.find(id); it != Sc->Widget.end()) return it->second;
    const uint32_t f = AddField(FieldKind::Widget, id, Nat[id], 1);
    Plan.Fields[f].Label = label;
    return Sc->Widget[id] = f;
}

// The URL set is metadata on the widget's own label, so it comes off the innermost segment.
uint32_t Lowering::SoundfileDescOf(SigId id) {
    const SigNode n = Sigs.Get(id);
    SoundfileDesc d;
    d.Label = n.Payload;
    d.Channels = n.Aux;

    const std::string_view path = Sigs.Str(n.Payload);
    const size_t slash = path.find('/');
    std::string label;
    std::map<std::string, std::set<std::string>> meta;
    ExtractMetadata(path.substr(0, slash == std::string_view::npos ? path.size() : slash), label, meta);
    const auto url = meta.find("url");
    if (url != meta.end())
        for (const std::string &v : url->second) {
            // As the runtime's `parseMenuList2`: `{`, `;`-separated single-quoted
            // names, `}`. Anything that does not parse as a list is one URL.
            std::vector<std::string> names;
            const char *p = v.c_str();
            const auto blank = [&] {
                while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
            };
            bool list = false;
            blank();
            if (*p == '{') {
                ++p;
                for (;;) {
                    blank();
                    if (*p != '\'') break;
                    const char *a = ++p;
                    while (*p && *p != '\'') ++p;
                    if (*p != '\'') break;
                    names.emplace_back(a, p++);
                    blank();
                    if (*p == ';') {
                        ++p;
                        continue;
                    }
                    list = *p == '}';
                    break;
                }
            }
            if (list) d.Urls.insert(d.Urls.end(), names.begin(), names.end());
            else d.Urls.push_back(v);
        }

    for (size_t i = 0; i < Plan.Soundfiles.size(); ++i)
        if (Plan.Soundfiles[i].Label == d.Label) return uint32_t(i);
    Plan.Soundfiles.push_back(std::move(d));
    return uint32_t(Plan.Soundfiles.size() - 1);
}

uint32_t Lowering::ForeignDescOf(ForeignKind kind, uint32_t name, uint8_t ftype, std::span<const SigId> args) {
    ForeignDesc d;
    d.Kind = kind;
    d.Name = Sigs.Str(name);
    // Typed by the declared result: `ffunction(int isnanf(float))` is an int.
    d.Result = FType(ftype) == FType::Int ? Nature::Int : Nature::Real;
    for (const SigId a : args) d.Args.push_back(Nat[a]);
    for (size_t i = 0; i < Plan.Foreign.size(); ++i) {
        const ForeignDesc &e = Plan.Foreign[i];
        if (e.Kind == d.Kind && e.Name == d.Name && e.Result == d.Result && e.Args == d.Args) return uint32_t(i);
    }
    Plan.Foreign.push_back(std::move(d));
    return uint32_t(Plan.Foreign.size() - 1);
}

uint32_t Lowering::LineOf(SigId id) {
    if (const auto it = Sc->Line.find(id); it != Sc->Line.end()) return it->second;
    const DelayLine l = LineFor(Sigs, id, Sc->Maxd[id], Nat[id]);
    if (l.Extent == 0) return Sc->Line[id] = NoField;
    const uint32_t f = AddField(FieldKind::Delay, id, l.Nature, l.Extent);
    Plan.Fields[f].Ring = l.Ring;
    Plan.Fields[f].MaxDelay = l.MaxDelay;
    if (l.Ring) IotaField();
    else Sc->Epilogues.push_back({Scope::Epilogue::Kind::Shift, f, 0, l.MaxDelay, GuardOf(id)});
    return Sc->Line[id] = f;
}

uint32_t Lowering::IotaField() {
    if (Sc->Iota == NoField) {
        Sc->Iota = AddField(FieldKind::Perm, NoSig, Nature::Int, 1);
        Sc->Epilogues.push_back({Scope::Epilogue::Kind::Iota, Sc->Iota, 0, 0, 0});
    }
    return Sc->Iota;
}

Reg Lowering::IntReg(int32_t v) {
    if (const auto it = Sc->Ints.find(v); it != Sc->Ints.end()) return it->second;
    SetGuard(0, NoReg);
    const Reg r = NewReg();
    Push(Band::Sample, Op::ConstInt, r, BitsOf(v), {});
    return Sc->Ints[v] = r;
}

Reg Lowering::IotaReg() {
    if (Sc->IotaReg != NoReg) return Sc->IotaReg;
    const uint32_t f = IotaField();
    SetGuard(0, NoReg);
    return Sc->IotaReg = Load(Band::Sample, f, {});
}

Reg Lowering::WriteIndex(uint32_t field) {
    if (!Plan.Fields[field].Ring) return IntReg(0);
    const int32_t mask = int32_t(Plan.Fields[field].Extent) - 1;
    const Reg iota = IotaReg(); // may allocate, so the field reference is not held
    return Bin(BinOpCode::AND, iota, IntReg(mask));
}

Reg Lowering::ReadIndex(uint32_t field, Reg delay, bool zero) {
    if (!Plan.Fields[field].Ring) return delay;
    const int32_t mask = int32_t(Plan.Fields[field].Extent) - 1;
    const Reg iota = IotaReg();
    return Bin(BinOpCode::AND, zero ? iota : Bin(BinOpCode::Sub, iota, delay), IntReg(mask));
}

void Lowering::Reach(SigId t) {
    if (Reached[t]) return;
    Reached[t] = 1;
    if (Sigs.KindOf(t) == SigKind::Proj) {
        const SigId rec = Sigs.Child(t, 0);
        const uint32_t i = Sigs.Get(t).Payload;
        if (i >= Sigs.Get(rec).ChildCount) return;
        ProjAt[{rec, i}] = t;
        Reach(Sigs.Child(rec, i)); // branch `i` alone: the group is not a node
        return;
    }
    for (const SigId c : Sigs.Children(t)) Reach(c);
}

void Lowering::Annotate(SigId t, uint32_t nc) {
    if (const auto it = CondAt.find(t); it != CondAt.end()) {
        const uint32_t xc = Conds.Or(it->second, nc);
        if (xc == it->second) return; // already at least this general
        nc = it->second = xc;
    } else {
        CondAt[t] = nc;
    }
    // A `Control` conjoins its condition onto its value operand only, not the condition one.
    if (Sigs.KindOf(t) == SigKind::Control) {
        const SigId x = Sigs.Child(t, 0), y = Sigs.Child(t, 1);
        Annotate(y, nc);
        Annotate(x, Conds.And(nc, Conds.Atom(y)));
        return;
    }
    if (Sigs.KindOf(t) == SigKind::Gen) return;
    for (uint32_t i = 0; i < Sigs.Get(t).ChildCount; ++i) Annotate(Sigs.Child(t, i), nc);
}

Reg Lowering::GuardReg(uint32_t cond) {
    if (const auto it = Sc->GuardReg.find(cond); it != Sc->GuardReg.end()) return it->second;
    // Atoms emit under their own conditions, the combining instructions outside every bracket.
    std::vector<std::vector<Reg>> atoms;
    for (const Clause &c : Conds.At(cond)) {
        atoms.emplace_back();
        for (const SigId a : c) atoms.back().push_back(Emit(a));
    }
    if (Failed) return NoReg;
    SetGuard(0, NoReg);
    const auto fold = [&](BinOpCode op, std::span<const Reg> in) {
        Reg acc = in[0];
        for (size_t i = 1; i < in.size(); ++i) acc = Bin(op, acc, in[i]);
        return acc;
    };
    std::vector<Reg> clauses;
    clauses.reserve(atoms.size());
    for (const std::vector<Reg> &c : atoms) clauses.push_back(fold(BinOpCode::AND, c));
    return Sc->GuardReg[cond] = fold(BinOpCode::OR, clauses);
}

void Lowering::SetGuard(uint32_t cond, Reg g) {
    if (Sc->Guard == cond) return;
    if (Sc->Guard != 0) Push(Band::Sample, Op::GuardEnd, NoReg, 0, {});
    Sc->Guard = cond;
    if (cond != 0) Push(Band::Sample, Op::GuardBegin, NoReg, 0, {g});
}

Slot Lowering::Open(SigId id) {
    Slot sl;
    sl.Field = LineOf(id);
    if (sl.Field != NoField) sl.At = WriteIndex(sl.Field);
    sl.Cond = GuardOf(id);
    const Reg g = sl.Cond ? GuardReg(sl.Cond) : NoReg;
    if (!Failed) SetGuard(sl.Cond, g);
    return sl;
}

Reg Lowering::Close(SigId id, const Slot &sl, Reg dst) {
    // The store is in the frame loop whatever band computed the value, so a block-rate
    // signal keeps a per-sample history.
    if (sl.Field != NoField) Push(Band::Sample, Op::StoreField, NoReg, sl.Field, {sl.At, dst});
    if (sl.Cond == 0) return dst;

    // A guarded value has to survive a frame the guard was false in, so it comes back out
    // of state, loaded outside the bracket.
    if (sl.Field != NoField) {
        SetGuard(0, NoReg);
        return Load(Band::Sample, sl.Field, {sl.At});
    }
    const uint32_t perm = Sc->Perm.contains(id) ? Sc->Perm[id] : (Sc->Perm[id] = AddField(FieldKind::Perm, id, Nat[id], 1));
    Push(Band::Sample, Op::StoreField, NoReg, perm, {dst});
    SetGuard(0, NoReg);
    return Load(Band::Sample, perm, {});
}

Reg Lowering::LoadValue(SigId id, Band b, uint32_t field, std::initializer_list<Reg> at) {
    Reg done;
    if (Already(id, done)) return done;
    const Slot sl = Open(id);
    return Sc->Val[id] = Close(id, sl, Load(b, field, at));
}

// The one loop other than the frame loop. The generator gets its own scope.
uint32_t Lowering::EmitTable(SigId id) {
    if (const auto it = Sc->Table.find(id); it != Sc->Table.end()) return it->second;
    const SigId size_sig = Sigs.Child(id, 0), gen = Sigs.Child(id, 1);
    if (!Sigs.IsInt(size_sig) || Sigs.IntValue(size_sig) <= 0) return Fail("a table size that is not a folded positive constant", id), NoField;
    if (Sigs.KindOf(gen) != SigKind::Gen) return Fail("a table whose contents are not a generator", id), NoField;
    const uint32_t size = uint32_t(Sigs.IntValue(size_sig));

    const uint32_t f = AddField(FieldKind::Table, id, Nat[id], size);
    Sc->Table[id] = f;

    const uint32_t loop = uint32_t(Sc->Target[0]->size());
    const Reg i = NewReg();
    Push(Band::Init, Op::LoopBegin, i, size, {});

    Scope inner;
    inner.Target[0] = inner.Target[1] = inner.Target[2] = Sc->Target[0];
    inner.Loop = loop;
    const std::array gen_roots{Sigs.Child(gen, 0)};
    const auto maxd = MaxDelays(Sigs, Iv, gen_roots);
    if (!maxd) return Fail("a generator whose delay index is not bounded", id), NoField;
    inner.Maxd = *maxd;

    Scope *outer = Sc;
    Sc = &inner;
    const Reg v = Emit(gen_roots[0]);
    if (!Failed) {
        Push(Band::Init, Op::StoreField, NoReg, f, {i, v});
        EmitEpilogue();
    }
    Sc = outer;
    Push(Band::Init, Op::LoopEnd, NoReg, 0, {});
    return f;
}

// A body reaches its own projections only through a delay, so the lines cut the cycle.
bool Lowering::EmitRec(SigId rec) {
    if (Sc->Rec[rec] != 0) return true;
    Sc->Rec[rec] = 1;
    const uint32_t n = Sigs.Get(rec).ChildCount;
    std::vector<SigId> projs(n, NoSig);
    for (uint32_t i = 0; i < n; ++i) {
        const auto it = ProjAt.find({rec, i});
        if (it != ProjAt.end()) projs[i] = it->second;
    }
    for (uint32_t i = 0; i < n; ++i)
        if (projs[i] != NoSig) LineOf(projs[i]);
    for (uint32_t i = 0; i < n; ++i) {
        if (projs[i] == NoSig) continue;
        const SigId p = projs[i];
        const Reg r = Emit(Sigs.Child(rec, i));
        if (Failed) return false;
        if (Sc->Line[p] == NoField) {
            Sc->Val[p] = r;
            continue;
        }
        // Unguarded, `Close` reads back `r` itself: a projection's `@0` is the register.
        const Slot sl = Open(p);
        if (Failed) return false;
        Sc->Val[p] = Close(p, sl, r);
    }
    Sc->Rec[rec] = 2;
    return !Failed;
}

Reg Lowering::Emit(SigId id) {
    if (const auto it = Sc->Val.find(id); it != Sc->Val.end()) return it->second;
    if (Failed) return NoReg;

    const SigNode n = Sigs.Get(id); // by value: `Children` is a span into the arena
    const SigKind k = SigKind(n.Kind);
    const Band b = BandOf[id];
    const std::vector<SigId> kids(Sigs.Children(id).begin(), Sigs.Children(id).end());

    // None of the forwarding kinds takes a delay line: it belongs to the producer.
    switch (k) {
        case SigKind::Error: return Fail("an error node reached lowering", id), NoReg;
        case SigKind::Rec: return Fail("a group reached lowering other than through a projection", id), NoReg;
        case SigKind::Gen: return Fail("a generator reached lowering other than through a table", id), NoReg;

        // Emitting `y` is what widens the root set.
        case SigKind::Attach: {
            const Reg r = Emit(kids[0]);
            Emit(kids[1]);
            return Failed ? NoReg : (Sc->Val[id] = r);
        }
        // The guard is not here, it is the condition annotated onto `x`.
        case SigKind::Control: {
            Emit(kids[1]);
            const Reg r = Emit(kids[0]);
            return Failed ? NoReg : (Sc->Val[id] = r);
        }
        case SigKind::Proj: {
            if (!EmitRec(kids[0])) return NoReg;
            if (const auto it = Sc->Val.find(id); it != Sc->Val.end()) return it->second;
            return Fail("a group's body reaches its own projection without a delay", id), NoReg;
        }
        case SigKind::Extended: {
            const Ext e = Ext(n.Form);
            // The bounds feed interval analysis rather than the program.
            if (e == Ext::AssertBounds) {
                const Reg r = Emit(kids[2]);
                return Failed ? NoReg : (Sc->Val[id] = r);
            }
            // Constant at the operand's bound, so the operand is typed, not emitted.
            if (e == Ext::Lowest || e == Ext::Highest) {
                const double v = e == Ext::Lowest ? Iv[kids[0]].Lo : Iv[kids[0]].Hi;
                const uint64_t bits = BitsOf(v);
                const Slot sl = Open(id);
                const Reg dst = NewReg();
                Instr c;
                c.Op = uint8_t(Op::ConstReal);
                c.Nature = Nature::Real;
                c.Dst = dst;
                c.Imm = uint32_t(bits);
                c.Aux = uint32_t(bits >> 32);
                Push(b, c);
                return Sc->Val[id] = Close(id, sl, dst);
            }
            break;
        }
        default: break;
    }

    switch (k) {
        // The host writes the field. The bounds belong to the UI tree, not the program.
        case SigKind::Button:
        case SigKind::Checkbox:
        case SigKind::VSlider:
        case SigKind::HSlider:
        case SigKind::NumEntry: return LoadValue(id, b, WidgetField(id, n.Payload), {});
        // Read only through the three accessors, so it carries a field and no register.
        case SigKind::Soundfile: {
            if (!Sc->Widget.contains(id)) {
                const uint32_t f = AddField(FieldKind::Soundfile, id, Nature::Real, 1);
                Plan.Fields[f].Desc = SoundfileDescOf(id);
                Sc->Widget[id] = f;
            }
            return Sc->Val[id] = NoReg;
        }
        case SigKind::WRTbl: {
            const uint32_t f = EmitTable(id);
            if (f == NoField) return NoReg;
            // Placed by variability, so a constant-rate write lands in the init band.
            if (kids.size() == 4) {
                const Reg wi = Emit(kids[2]), ws = Emit(kids[3]);
                if (Failed) return NoReg;
                const uint32_t c = GuardOf(id);
                if (c) SetGuard(c, GuardReg(c));
                Push(b, Op::StoreField, NoReg, f, {wi, ws});
                if (c) SetGuard(0, NoReg);
            }
            // A table is a value in the graph only, ordering a read after its write.
            return Sc->Val[id] = NoReg;
        }
        case SigKind::RDTbl: {
            const SigId tbl = kids[0];
            if (Sigs.KindOf(tbl) != SigKind::WRTbl) return Fail("a table read whose table is not a table", id), NoReg;
            Emit(tbl);
            if (Failed) return NoReg;
            const uint32_t f = Sc->Table[tbl];
            const Reg ri = Emit(kids[1]);
            if (Failed) return NoReg;
            return LoadValue(id, b, f, {ri});
        }
        // A bare `waveform` cycles: a static array plus an index advanced per frame.
        case SigKind::Waveform: {
            const std::vector<double> &w = Sigs.WaveformAt(n.Aux);
            const uint32_t f = AddField(FieldKind::Table, id, Nat[id], uint32_t(w.size()));
            Plan.Fields[f].Desc = uint32_t(Plan.Waves.size());
            Plan.Waves.push_back(w);
            const uint32_t idx = AddField(FieldKind::Perm, id, Nature::Int, 1);
            Sc->Epilogues.push_back({Scope::Epilogue::Kind::WaveIndex, idx, uint32_t(w.size()), 0, GuardOf(id)});
            const Slot sl = Open(id);
            const Reg at = Load(b, idx, {});
            return Sc->Val[id] = Close(id, sl, Load(b, f, {at}));
        }
        case SigKind::Delay1:
        case SigKind::Delay: {
            const SigId x = kids[0];
            const Reg d = k == SigKind::Delay1 ? IntReg(1) : Emit(kids[1]);
            if (Failed) return NoReg;
            // A projection whose group is in flight stops here, cutting the cycle.
            if (Sigs.KindOf(x) == SigKind::Proj) {
                if (!EmitRec(Sigs.Child(x, 0))) return NoReg;
            } else if (!Sc->Line.contains(x)) {
                Emit(x);
                if (Failed) return NoReg;
            }
            const auto at_line = Sc->Line.find(x);
            const uint32_t xf = at_line == Sc->Line.end() ? NoField : at_line->second;
            if (xf == NoField) {
                // `s@0` survives simplification only on a projection. Any other read of
                // history a signal does not keep is an error.
                const auto v = Sc->Val.find(x);
                if (v == Sc->Val.end() || Sc->Maxd[x] > 0) return Fail("a delayed read of a signal that keeps no history", id), NoReg;
                return Sc->Val[id] = v->second;
            }
            Reg done;
            if (Already(id, done)) return done;
            const Reg ri = ReadIndex(xf, d, k == SigKind::Delay && Sigs.IsInt(kids[1]) && Sigs.IntValue(kids[1]) == 0);
            return LoadValue(id, b, xf, {ri});
        }
        // `prefix(x, y)` is `x` at time 0 then `y` delayed. The load precedes the store,
        // cutting the cycle when `y` reaches back here.
        case SigKind::Prefix: {
            const uint32_t f = AddField(FieldKind::Perm, id, Nat[id], 1);
            Sc->Perm[id] = f;
            if (BandOf[kids[0]] != Band::Init) return Fail("a prefix whose initial value is not constant", id), NoReg;
            const Reg init = Emit(kids[0]);
            if (Failed) return NoReg;
            Push(Band::Init, Op::StoreField, NoReg, f, {init});
            const Reg v = Load(Band::Sample, f, {});
            Sc->Val[id] = v;
            const Reg next = Emit(kids[1]);
            if (Failed) return NoReg;
            const uint32_t c = GuardOf(id);
            if (c) SetGuard(c, GuardReg(c));
            Push(Band::Sample, Op::StoreField, NoReg, f, {next});
            if (c) SetGuard(0, NoReg);
            return v;
        }
        default: break;
    }

    Instr in;
    in.Form = n.Form;
    in.Nature = Nat[id];
    switch (k) {
        case SigKind::Int:
            in.Op = uint8_t(Op::ConstInt);
            in.Imm = n.Payload;
            break;
        case SigKind::Real:
            in.Op = uint8_t(Op::ConstReal);
            in.Imm = n.Payload;
            in.Aux = n.Aux;
            break;
        case SigKind::Input:
            in.Op = uint8_t(Op::Input);
            in.Imm = n.Payload;
            break;
        // The descriptor index, not the name, so reading a Plan needs no `Signals`.
        case SigKind::FConst:
            in.Op = uint8_t(Op::FConst);
            in.Imm = ForeignDescOf(ForeignKind::Constant, n.Payload, n.Form, {});
            break;
        case SigKind::FVar:
            in.Op = uint8_t(Op::FVar);
            in.Imm = ForeignDescOf(ForeignKind::Variable, n.Payload, n.Form, {});
            break;
        case SigKind::FFun:
            in.Op = uint8_t(Op::FFun);
            in.Imm = ForeignDescOf(ForeignKind::Function, n.Payload, n.Form, kids);
            break;
        case SigKind::BinOp: in.Op = uint8_t(Op::BinOp); break;
        case SigKind::Extended: in.Op = uint8_t(Op::Extended); break;
        case SigKind::IntCast: in.Op = uint8_t(Op::IntCast); break;
        case SigKind::FloatCast: in.Op = uint8_t(Op::FloatCast); break;
        case SigKind::BitCast: in.Op = uint8_t(Op::BitCast); break;
        // Faust evaluates both branches and then selects, so they are ordinary operands.
        case SigKind::Select2: in.Op = uint8_t(Op::Select2); break;
        case SigKind::Select3: in.Op = uint8_t(Op::Select3); break;
        case SigKind::SoundfileLength: in.Op = uint8_t(Op::SoundfileLength); break;
        case SigKind::SoundfileRate: in.Op = uint8_t(Op::SoundfileRate); break;
        case SigKind::SoundfileBuffer: in.Op = uint8_t(Op::SoundfileRead); break;
        case SigKind::VBargraph:
        case SigKind::HBargraph: break; // below, once its operand exists
        default: return Fail("no lowering for this node", id), NoReg;
    }

    if (k == SigKind::VBargraph || k == SigKind::HBargraph) {
        // The program reads the load, where a `FAUSTFLOAT` round trip shows at single precision.
        const Reg v = Emit(kids[2]);
        if (Failed) return NoReg;
        const uint32_t f = WidgetField(id, n.Payload);
        Reg done;
        if (Already(id, done)) return done;
        const Slot sl = Open(id);
        Push(b, Op::StoreField, NoReg, f, {v});
        return Sc->Val[id] = Close(id, sl, Load(b, f, {}));
    }

    std::vector<Reg> args;
    if (k == SigKind::SoundfileLength || k == SigKind::SoundfileRate || k == SigKind::SoundfileBuffer) {
        Emit(kids[0]); // allocates the pointer field
        if (Failed) return NoReg;
        in.Imm = Sc->Widget[kids[0]];
        for (size_t i = 1; i < kids.size(); ++i) args.push_back(Emit(kids[i]));
    } else {
        for (const SigId c : kids) args.push_back(Emit(c));
    }
    if (Failed) return NoReg;
    Reg done;
    if (Already(id, done)) return done;

    const Slot sl = Open(id);
    if (Failed) return NoReg;
    const Reg dst = NewReg();
    in.Dst = dst;
    in.Args = PushArgs(args);
    in.ArgCount = uint32_t(args.size());
    Push(b, in);
    // Index arithmetic shares the literal's instruction.
    if (k == SigKind::Int) Sc->Ints.emplace(Sigs.IntValue(id), dst);
    return Sc->Val[id] = Close(id, sl, dst);
}

void Lowering::EmitEpilogue() {
    SetGuard(0, NoReg);
    // Indexed rather than ranged: the vector can grow while it is walked.
    for (size_t e = 0; e < Sc->Epilogues.size() && !Failed; ++e) {
        const Scope::Epilogue ep = Sc->Epilogues[e];
        const Reg g = ep.Cond ? GuardReg(ep.Cond) : NoReg;
        if (Failed) return;
        SetGuard(ep.Cond, g);
        switch (ep.Kind) {
            // A copy line is capped at fifteen pairs, so the sample band stays loop-free.
            case Scope::Epilogue::Kind::Shift:
                for (int32_t i = ep.MaxDelay; i >= 1; --i) {
                    const Reg from = IntReg(i - 1), to = IntReg(i);
                    SetGuard(ep.Cond, g); // `IntReg` closes any open bracket
                    const Reg r = Load(Band::Sample, ep.Field, {from});
                    Push(Band::Sample, Op::StoreField, NoReg, ep.Field, {to, r});
                }
                break;
            case Scope::Epilogue::Kind::Iota: {
                const Reg cur = Load(Band::Sample, ep.Field, {});
                const Reg nxt = Bin(BinOpCode::Add, cur, IntReg(1));
                Push(Band::Sample, Op::StoreField, NoReg, ep.Field, {nxt});
                break;
            }
            case Scope::Epilogue::Kind::WaveIndex: { // `(1 + idx) % size`
                const Reg cur = Load(Band::Sample, ep.Field, {});
                const Reg inc = Bin(BinOpCode::Add, IntReg(1), cur);
                const Reg nxt = Bin(BinOpCode::Rem, inc, IntReg(int32_t(ep.Size)));
                Push(Band::Sample, Op::StoreField, NoReg, ep.Field, {nxt});
                break;
            }
        }
    }
    SetGuard(0, NoReg);
}

void Prune(Plan &p) {
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<uint8_t> read(p.Regs, 0);
        for (const std::vector<Instr> &band : p.Bands)
            for (const Instr &i : band)
                for (uint32_t a = 0; a < i.ArgCount; ++a) read[p.Operands[i.Args + a]] = 1;
        // A loop's induction register is not a value, so it survives an unread `Dst`.
        for (std::vector<Instr> &band : p.Bands) {
            const size_t was = band.size();
            std::erase_if(band, [&](const Instr &i) { return i.Dst != NoReg && !read[i.Dst] && Op(i.Op) != Op::LoopBegin; });
            changed = changed || band.size() != was;
        }
    }
}

std::expected<void, std::string> Lowering::Run() {
    Nat = InferNatures(Sigs);
    BandOf = AssignBands(InferVariability(Sigs));
    Iv = InferIntervals(Sigs);

    Scope main;
    for (size_t b = 0; b < Plan.Bands.size(); ++b) main.Target[b] = &Plan.Bands[b];
    auto maxd = MaxDelays(Sigs, Iv, Roots);
    if (!maxd) return std::unexpected(std::move(maxd).error());
    main.Maxd = *std::move(maxd);
    Sc = &main;

    for (const SigId r : Roots) {
        Reach(r);
        Annotate(r, 0);
    }

    std::vector<Reg> outs;
    for (const SigId r : Roots) {
        outs.push_back(Emit(r));
        if (Failed) return std::unexpected(std::move(Why));
    }
    SetGuard(0, NoReg);
    for (size_t i = 0; i < outs.size(); ++i) Push(Band::Sample, Op::Output, NoReg, uint32_t(i), {outs[i]});
    EmitEpilogue();
    Plan.Outputs = int32_t(Roots.size());
    if (Failed) return std::unexpected(std::move(Why));
    Prune(Plan);
    return {};
}

} // namespace

std::string_view OpName(Op o) { return OpNames[size_t(o)]; }

Graph::Graph(Session &s, const std::string &path, Signals &sigs, bool add_normal_form)
    : Prop(s.Boxes, s.Terms, sigs), Box(s.Process(path)), Arity(s.Boxes.ArityOf(Box)), Ok(!s.Boxes.IsError(Box) && Arity.Known) {
    if (Ok) Outs = Normalize(sigs, Prop.Run(Box, Arity.Ins), add_normal_form);
}

std::expected<Plan, std::string> Graph::Lower() const {
    const Signals &s = Prop.Sigs;
    Plan out;
    out.Inputs = Arity.Ins;
    if (auto lowered = Lowering{s, Outs, out}.Run(); !lowered) return std::unexpected(std::move(lowered).error());
    // Copied out of the arena that interned them: the ids mean nothing outside it.
    for (const Field &f : out.Fields) {
        if (f.Kind != FieldKind::Widget) continue;
        if (out.Labels.size() <= f.Label) out.Labels.resize(f.Label + 1);
        out.Labels[f.Label] = std::string(s.Str(f.Label));
    }
    return out;
}

UiNode Graph::Ui(std::string_view root_name) const { return BuildUiTree(Prop.Ui, root_name, KeepCounts(Prop.Sigs, Outs)); }

uint64_t Hash(const Plan &p) {
    uint64_t h = Mix(0xA0761D6478BD642Full, uint64_t(p.Inputs));
    h = Mix(h, uint64_t(p.Outputs));
    h = Mix(h, p.Regs);
    for (const Field &f : p.Fields) {
        h = Mix(h, uint64_t(f.Kind));
        h = Mix(h, uint64_t(f.Nature));
        h = Mix(h, f.Hash); // `Origin` deliberately not mixed: it is position
        h = Mix(h, f.Shape);
        h = Mix(h, f.Extent);
        h = Mix(h, f.Ring ? 1 : 0);
        h = Mix(h, uint64_t(f.MaxDelay));
        h = Mix(h, f.Label);
        h = Mix(h, f.Desc);
        h = Mix(h, f.Loop);
    }
    for (const std::vector<Instr> &band : p.Bands)
        for (const Instr &i : band) {
            h = Mix(h, uint64_t(i.Op));
            h = Mix(h, uint64_t(i.Form));
            h = Mix(h, uint64_t(i.Nature));
            h = Mix(h, i.Dst);
            h = Mix(h, i.Imm);
            h = Mix(h, i.Aux);
            for (const Reg r : p.Args(i)) h = Mix(h, r);
        }
    for (const std::vector<double> &w : p.Waves)
        for (const double v : w) h = Mix(h, BitsOf(v));
    for (const SoundfileDesc &d : p.Soundfiles) {
        h = Mix(h, d.Label);
        h = Mix(h, d.Channels);
        for (const std::string &u : d.Urls)
            for (const char c : u) h = Mix(h, uint8_t(c));
    }
    for (const ForeignDesc &d : p.Foreign) {
        h = Mix(h, uint64_t(d.Kind));
        h = Mix(h, uint64_t(d.Result));
        for (const char c : d.Name) h = Mix(h, uint8_t(c));
        for (const Nature n : d.Args) h = Mix(h, uint64_t(n));
    }
    return h;
}

} // namespace faustlens
