#include "runtime/Native.h"
#include "runtime/Math.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <map>
#include <stdexcept>

namespace faustlens {
namespace {

int32_t SoundLength(const Instance *dsp, uint32_t field, uint32_t part) { return dsp->Sound[field]->Length[std::min(part, Soundfile::Parts - 1)]; }

int32_t SoundRate(const Instance *dsp, uint32_t field, uint32_t part) { return dsp->Sound[field]->Rate[std::min(part, Soundfile::Parts - 1)]; }

double SoundRead(const Instance *dsp, uint32_t field, uint32_t channel, uint32_t part, uint32_t frame) {
    const Soundfile &sf = *dsp->Sound[field];
    const uint32_t at = uint32_t(sf.Offset[std::min(part, Soundfile::Parts - 1)]) + frame;
    return sf.Channel[std::min(channel, uint32_t(sf.Channel.size()) - 1)][std::min<size_t>(at, sf.Owned[0].size() - 1)];
}

struct Emit {
    const Instance &Dsp;
    std::vector<uint32_t> Words;
    std::vector<const Instr *> Def;
    std::array<std::vector<Instr>, 3> Prepared;
    std::vector<Reg> Operands;

    std::span<const Reg> Args(const Instr &i) const { return {Operands.data() + i.Args, i.ArgCount}; }
    struct Loop {
        size_t Body, Exit;
        Reg Counter;
        uint32_t Bound;
    };
    std::vector<Loop> Loops;
    struct Allocation {
        uint32_t Begin = UINT32_MAX, End = 0, Physical = 0;
        Nature Type = Nature::Real;
        bool Written = false;
    };
    struct CachedField {
        uint32_t Field, Index;
    };
    std::vector<Allocation> Alloc;
    std::vector<CachedField> Cached;
    uint32_t Pc = 0;
    std::vector<uint32_t> CallMask;
    std::vector<std::pair<bool, uint32_t>> Callee;
    uint32_t StackBytes = 0;
    struct Channel {
        bool Output;
        uint32_t Index, Physical;
    };
    std::vector<Channel> Channels;

    uint32_t ChannelReg(bool output, uint32_t index) const {
        const auto it = std::ranges::find_if(Channels, [&](const Channel &c) { return c.Output == output && c.Index == index; });
        return it == Channels.end() ? 0 : it->Physical;
    }
    uint32_t CachedReg(uint32_t field, uint32_t index) const {
        const auto it = std::ranges::find_if(Cached, [&](const CachedField &f) { return f.Field == field && f.Index == index; });
        return it == Cached.end() ? 0 : Alloc[Dsp.Plan.Regs + (it - Cached.begin())].Physical;
    }

    std::optional<uint32_t> Index(const Instr &i) const {
        const auto args = Args(i);
        if (args.size() == (Op(i.Op) == Op::StoreField ? 1u : 0u)) return 0;
        const Instr *def = Def[args[0]];
        if (!def || Op(def->Op) != Op::ConstInt) return {};
        return std::min(def->Imm, std::max(1u, Dsp.Plan.Fields[i.Imm].Extent) - 1);
    }
    void Allocate(std::span<const Instr> code, faustlens::Band band) {
        Channels.clear();
        for (const Instr &i : code)
            if (Op(i.Op) == Op::Input || Op(i.Op) == Op::Output) {
                const bool output = Op(i.Op) == Op::Output;
                if (Channels.size() < 8 && !ChannelReg(output, i.Imm)) {
                    const auto k = uint32_t(Channels.size());
                    Channels.push_back({output, i.Imm, k == 0 ? 8u : k == 1 ? 15u : 17u + k});
                }
            }
        const uint32_t integerBegin = 19 + uint32_t(Channels.size() > 2 ? Channels.size() - 2 : 0);
        Alloc.assign(Dsp.Plan.Regs, {});
        Cached.clear();
        for (Reg r = 0; r < Dsp.Plan.Regs; ++r) Alloc[r].Type = Dsp.Registers.Types[r];
        std::vector<bool> eligible(Dsp.Plan.Fields.size(), band == faustlens::Band::Sample);
        for (const Instr &i : code)
            if (Op(i.Op) == Op::LoadField || Op(i.Op) == Op::StoreField)
                if (!Index(i) || Dsp.Plan.Fields[i.Imm].Kind == FieldKind::Widget) eligible[i.Imm] = false;
        struct LifetimeLoop {
            Reg Counter;
            uint32_t Begin;
            std::vector<Reg> LiveIn;
        };
        std::vector<LifetimeLoop> counters;
        for (uint32_t pc = 0; pc < code.size(); ++pc) {
            const Instr &i = code[pc];
            const auto touch = [&](Reg r, bool write) {
                auto &a = Alloc[r];
                if (!write)
                    for (auto &loop : counters)
                        if (a.Begin < loop.Begin) loop.LiveIn.push_back(r);
                a.Begin = std::min(a.Begin, pc);
                a.End = std::max(a.End, pc);
                a.Written |= write;
            };
            const auto args = Args(i);
            const bool fixed = (Op(i.Op) == Op::LoadField || Op(i.Op) == Op::StoreField) && Index(i).has_value();
            for (size_t k = 0; k < args.size(); ++k)
                if (!fixed || (Op(i.Op) == Op::StoreField && k + 1 == args.size())) touch(args[k], false);
            if (i.Dst != NoReg) touch(i.Dst, true);
            if (Op(i.Op) == Op::LoopBegin) counters.push_back({i.Dst, pc, {}});
            if (Op(i.Op) == Op::LoopEnd) {
                if (counters.empty()) throw std::runtime_error("unmatched native loop end");
                touch(counters.back().Counter, true);
                for (Reg r : counters.back().LiveIn) Alloc[r].End = pc;
                counters.pop_back();
            }
            if ((Op(i.Op) == Op::LoadField || Op(i.Op) == Op::StoreField) && eligible[i.Imm]) {
                const uint32_t index = *Index(i);
                auto it = std::ranges::find_if(Cached, [&](const CachedField &f) { return f.Field == i.Imm && f.Index == index; });
                if (it == Cached.end()) {
                    Cached.push_back({i.Imm, index});
                    Alloc.push_back({0, uint32_t(code.size()), 0, Dsp.Plan.Fields[i.Imm].Nature, Op(i.Op) == Op::StoreField});
                } else Alloc[Dsp.Plan.Regs + (it - Cached.begin())].Written |= Op(i.Op) == Op::StoreField;
            }
        }
        if (!counters.empty()) throw std::runtime_error("unmatched native loop begin");
        for (Reg r : Dsp.Registers.Persistent)
            if (Alloc[r].Begin != UINT32_MAX) {
                Alloc[r].Begin = 0;
                Alloc[r].End = uint32_t(code.size());
            }
        std::vector<Reg> order;
        for (Reg r = 0; r < Alloc.size(); ++r)
            if (Alloc[r].Begin != UINT32_MAX) order.push_back(r);
        std::ranges::stable_sort(order, {}, [&](Reg r) { return Alloc[r].Begin; });
        for (Nature type : {Nature::Int, Nature::Real}) {
            std::vector<uint32_t> free;
            for (uint32_t reg = type == Nature::Int ? integerBegin : 3; reg < (type == Nature::Int ? 29u : 32u); ++reg) free.push_back(reg);
            std::reverse(free.begin(), free.end());
            std::vector<Reg> active;
            for (Reg r : order) {
                auto &a = Alloc[r];
                if (a.Type != type) continue;
                for (size_t k = 0; k < active.size();) {
                    if (Alloc[active[k]].End < a.Begin) {
                        free.push_back(Alloc[active[k]].Physical);
                        active.erase(active.begin() + k);
                    } else ++k;
                }
                if (free.empty()) {
                    auto far = std::ranges::max_element(active, {}, [&](Reg at) { return Alloc[at].End; });
                    if (far != active.end() && Alloc[*far].End > a.End) {
                        free.push_back(Alloc[*far].Physical);
                        Alloc[*far].Physical = 0;
                        active.erase(far);
                    }
                }
                if (!free.empty()) {
                    a.Physical = free.back();
                    free.pop_back();
                    active.push_back(r);
                }
            }
        }
        for (uint32_t pc = 0; pc < code.size(); ++pc) {
            const Instr &i = code[pc];
            if (Op(i.Op) != Op::LoadField || Dsp.Registers.Slot[i.Dst] != NoReg) continue;
            const auto index = Index(i);
            if (!index) continue;
            const uint32_t physical = CachedReg(i.Imm, *index);
            if (!physical) continue;
            bool safe = true;
            for (uint32_t k = pc + 1; k < Alloc[i.Dst].End; ++k)
                if (Op(code[k].Op) == Op::StoreField && code[k].Imm == i.Imm && Index(code[k]) == index) safe = false;
            if (safe) Alloc[i.Dst].Physical = physical;
        }
        CallMask.assign(code.size(), 0);
        for (const auto &a : Alloc)
            if (a.Physical && a.Type == Nature::Real && (a.Physical < 8 || a.Physical >= 16))
                for (uint32_t pc = a.Begin; pc <= a.End && pc < code.size(); ++pc) CallMask[pc] |= 1u << a.Physical;
        Callee.clear();
        for (const auto &c : Channels)
            if (c.Physical >= 19) Callee.emplace_back(true, c.Physical);
        for (bool integer : {true, false})
            for (uint32_t reg = integer ? integerBegin : 8; reg < (integer ? 29u : 16u); ++reg)
                if (std::ranges::any_of(Alloc, [&](const Allocation &a) { return a.Physical == reg && (a.Type == Nature::Int) == integer; }))
                    Callee.emplace_back(integer, reg);
        StackBytes = (uint32_t(Callee.size()) * 8 + 15) & ~15u;
    }
    void Move(bool integer, uint32_t from, uint32_t to) {
        if (from == to) return;
        Word(integer ? (0x2a0003e0 | from << 16 | to) : (0x1e604000 | from << 5 | to));
    }
    void Home(Reg r, bool store, uint32_t reg) {
        const auto &a = Alloc[r];
        const bool integer = a.Type == Nature::Int;
        if (r < Dsp.Plan.Regs) Storage(r, store, integer, reg);
        else {
            const auto &f = Cached[r - Dsp.Plan.Regs];
            ScalarMem(store, integer, reg, 1, (Dsp.FieldAt[f.Field] + f.Index) * 8);
        }
    }
    void Boundary(bool store) {
        for (Reg r = 0; r < Alloc.size(); ++r) {
            const auto &a = Alloc[r];
            if (a.Physical && (r >= Dsp.Plan.Regs || Dsp.Registers.Slot[r] != NoReg) && (!store || a.Written)) Home(r, store, a.Physical);
        }
    }

    explicit Emit(const Instance &dsp) : Dsp(dsp), Def(dsp.Plan.Regs), Operands(dsp.Plan.Operands) {
        for (const auto &band : dsp.Plan.Bands)
            for (const Instr &i : band)
                if (i.Dst != NoReg) Def[i.Dst] = &i;
        for (size_t b = 0; b < Prepared.size(); ++b) {
            auto &code = Prepared[b];
            if (b == size_t(faustlens::Band::Init)) {
                code = dsp.Plan.Bands[b];
                continue;
            }
            std::vector<Reg> alias(dsp.Plan.Regs, NoReg);
            std::map<std::pair<uint32_t, uint32_t>, Reg> fields;
            for (Instr i : dsp.Plan.Bands[b]) {
                const auto original = dsp.Plan.Args(i);
                i.Args = uint32_t(Operands.size());
                for (Reg r : original) Operands.push_back(alias[r] == NoReg ? r : alias[r]);
                const Op op = Op(i.Op);
                if (op == Op::GuardBegin || op == Op::GuardEnd) fields.clear();
                if ((op == Op::LoadField || op == Op::StoreField) && dsp.Plan.Fields[i.Imm].Kind != FieldKind::Widget) {
                    if (const auto index = Index(i)) {
                        const auto key = std::pair{i.Imm, *index};
                        if (op == Op::StoreField) {
                            const Reg value = Args(i).back();
                            if (Dsp.Registers.Types[value] == dsp.Plan.Fields[i.Imm].Nature) fields[key] = value;
                            else fields.erase(key);
                        } else {
                            const auto prior = fields.find(key);
                            if (prior != fields.end() && dsp.Registers.Slot[i.Dst] == NoReg) {
                                alias[i.Dst] = prior->second;
                                continue;
                            }
                            fields[key] = i.Dst;
                        }
                    } else if (op == Op::StoreField) std::erase_if(fields, [&](const auto &entry) { return entry.first.first == i.Imm; });
                }
                code.push_back(i);
            }
            std::vector<std::vector<Instr>> before(code.size());
            std::vector<bool> moved(code.size());
            std::vector<uint32_t> defined(dsp.Plan.Regs);
            std::vector<uint32_t> fieldBarrier(dsp.Plan.Fields.size());
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> access;
            uint32_t barrier = 0;
            for (uint32_t pc = 0; pc < code.size(); ++pc) {
                const Instr &i = code[pc];
                const Op op = Op(i.Op);
                if (op == Op::GuardBegin || op == Op::GuardEnd || op == Op::FFun) barrier = pc + 1;
                if (i.Dst != NoReg) defined[i.Dst] = pc + 1;
                if (op != Op::LoadField && op != Op::StoreField) continue;
                const auto index = Index(i);
                if (!index || dsp.Plan.Fields[i.Imm].Kind == FieldKind::Widget) {
                    fieldBarrier[i.Imm] = pc + 1;
                    continue;
                }
                const auto key = std::pair{i.Imm, *index};
                if (op == Op::StoreField) {
                    uint32_t earliest = std::max({barrier, fieldBarrier[i.Imm], access[key]});
                    for (Reg r : Args(i)) earliest = std::max(earliest, defined[r]);
                    if (earliest < pc) {
                        before[earliest].push_back(i);
                        moved[pc] = true;
                    }
                }
                access[key] = pc + 1;
            }
            std::vector<Instr> scheduled;
            scheduled.reserve(code.size());
            for (size_t pc = 0; pc < code.size(); ++pc) {
                scheduled.insert(scheduled.end(), before[pc].begin(), before[pc].end());
                if (!moved[pc]) scheduled.push_back(code[pc]);
            }
            code = std::move(scheduled);
        }
    }

    void Word(uint32_t word) { Words.push_back(word); }
    size_t Branch(uint32_t op, bool distant = false) {
        if (distant) {
            Word((op ^ ((op & 0xff000000) == 0x54000000 ? 1u : 1u << 24)) | 2u << 5);
            op = 0x14000000;
        }
        const size_t at = Words.size();
        Word(op);
        return at;
    }
    void Target(size_t at, size_t to) {
        const int64_t offset = int64_t(to) - int64_t(at);
        const bool distant = Words[at] == 0x14000000;
        const uint32_t bits = distant ? 26 : 19;
        if (offset < -(1 << (bits - 1)) || offset >= (1 << (bits - 1))) throw std::runtime_error("native branch exceeds range");
        Words[at] |= (uint32_t(offset) & ((1u << bits) - 1)) << (distant ? 0 : 5);
    }
    void Back(uint32_t op, size_t to) { Target(Branch(op, Words.size() - to >= (1u << 18)), to); }
    void Imm(uint64_t value, uint32_t reg = 9) {
        Word(0xd2800000 | uint32_t(value & 0xffff) << 5 | reg); // movz
        for (uint32_t k = 1; k < 4; ++k)
            if (const uint32_t bits = uint32_t((value >> (k * 16)) & 0xffff)) Word(0xf2800000 | k << 21 | bits << 5 | reg); // movk
    }
    void Mem(uint32_t op, uint32_t reg, uint32_t base, uint32_t bytes, uint32_t scale = 8) {
        if (bytes % scale || bytes / scale >= 4096) {
            Imm(bytes, 17);
            Word(0x8b110000 | base << 5 | 17); // add x17, base, x17
            base = 17;
            bytes = 0;
        }
        Word(op | (bytes / scale) << 10 | base << 5 | reg);
    }
    void ScalarMem(bool store, bool integer, uint32_t reg, uint32_t base, uint32_t bytes) {
        Mem(integer ? (store ? 0xb9000000 : 0xb9400000) : (store ? 0xfd000000 : 0xfd400000), reg, base, bytes, integer ? 4 : 8);
    }
    void Value(Reg r, bool store, bool integer = false, uint32_t reg = 0) {
        if (const uint32_t physical = Alloc[r].Physical) {
            Move(integer, store ? reg : physical, store ? physical : reg);
            return;
        }
        Storage(r, store, integer, reg);
    }
    void Storage(Reg r, bool store, bool integer, uint32_t reg) {
        const uint32_t slot = Dsp.Registers.Slot.at(r);
        const uint32_t base = slot == NoReg ? 7 : 0;
        const uint32_t bytes = (slot == NoReg ? r : slot) * 8;
        ScalarMem(store, integer, reg, base, bytes);
    }
    void Real(Reg r, uint32_t reg = 0) {
        if (Dsp.Registers.Types[r] == Nature::Int) {
            Value(r, false, true, 9);
            Word(0x1e620120 | reg); // scvtf dN, w9
        } else Value(r, false, false, reg);
    }
    void Integer(Reg r, uint32_t reg = 9) {
        if (Dsp.Registers.Types[r] == Nature::Real) {
            Value(r, false, false, 2);
            Word(0x1e780040 | reg); // fcvtzs wN, d2
        } else Value(r, false, true, reg);
    }
    uint32_t RealReg(Reg r, uint32_t scratch = 0) {
        if (Alloc[r].Physical && Dsp.Registers.Types[r] == Nature::Real) return Alloc[r].Physical;
        Real(r, scratch);
        return scratch;
    }
    uint32_t IntReg(Reg r, uint32_t scratch = 9) {
        if (Alloc[r].Physical && Dsp.Registers.Types[r] == Nature::Int) return Alloc[r].Physical;
        Integer(r, scratch);
        return scratch;
    }
    uint32_t Dst(Reg r, uint32_t scratch = 0) const { return Alloc[r].Physical ? Alloc[r].Physical : scratch; }
    void Binary(uint32_t op, uint32_t dst, uint32_t x, uint32_t y) { Word(op | y << 16 | x << 5 | dst); }
    void Field(const Instr &i, std::span<const Reg> args, bool store) {
        const auto &f = Dsp.Plan.Fields.at(i.Imm);
        if (const auto index = Index(i)) {
            if (const uint32_t physical = CachedReg(i.Imm, *index)) {
                const bool integer = f.Nature == Nature::Int;
                if (store) Move(integer, integer ? IntReg(args.back()) : RealReg(args.back()), physical);
                else Value(i.Dst, true, integer, physical);
                return;
            }
        }
        const size_t count = store ? 2 : 1;
        if (args.size() != count && args.size() != count - 1) throw std::runtime_error("invalid native field operands");
        uint32_t base = 1, offset = Dsp.FieldAt[i.Imm] * 8;
        if (const auto index = Index(i)) offset += *index * 8;
        else {
            Imm(offset, 11);
            Word(0x8b0b002b); // add x11, x1, x11
            Integer(args[0]);
            Imm(f.Extent - 1, 10);
            Word(0x6b0a013f); // cmp w9, w10
            Word(0x1a8a3129); // csel w9, w9, w10, lo
            Word(0x8b090d6b); // add x11, x11, x9, lsl #3
            base = 11;
            offset = 0;
        }
        const bool integer = f.Nature == Nature::Int;
        const uint32_t reg = store ? (integer ? IntReg(args.back()) : RealReg(args.back())) : Dst(i.Dst, integer ? 9 : 0);
        ScalarMem(store, integer, reg, base, offset);
        if (!store) Value(i.Dst, true, integer, reg);
    }

    void CallRegisters(bool store) {
        uint32_t slot = 0;
        for (uint32_t mask = CallMask[Pc]; mask; mask &= mask - 1) Mem(store ? 0xfd000000 : 0xfd400000, std::countr_zero(mask), 31, 80 + slot++ * 8);
    }
    void SaveCall() {
        Word(0xd10403ff); // sub sp, sp, #256
        CallRegisters(true);
        Word(0xa90007e0);
        Word(0xa9010fe2);
        Word(0xa90217e4);
        Word(0xa9031fe6); // stp x6, x7, [sp, #48]
        Mem(0xf9000000, 30, 31, 64);
        Mem(0xf9000000, 8, 31, 72);
        Mem(0xf9000000, 15, 31, 248);
    }
    void RestoreCall() {
        Mem(0xf9400000, 30, 31, 64);
        Mem(0xf9400000, 8, 31, 72);
        Mem(0xf9400000, 15, 31, 248);
        Word(0xa94007e0);
        Word(0xa9410fe2);
        Word(0xa94217e4);
        Word(0xa9431fe6); // ldp x6, x7, [sp, #48]
        CallRegisters(false);
        Word(0x910403ff); // add sp, sp, #256
    }
    void Invoke(uintptr_t fn, bool integer = false) {
        Imm(fn, 16);
        Word(0xd63f0200); // blr x16
        if (integer) Word(0x2a0003e9); // mov w9, w0
        RestoreCall();
    }
    template<class F> void Call(F fn) {
        SaveCall();
        Invoke(reinterpret_cast<uintptr_t>(fn));
    }
    void Result(const Instr &i, bool integer) {
        if (integer && i.Nature == Nature::Real) Word(0x1e620120); // scvtf d0, w9
        if (!integer && i.Nature == Nature::Int) Word(0x1e780009); // fcvtzs w9, d0
        Value(i.Dst, true, i.Nature == Nature::Int, i.Nature == Nature::Int ? 9 : 0);
    }
    void Extended(const Instr &i, std::span<const Reg> args) {
        const Ext form = Ext(i.Form);
        const bool integer = Dsp.Registers.Types[args[0]] == Nature::Int;
        if (form == Ext::Pow) {
            if (const auto k = PowerExponent(Def[args[1]])) {
                if (integer) {
                    Integer(args[0]);
                    Word(0x2a0903ea); // mov w10, w9
                    if (*k == 0) Imm(1);
                    for (int n = 1; n < *k; ++n) Word(0x1b0a7d29); // mul w9, w9, w10
                } else {
                    Real(args[0]);
                    Word(0x1e604001); // fmov d1, d0
                    if (*k == 0) Word(0x1e6e1000); // fmov d0, #1.0
                    for (int n = 1; n < *k; ++n) Word(0x1e610800); // fmul d0, d0, d1
                }
                Result(i, integer);
                return;
            }
        }
        if (integer && (form == Ext::Abs || form == Ext::Min || form == Ext::Max)) {
            Integer(args[0]);
            if (form == Ext::Abs) {
                Word(0x7100013f); // cmp w9, #0
                Word(0x5a895529); // cneg w9, w9, mi
            } else {
                Integer(args[1], 10);
                Word(0x6b0a013f);
                Word(form == Ext::Min ? 0x1a8ab129 : 0x1a8ac129); // csel signed min/max
            }
            Result(i, true);
            return;
        }
        uint32_t unaryOp = 0;
        switch (form) {
            case Ext::Abs: unaryOp = 0x1e60c000; break;
            case Ext::Ceil: unaryOp = 0x1e64c000; break;
            case Ext::Floor: unaryOp = 0x1e654000; break;
            case Ext::Rint: unaryOp = 0x1e674000; break;
            case Ext::Round: unaryOp = 0x1e664000; break;
            case Ext::Sqrt: unaryOp = 0x1e61c000; break;
            default: break;
        }
        if (unaryOp || form == Ext::Min || form == Ext::Max) {
            const uint32_t x = RealReg(args[0]);
            const uint32_t dst = i.Nature == Nature::Real ? Dst(i.Dst) : 0;
            if (unaryOp) Word(unaryOp | x << 5 | dst);
            else {
                const uint32_t y = RealReg(args[1], 1);
                Binary(0x1e602000, 0, x, y);
                // Preserve the first operand for equal and unordered comparisons.
                Binary(form == Ext::Min ? 0x1e60cc00 : 0x1e604c00, dst, y, x);
            }
            if (i.Nature == Nature::Real) Value(i.Dst, true, false, dst);
            else Result(i, false);
            return;
        }
        Real(args[0]);
        if (args.size() > 1) Real(args[1], 1);
        double (*unary)(double) = nullptr;
        double (*binary)(double, double) = nullptr;
        switch (form) {
            case Ext::Acos: unary = std::acos; break;
            case Ext::Acosh: unary = std::acosh; break;
            case Ext::Asin: unary = std::asin; break;
            case Ext::Asinh: unary = std::asinh; break;
            case Ext::Atan: unary = std::atan; break;
            case Ext::Atan2: binary = std::atan2; break;
            case Ext::Atanh: unary = std::atanh; break;
            case Ext::Cos: unary = std::cos; break;
            case Ext::Cosh: unary = std::cosh; break;
            case Ext::Exp: unary = std::exp; break;
            case Ext::Fmod: binary = std::fmod; break;
            case Ext::Log: unary = std::log; break;
            case Ext::Log10: unary = std::log10; break;
            case Ext::Pow: binary = std::pow; break;
            case Ext::Remainder: binary = std::remainder; break;
            case Ext::Sin: unary = std::sin; break;
            case Ext::Sinh: unary = std::sinh; break;
            case Ext::Tan: unary = std::tan; break;
            case Ext::Tanh: unary = std::tanh; break;
            default: throw std::runtime_error("invalid native math operation");
        }
        if (unary) Call(unary);
        if (binary) Call(binary);
        Result(i, false);
    }
    void Foreign(const Instr &i, std::span<const Reg> args) {
        const auto *symbol = Dsp.Symbol.at(i.Imm);
        const bool integer = i.Nature == Nature::Int;
        if (!symbol) {
            Imm(0);
            Word(0x9e670120); // fmov d0, x9
        } else if (symbol->Provides == Symbol::Runtime::SampleRate) {
            Mem(0xfd400000, 0, 6, 0);
            if (integer) Word(0x1e780009);
        } else if (symbol->Provides == Symbol::Runtime::BlockSize) {
            Mem(0xb9400000, 9, 6, 8, 4);
            if (!integer) Word(0x1e620120);
        } else if (symbol->Kind != ForeignKind::Function) {
            Imm(reinterpret_cast<uintptr_t>(symbol->Addr), 11);
            Mem(integer ? 0xb9400000 : 0xfd400000, integer ? 9 : 0, 11, 0);
        } else {
            SaveCall();
            uint32_t ints = 0, reals = 0;
            for (size_t k = 0; k < args.size(); ++k) {
                if (symbol->Args[k] == Nature::Int) Integer(args[k], 12 + ints++);
                else Real(args[k], reals++);
            }
            for (uint32_t k = 0; k < ints; ++k) Word(0x2a0003e0 | (12 + k) << 16 | k); // mov wN, w(12+N)
            Invoke(reinterpret_cast<uintptr_t>(symbol->Fn), integer);
        }
        Value(i.Dst, true, integer, integer ? 9 : 0);
    }
    void Sound(const Instr &i, std::span<const Reg> args) {
        SaveCall();
        for (uint32_t k = 0; k < args.size(); ++k) Integer(args[k], 12 + k);
        Mem(0xf9400000, 0, 6, 24);
        Imm(i.Imm, 1);
        for (uint32_t k = 0; k < args.size(); ++k) Word(0x2a0003e0 | (12 + k) << 16 | (2 + k));
        const bool integer = Op(i.Op) != Op::SoundfileRead;
        Invoke(
            Op(i.Op) == Op::SoundfileLength   ? reinterpret_cast<uintptr_t>(&SoundLength) :
                Op(i.Op) == Op::SoundfileRate ? reinterpret_cast<uintptr_t>(&SoundRate) :
                                                reinterpret_cast<uintptr_t>(&SoundRead),
            integer
        );
        Value(i.Dst, true, integer, integer ? 9 : 0);
    }
    void Instruction(const Instr &i, std::vector<size_t> &guards) {
        const auto args = Args(i);
        switch (Op(i.Op)) {
            case Op::ConstReal:
                Imm(uint64_t(i.Imm) | uint64_t(i.Aux) << 32);
                Word(0x9e670120); // fmov d0, x9
                Value(i.Dst, true);
                return;
            case Op::ConstInt:
                Imm(i.Imm);
                Value(i.Dst, true, true, 9);
                return;
            case Op::Input:
            case Op::Output: {
                const bool output = Op(i.Op) == Op::Output;
                const uint32_t reg = output ? RealReg(args[0]) : Dst(i.Dst), base = output ? 3 : 2;
                if (!output) Word(0x9e6703e0 | reg); // fmov dN, xzr
                uint32_t pointer = ChannelReg(output, i.Imm);
                std::optional<size_t> absent;
                if (!pointer) {
                    absent = Branch(0xb4000000 | base);
                    pointer = 9;
                    Mem(0xf9400000, pointer, base, i.Imm * 8);
                }
                const size_t missing = Branch(0xb4000000 | pointer);
                Word((output ? 0xfc257800 : 0xfc657800) | pointer << 5 | reg); // str/ldr dN, [pointer, x5, lsl #3]
                if (absent) Target(*absent, Words.size());
                Target(missing, Words.size());
                if (!output) Value(i.Dst, true, false, reg);
                return;
            }
            case Op::BinOp: {
                const bool integer = Dsp.Registers.Types[args[0]] == Nature::Int;
                const uint32_t x = integer ? IntReg(args[0]) : RealReg(args[0]);
                const uint32_t y = integer ? IntReg(args[1], 10) : RealReg(args[1], 1);
                const auto form = BinOpCode(i.Form);
                if (form >= BinOpCode::GT && form <= BinOpCode::NE) {
                    uint32_t condition = 0;
                    switch (form) {
                        case BinOpCode::GT: condition = 12; break;
                        case BinOpCode::LT: condition = integer ? 11 : 4; break;
                        case BinOpCode::GE: condition = 10; break;
                        case BinOpCode::LE: condition = integer ? 13 : 9; break;
                        case BinOpCode::EQ: condition = 0; break;
                        case BinOpCode::NE: condition = 1; break;
                        default: break;
                    }
                    const uint32_t dst = Dst(i.Dst, 9);
                    Binary(integer ? 0x6b00001f : 0x1e602000, 0, x, y); // cmp or fcmp
                    Word(0x1a9f07e0 | (condition ^ 1) << 12 | dst); // cset
                    Value(i.Dst, true, true, dst);
                    return;
                }
                uint32_t op = 0;
                switch (form) {
                    case BinOpCode::Add: op = integer ? 0x0b000000 : 0x1e602800; break;
                    case BinOpCode::Sub: op = integer ? 0x4b000000 : 0x1e603800; break;
                    case BinOpCode::Mul: op = integer ? 0x1b007c00 : 0x1e600800; break;
                    case BinOpCode::Div: op = integer ? 0x1ac00c00 : 0x1e601800; break;
                    case BinOpCode::Rem:
                        if (integer) {
                            Binary(0x1ac00c00, 12, x, y); // sdiv
                            op = 0x1b008000 | x << 10; // msub
                        } else {
                            Move(false, x, 0);
                            Move(false, y, 1);
                            Call(static_cast<double (*)(double, double)>(std::fmod));
                            Result(i, false);
                            return;
                        }
                        break;
                    case BinOpCode::LeftShift:
                        if (integer) op = 0x1ac02000;
                        break;
                    case BinOpCode::RightShift:
                        if (integer) op = 0x1ac02800;
                        break;
                    case BinOpCode::LRightShift:
                        if (integer) op = 0x1ac02400;
                        break;
                    case BinOpCode::AND:
                        if (integer) op = 0x0a000000;
                        break;
                    case BinOpCode::OR:
                        if (integer) op = 0x2a000000;
                        break;
                    case BinOpCode::XOR:
                        if (integer) op = 0x4a000000;
                        break;
                    default: break;
                }
                if (!op) break;
                const uint32_t dst = integer == (i.Nature == Nature::Int) ? Dst(i.Dst, integer ? 9 : 0) : (integer ? 9 : 0);
                Binary(op, dst, form == BinOpCode::Rem ? 12 : x, y);
                if (integer && i.Nature == Nature::Real) {
                    Word(0x1e620000 | dst << 5);
                    Value(i.Dst, true);
                } else Value(i.Dst, true, integer, dst);
                return;
            }
            case Op::FConst:
            case Op::FVar:
            case Op::FFun: Foreign(i, args); return;
            case Op::Extended: Extended(i, args); return;
            case Op::SoundfileLength:
            case Op::SoundfileRate:
            case Op::SoundfileRead: Sound(i, args); return;
            case Op::Select2:
            case Op::Select3: {
                const bool integer = i.Nature == Nature::Int;
                const uint32_t value = integer ? 10 : 0, candidate = integer ? 11 : 1;
                Integer(args[0]);
                Value(args.back(), false, integer, value);
                for (size_t k = args.size() - 2; k > 0; --k) {
                    Value(args[k], false, integer, candidate);
                    Word(0x7100013f | uint32_t(k - 1) << 10); // cmp w9, #index
                    Binary(integer ? 0x1a800000 : 0x1e600c00, value, candidate, value); // csel/fcsel eq
                }
                Value(i.Dst, true, integer, value);
                return;
            }
            case Op::LoopBegin:
                Imm(0);
                Value(i.Dst, true, true, 9);
                Imm(i.Imm, 10);
                {
                    const size_t exit = Branch(0x3400000a, true);
                    Loops.push_back({Words.size(), exit, i.Dst, i.Imm});
                }
                return;
            case Op::LoopEnd: {
                if (Loops.empty()) throw std::runtime_error("unmatched native loop end");
                const auto loop = Loops.back();
                Integer(loop.Counter);
                Word(0x11000529); // add w9, w9, #1
                Value(loop.Counter, true, true, 9);
                Imm(loop.Bound, 10);
                Word(0x6b0a013f);
                Back(0x54000003, loop.Body); // b.lo
                Target(loop.Exit, Words.size());
                Loops.pop_back();
                return;
            }
            case Op::FloatCast:
                Real(args[0]);
                Value(i.Dst, true);
                return;
            case Op::IntCast:
                Integer(args[0]);
                Value(i.Dst, true, true, 9);
                return;
            case Op::LoadField: Field(i, args, false); return;
            case Op::StoreField: Field(i, args, true); return;
            case Op::GuardBegin:
                Integer(args[0]);
                guards.push_back(Branch(0x34000009, true));
                return;
            case Op::GuardEnd:
                if (guards.empty()) throw std::runtime_error("unmatched native guard end");
                Target(guards.back(), Words.size());
                guards.pop_back();
                return;
            default: break;
        }
        throw std::runtime_error(std::format("native does not support {} form {}", OpName(Op(i.Op)), i.Form));
    }
    size_t Band(faustlens::Band band) {
        const auto &code = Prepared[size_t(band)];
        const size_t entry = Words.size();
        if (code.empty()) {
            Word(0xd65f03c0); // ret
            return entry;
        }
        Allocate(code, band);
        const size_t empty = Branch(0x34000004, true); // cbz w4
        Word(0xaa0503e6); // mov x6, x5
        Mem(0xf9400000, 7, 6, 16);
        Word(0x2a1f03e5); // mov w5, wzr
        if (StackBytes) Word(0xd10003ff | StackBytes << 10);
        for (uint32_t k = 0; k < Callee.size(); ++k) Mem(Callee[k].first ? 0xf9000000 : 0xfd000000, Callee[k].second, 31, k * 8);
        Boundary(false);
        for (const auto &c : Channels) {
            const uint32_t base = c.Output ? 3 : 2;
            Imm(0, c.Physical);
            const size_t absent = Branch(0xb4000000 | base);
            Mem(0xf9400000, c.Physical, base, c.Index * 8);
            Target(absent, Words.size());
        }
        const size_t loop = Words.size();
        std::vector<size_t> guards;
        Pc = 0;
        for (const Instr &i : code) {
            Instruction(i, guards);
            ++Pc;
        }
        if (!guards.empty()) throw std::runtime_error("unmatched native guard begin");
        Word(0x110004a5); // add w5, w5, #1
        Word(0x6b0400bf); // cmp w5, w4
        Back(0x5400000b, loop); // b.lt
        Boundary(true);
        for (uint32_t k = 0; k < Callee.size(); ++k) Mem(Callee[k].first ? 0xf9400000 : 0xfd400000, Callee[k].second, 31, k * 8);
        if (StackBytes) Word(0x910003ff | StackBytes << 10);
        Target(empty, Words.size());
        Word(0xd65f03c0); // ret
        return entry;
    }
};

} // namespace

std::expected<std::unique_ptr<Native>, std::string> Native::Compile(const faustlens::Plan &p, const UiNode &ui, const faustlens::Registry &reg) {
    try {
        auto native = std::unique_ptr<Native>(new Native(p, ui, reg));
        native->Scratch.resize(p.Regs);
        Emit emit{*native};
        std::array<size_t, 3> offsets;
        for (size_t b = 0; b < offsets.size(); ++b) offsets[b] = emit.Band(Band(b));
        auto code = Executable::Publish(emit.Words);
        if (!code) return std::unexpected(code.error());
        native->Code = std::move(*code);
        for (size_t b = 0; b < offsets.size(); ++b)
            native->Entries[b] = std::bit_cast<Entry>(static_cast<const uint32_t *>(native->Code->Address()) + offsets[b]);
        return native;
    } catch (const std::runtime_error &e) { return std::unexpected(e.what()); }
}

void Native::Execute(Band band, int32_t frames, const double *const *in, double *const *out) {
    static_assert(offsetof(Parameters, SampleRate) == 0 && offsetof(Parameters, Frames) == 8);
    static_assert(offsetof(Parameters, Scratch) == 16 && offsetof(Parameters, Owner) == 24);
    const Parameters parameters{SampleRate, Frames, Scratch.data(), this};
    Entries[size_t(band)](Values.data(), State.data(), in, out, frames, &parameters);
}

} // namespace faustlens
