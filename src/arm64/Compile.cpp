#include "arm64/Program.h"
#include "runtime/Math.h"

#include <algorithm>
#include <bit>
#include <format>
#include <map>
#include <stdexcept>

namespace faustlens::arm64 {
namespace {

struct Emit {
    static constexpr size_t LoopWords = 256; // Limit each duplicated loop to 1 KiB.
    static constexpr size_t CallLoopWords = 64; // Limit duplicated loops containing calls to 256 bytes.
    const Plan &Plan;
    const InstanceLayout &Layout;
    std::vector<Relocation> Relocations;
    std::vector<uint32_t> Words;
    struct LiteralUse {
        size_t Word;
        uint64_t Bits;
    };
    std::vector<LiteralUse> Literals;
    std::vector<const Instr *> Def;
    // Prepared BinOps with one operand encode an integer constant in Imm and the ARM64 operation in Aux.
    std::array<std::vector<Instr>, 3> Prepared;
    std::vector<Reg> Operands;
    struct Bounds {
        int32_t Lo = INT32_MIN, Hi = INT32_MAX;
        bool Absolute = false;
    };
    std::vector<Bounds> Ranges;

    bool NonnegativeRemainder(Reg r, uint32_t divisor) const { return Ranges[r].Lo >= 0 || (Ranges[r].Absolute && std::has_single_bit(divisor)); }

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
        bool Written = false, Load = true;
    };
    struct CachedField {
        uint32_t Field, Index;
    };
    std::vector<Allocation> Alloc;
    std::vector<CachedField> Cached;
    uint32_t Pc = 0, ControlEnd = 0;
    std::vector<uint32_t> CallMask;
    std::vector<Reg> CallValues;
    Reg CallInput = NoReg, CallOutput = NoReg;
    std::vector<std::pair<bool, uint32_t>> Callee;
    uint32_t StackBytes = 0, CallContext = 0, UsedContext = 0;
    // Values, state, input/output arrays, frame count/index, runtime parameters, and scratch storage.
    std::array<uint32_t, 8> Context;
    struct Channel {
        bool Output;
        uint32_t Index, Physical;
    };
    std::vector<Channel> Channels;
    bool Connected = false, Leaf = true, Advancing = false;
    struct MathTarget {
        uint32_t Index, Uses, Physical = 0;
    };
    std::vector<MathTarget> MathTargets;
    struct FieldBase {
        uint32_t Field, Physical;
    };
    std::vector<FieldBase> Bases;

    uint32_t BaseReg(uint32_t field) const {
        const auto it = std::ranges::find(Bases, field, &FieldBase::Field);
        return it == Bases.end() ? 0 : it->Physical;
    }
    uint32_t ChannelReg(bool output, uint32_t index) const {
        const auto it = std::ranges::find_if(Channels, [&](const Channel &c) { return c.Output == output && c.Index == index; });
        return it == Channels.end() ? 0 : it->Physical;
    }
    uint32_t CachedReg(uint32_t field, uint32_t index) const {
        const auto it = std::ranges::find_if(Cached, [&](const CachedField &f) { return f.Field == field && f.Index == index; });
        return it == Cached.end() ? 0 : Alloc[Plan.Regs + (it - Cached.begin())].Physical;
    }

    std::optional<uint32_t> Index(const Instr &i) const {
        const auto args = Args(i);
        if (args.size() == (Op(i.Op) == Op::StoreField ? 1u : 0u)) return 0;
        const Instr *def = Def[args[0]];
        if (!def || Op(def->Op) != Op::ConstInt) return {};
        return std::min(def->Imm, std::max(1u, Plan.Fields[i.Imm].Extent) - 1);
    }
    std::span<const Reg> ReadArgs(const Instr &i) const {
        const auto args = Args(i);
        const Op op = Op(i.Op);
        if ((op == Op::LoadField || op == Op::StoreField) && Index(i)) return op == Op::StoreField ? args.last(1) : args.first(0);
        if (op == Op::Extended && Ext(i.Form) == Ext::Pow && PowerExponent(Def[args[1]])) return args.first(1);
        return args;
    }
    static uint32_t UnaryOp(Ext form) {
        switch (form) {
            case Ext::Abs: return 0x1e60c000;
            case Ext::Ceil: return 0x1e64c000;
            case Ext::Floor: return 0x1e654000;
            case Ext::Rint: return 0x1e674000;
            case Ext::Round: return 0x1e664000;
            case Ext::Sqrt: return 0x1e61c000;
            default: return 0;
        }
    }
    bool Calls(const Instr &i) const {
        switch (Op(i.Op)) {
            case Op::BinOp: return i.ArgCount == 2 && BinOpCode(i.Form) == BinOpCode::Rem && Layout.Registers.Types[Args(i)[0]] == Nature::Real;
            case Op::Extended: {
                const auto form = Ext(i.Form);
                return !UnaryOp(form) && form != Ext::Min && form != Ext::Max && (form != Ext::Pow || !PowerExponent(Def[Args(i)[1]]));
            }
            case Op::FConst:
            case Op::FVar:
            case Op::FFun: return Plan.Foreign.at(i.Imm).Kind == ForeignKind::Function;
            case Op::SoundfileLength:
            case Op::SoundfileRate:
            case Op::SoundfileRead: return true;
            default: return false;
        }
    }
    uint32_t Spare(uint32_t begin) {
        for (uint32_t reg = 28; reg >= begin; --reg)
            if (std::ranges::find(Callee, std::pair{true, reg}) == Callee.end()) {
                Callee.emplace_back(true, reg);
                return reg;
            }
        return 0;
    }
    void Allocate(std::span<const Instr> code, faustlens::Band band) {
        std::vector<uint32_t> calls(code.size() + 1);
        std::vector<uint32_t> callSites;
        for (uint32_t pc = 0; pc < code.size(); ++pc) {
            if (Calls(code[pc])) callSites.push_back(pc);
            calls[pc + 1] = uint32_t(callSites.size());
        }
        const auto callsAt = [&](uint32_t pc) { return calls[pc + 1] != calls[pc]; };
        const bool leaf = Leaf = calls.back() == 0;
        Context = {0, 1, 2, 3, leaf ? 4u : 19u, leaf ? 5u : 20u, 6, 7};
        const uint32_t channelBegin = leaf ? 19 : 21;
        Channels.clear();
        for (const Instr &i : code)
            if (Op(i.Op) == Op::Input || Op(i.Op) == Op::Output) {
                const bool output = Op(i.Op) == Op::Output;
                if (Channels.size() < 8 && !ChannelReg(output, i.Imm)) {
                    const auto k = uint32_t(Channels.size());
                    Channels.push_back({output, i.Imm, k == 0 ? 8u : k == 1 ? 15u : channelBegin + k - 2});
                }
            }
        const uint32_t integerBegin = channelBegin + uint32_t(Channels.size() > 2 ? Channels.size() - 2 : 0);
        Alloc.assign(Plan.Regs, {});
        Cached.clear();
        for (Reg r = 0; r < Plan.Regs; ++r) Alloc[r].Type = Layout.Registers.Types[r];
        std::vector<bool> eligible(Plan.Fields.size(), band == faustlens::Band::Sample);
        for (const Instr &i : code)
            if (Op(i.Op) == Op::LoadField || Op(i.Op) == Op::StoreField)
                if (!Index(i) || Plan.Fields[i.Imm].Kind == FieldKind::Widget) eligible[i.Imm] = false;
        struct LifetimeLoop {
            Reg Counter;
            uint32_t Begin;
            std::vector<Reg> LiveIn;
        };
        std::vector<LifetimeLoop> counters;
        uint32_t guards = 0;
        for (uint32_t pc = 0; pc < code.size(); ++pc) {
            const Instr &i = code[pc];
            const auto touch = [&](Reg r, bool write) {
                auto &a = Alloc[r];
                if (a.Begin == UINT32_MAX && write && band == faustlens::Band::Sample && pc < ControlEnd && !guards && counters.empty() &&
                    Op(i.Op) != Op::LoopBegin)
                    a.Load = false;
                if (!write)
                    for (auto &loop : counters)
                        if (a.Begin < loop.Begin) loop.LiveIn.push_back(r);
                a.Begin = std::min(a.Begin, pc);
                a.End = std::max(a.End, pc);
                a.Written |= write;
            };
            for (Reg r : ReadArgs(i)) touch(r, false);
            if (i.Dst != NoReg) touch(i.Dst, true);
            if (Op(i.Op) == Op::GuardBegin) ++guards;
            if (Op(i.Op) == Op::GuardEnd) --guards;
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
                    Alloc.push_back({0, uint32_t(code.size()), 0, Plan.Fields[i.Imm].Nature, Op(i.Op) == Op::StoreField});
                } else Alloc[Plan.Regs + (it - Cached.begin())].Written |= Op(i.Op) == Op::StoreField;
            }
        }
        if (!counters.empty()) throw std::runtime_error("unmatched native loop begin");
        for (Reg r : Layout.Registers.Persistent)
            if (Alloc[r].Begin != UINT32_MAX) {
                Alloc[r].Begin = 0;
                Alloc[r].End = uint32_t(code.size());
            }
        std::vector<Reg> order;
        for (Reg r = 0; r < Alloc.size(); ++r)
            if (Alloc[r].Begin != UINT32_MAX) order.push_back(r);
        std::ranges::stable_sort(order, {}, [&](Reg r) { return Alloc[r].Begin; });
        const auto preserved = [](uint32_t reg) { return reg >= 8 && reg < 16; };
        const auto reloadable = [&](Reg r) { return r < Plan.Regs && Alloc[r].Type == Nature::Real && !Alloc[r].Written && Layout.Registers.Slot[r] != NoReg; };
        const auto callBegin = [&](Reg r) {
            return std::min(Alloc[r].End, Alloc[r].Begin + (r < Plan.Regs && Layout.Registers.Slot[r] == NoReg && code[Alloc[r].Begin].Dst == r));
        };
        const bool weighted =
            !leaf && std::ranges::any_of(order, [&](Reg r) { return Alloc[r].Type == Nature::Real && !reloadable(r) && callBegin(r) < Alloc[r].End; });
        std::vector<uint32_t> savings;
        if (weighted) {
            savings.resize(Alloc.size());
            for (const Instr &i : code)
                for (Reg r : ReadArgs(i))
                    if (reloadable(r)) ++savings[r];
            // Each live volatile value needs a save and reload around a call.
            for (Reg r : order)
                if (!reloadable(r)) savings[r] = 2 * (calls[Alloc[r].End] - calls[callBegin(r)]);
        }
        for (Nature type : {Nature::Int, Nature::Real}) {
            std::vector<uint32_t> free;
            for (uint32_t reg = type == Nature::Int ? 29u : 32u; reg > (type == Nature::Int ? integerBegin : 3u);) free.push_back(--reg);
            // x14 and x16 are available when the entry contains no calls.
            if (type == Nature::Int && leaf) free.insert(free.end(), {16, 14});
            // Allocate d3-d7 and d16-d31 before callee-saved d8-d15.
            if (type == Nature::Real && leaf) std::rotate(free.begin(), free.begin() + 16, free.begin() + 24);
            std::vector<Reg> active;
            for (Reg r : order) {
                auto &a = Alloc[r];
                if (a.Type != type) continue;
                const bool reload = !leaf && reloadable(r);
                const bool across = weighted && type == Nature::Real && savings[r];
                const auto &definition = code[a.Begin];
                // Binary and extended operations read operands before writing their destination.
                const bool reuse = r < Plan.Regs && Layout.Registers.Slot[r] == NoReg && definition.Dst == r && !callsAt(a.Begin) &&
                    (Op(definition.Op) == Op::BinOp || Op(definition.Op) == Op::Extended);
                std::erase_if(active, [&](Reg source) {
                    const auto &previous = Alloc[source];
                    if (previous.End > a.Begin ||
                        (previous.End == a.Begin && (!reuse || std::ranges::find(ReadArgs(definition), source) == ReadArgs(definition).end())))
                        return false;
                    free.push_back(previous.Physical);
                    return true;
                });
                if (across && std::ranges::none_of(free, preserved)) {
                    auto victim = active.end();
                    for (auto it = active.begin(); it != active.end(); ++it)
                        if (reloadable(*it) && savings[*it] < savings[r] && (victim == active.end() || savings[*it] < savings[*victim])) victim = it;
                    if (victim != active.end()) {
                        free.push_back(Alloc[*victim].Physical);
                        Alloc[*victim].Physical = 0;
                        active.erase(victim);
                    }
                }
                if (free.empty() && !reload) {
                    auto far = std::ranges::max_element(active, {}, [&](Reg at) { return Alloc[at].End; });
                    if (far != active.end() && Alloc[*far].End > a.End) {
                        free.push_back(Alloc[*far].Physical);
                        Alloc[*far].Physical = 0;
                        active.erase(far);
                    }
                }
                auto physical = free.end();
                if (reload || (weighted && type == Nature::Real))
                    physical = std::ranges::find_if(free, [&](uint32_t reg) { return preserved(reg) == (reload || across); });
                if (physical == free.end() && !reload && !free.empty()) physical = std::prev(free.end());
                if (physical != free.end()) {
                    a.Physical = *physical;
                    free.erase(physical);
                    active.push_back(r);
                }
            }
        }
        for (uint32_t pc = 0; pc < code.size(); ++pc) {
            const Instr &i = code[pc];
            if (Op(i.Op) != Op::LoadField || Layout.Registers.Slot[i.Dst] != NoReg) continue;
            const auto index = Index(i);
            if (!index) continue;
            const uint32_t physical = CachedReg(i.Imm, *index);
            if (!physical) continue;
            bool safe = true;
            for (uint32_t k = pc + 1; k < Alloc[i.Dst].End && safe; ++k)
                safe = Op(code[k].Op) != Op::StoreField || code[k].Imm != i.Imm || Index(code[k]) != index;
            if (safe) Alloc[i.Dst].Physical = physical;
        }
        if (band == faustlens::Band::Sample && !Cached.empty() && code.size() <= LoopWords &&
            std::ranges::none_of(code, [](const Instr &i) { return Op(i.Op) == Op::GuardBegin || Op(i.Op) == Op::LoopBegin || Op(i.Op) == Op::FFun; }))
            Coalesce(std::vector<std::optional<Instr>>(code.begin(), code.end()), true);
        CallValues.assign(code.size() + 1, NoReg);
        const auto math = [&](uint32_t pc) { return Op(code[pc].Op) == Op::Extended && code[pc].Nature == Nature::Real && callsAt(pc); };
        const auto arithmetic = [&](const Instr &i) {
            return Op(i.Op) == Op::BinOp && i.Nature == Nature::Real && BinOpCode(i.Form) <= BinOpCode::Div &&
                Layout.Registers.Types[Args(i)[0]] == Nature::Real;
        };
        if (!leaf)
            for (uint32_t pc = 0; pc + 1 < code.size(); ++pc) {
                const auto &i = code[pc], &next = code[pc + 1];
                const Reg r = i.Dst;
                if (r == NoReg || Layout.Registers.Slot[r] != NoReg || Alloc[r].Begin != pc || Alloc[r].End != pc + 1) continue;
                const bool call = math(pc), nextCall = math(pc + 1);
                if (!(call || nextCall) || !(call || arithmetic(i)) || !(nextCall || arithmetic(next))) continue;
                const auto args = Args(next);
                // Loading the first operand into d0 would overwrite a forwarded second operand.
                if (args[0] != r && (nextCall || args[1] != r || !Alloc[args[0]].Physical)) continue;
                CallValues[pc + 1] = r;
            }
        CallMask.assign(code.size(), 0);
        if (!leaf)
            for (Reg r = 0; r < Alloc.size(); ++r) {
                const auto &a = Alloc[r];
                if (!a.Physical || a.Type != Nature::Real || (a.Physical >= 8 && a.Physical < 16)) continue;
                for (uint32_t k = calls[callBegin(r)]; k < calls[a.End]; ++k) CallMask[callSites[k]] |= 1u << a.Physical;
            }
        Callee.clear();
        if (!leaf) {
            Callee.emplace_back(true, Context[4]);
            Callee.emplace_back(true, Context[5]);
            Callee.emplace_back(true, 30);
        }
        for (const auto &c : Channels)
            if (c.Physical >= 19) Callee.emplace_back(true, c.Physical);
        uint32_t used[2] = {};
        for (const auto &a : Alloc) used[a.Type == Nature::Int] |= 1u << a.Physical;
        for (bool integer : {true, false})
            for (uint32_t reg = integer ? integerBegin : 8; reg < (integer ? 29u : 16u); ++reg)
                if (used[integer] & (1u << reg)) Callee.emplace_back(integer, reg);
        Bases.clear();
        if (band == faustlens::Band::Sample)
            for (const Instr &i : code) {
                if ((Op(i.Op) != Op::LoadField && Op(i.Op) != Op::StoreField) || Index(i) || BaseReg(i.Imm)) continue;
                if (const uint32_t reg = Spare(integerBegin)) Bases.push_back({i.Imm, reg});
                else break;
            }
        MathTargets.clear();
        CallContext = 0;
        uint32_t callSlots = 0;
        UsedContext = 0;
        for (Reg r = 0; r < Plan.Regs; ++r) {
            const auto &a = Alloc[r];
            if (a.Begin == UINT32_MAX) continue;
            if (Layout.Registers.Slot[r] != NoReg) {
                if (!a.Physical || a.Written) UsedContext |= 1u << 0;
            } else if (!a.Physical) UsedContext |= 1u << 7;
        }
        for (uint32_t pc = 0; pc < code.size(); ++pc) {
            const auto &i = code[pc];
            const Op op = Op(i.Op);
            if (callsAt(pc)) {
                callSlots = std::max(callSlots, uint32_t(std::popcount(CallMask[pc])));
                if (band == faustlens::Band::Sample && (op == Op::Extended || op == Op::BinOp)) {
                    const uint32_t index = op == Op::Extended ? i.Form : uint32_t(Ext::Fmod);
                    const auto target = std::ranges::find(MathTargets, index, &MathTarget::Index);
                    if (target == MathTargets.end()) MathTargets.push_back({index, 1});
                    else ++target->Uses;
                }
            }
            if (op == Op::LoadField || op == Op::StoreField) UsedContext |= 1u << 1;
            if ((op == Op::Input || op == Op::Output) && !ChannelReg(op == Op::Output, i.Imm)) UsedContext |= 1u << (op == Op::Output ? 3 : 2);
            if (op == Op::FConst || op == Op::FVar || op == Op::FFun || op == Op::SoundfileLength || op == Op::SoundfileRate || op == Op::SoundfileRead)
                UsedContext |= 1u << 6;
        }
        if (!leaf) {
            const auto preserve = [&](uint32_t &physical) {
                if (const uint32_t reg = Spare(integerBegin)) physical = reg;
                else CallContext |= 1u << physical;
            };
            for (auto &channel : Channels)
                if (channel.Physical < 19) preserve(channel.Physical);
            for (uint32_t reg = 0; reg < Context.size(); ++reg)
                if (UsedContext & (1u << reg)) {
                    // Published foreign bindings read runtime parameters from x6.
                    if (reg == 6) CallContext |= 1u << reg;
                    else preserve(Context[reg]);
                }
            std::ranges::stable_sort(MathTargets, std::greater{}, &MathTarget::Uses);
            size_t cached = 0;
            for (auto &target : MathTargets) {
                target.Physical = Spare(integerBegin);
                if (!target.Physical) break;
                ++cached;
            }
            MathTargets.resize(cached);
            callSlots += std::popcount(CallContext);
        }
        StackBytes = ((uint32_t(Callee.size()) + callSlots) * 8 + 15) & ~15u;
    }
    void Move(bool integer, uint32_t from, uint32_t to) {
        if (from == to) return;
        Word(integer ? (0x2a0003e0 | from << 16 | to) : (0x1e604000 | from << 5 | to));
    }
    struct Transfer {
        uint32_t Base, Bytes, Physical;
        bool Integer;
        bool operator==(const Transfer &) const = default;
    };
    std::vector<Transfer> Transfers(bool store) const {
        std::vector<Transfer> result;
        const auto append = [&](Reg r, uint32_t base, uint32_t bytes) {
            const auto &a = Alloc[r];
            if (a.Physical && (store ? a.Written : a.Load)) result.push_back({base, bytes, a.Physical, a.Type == Nature::Int});
        };
        for (uint32_t slot = 0; slot < Layout.Registers.Persistent.size(); ++slot) append(Layout.Registers.Persistent[slot], Context[0], slot * 8);
        for (uint32_t k = 0; k < Cached.size(); ++k) {
            const auto &f = Cached[k];
            append(Plan.Regs + k, Context[1], (Layout.FieldAt[f.Field] + f.Index) * 8);
        }
        return result;
    }
    void Boundary(bool store, std::span<const Transfer> values) {
        uint32_t cachedBase = NoReg, cachedOffset = 0;
        for (size_t k = 0; k < values.size(); ++k) {
            const auto &v = values[k];
            uint32_t base = v.Base, bytes = v.Bytes;
            if (bytes >= 4096u * (v.Integer ? 4 : 8)) {
                const uint32_t high = bytes & ~4095u;
                if (cachedBase != base || cachedOffset != high) {
                    AddOffset(base, high, 17);
                    cachedBase = base;
                    cachedOffset = high;
                }
                base = 17;
                bytes &= 4095;
            }
            if (!v.Integer && bytes < 512 && k + 1 < values.size()) {
                const auto &next = values[k + 1];
                if (!next.Integer && next.Base == v.Base && next.Bytes == v.Bytes + 8 && (store || next.Physical != v.Physical)) {
                    Word((store ? 0x6d000000 : 0x6d400000) | (bytes / 8) << 15 | next.Physical << 10 | base << 5 | v.Physical);
                    ++k;
                    continue;
                }
            }
            ScalarMem(store, v.Integer, v.Physical, base, bytes);
        }
    }

    void FoldInstructions() {
        std::vector<uint32_t> uses(Plan.Regs);
        for (const auto &code : Prepared)
            for (const Instr &i : code)
                for (Reg r : Args(i)) ++uses[r];
        for (auto &code : Prepared) {
            size_t size = 0;
            for (Instr i : code) {
                if (Op(i.Op) == Op::IntCast) {
                    i.Form = 0;
                    if (size) {
                        const Instr &mul = code[size - 1];
                        if (Op(mul.Op) == Op::BinOp && BinOpCode(mul.Form) == BinOpCode::Mul && mul.Nature == Nature::Real && mul.Dst == Args(i)[0] &&
                            uses[mul.Dst] == 1 && Layout.Registers.Slot[mul.Dst] == NoReg) {
                            const auto args = Args(mul);
                            for (size_t k = 0; k < 2; ++k) {
                                const Instr *constant = Def[args[k]];
                                if (!constant || Op(constant->Op) != Op::ConstReal || Layout.Registers.Types[args[1 - k]] != Nature::Real) continue;
                                const uint64_t bits = uint64_t(constant->Imm) | uint64_t(constant->Aux) << 32;
                                const uint64_t exponent = bits >> 52;
                                // Power-of-two scaling is exact for finite values whose integer conversion does not saturate.
                                if ((bits & ((1ull << 52) - 1)) || exponent <= 1023 || exponent > 1055) continue;
                                i.Form = uint8_t(exponent - 1023);
                                i.Args = uint32_t(Operands.size());
                                const Reg value = args[1 - k];
                                Operands.push_back(value);
                                --size;
                                break;
                            }
                        }
                    }
                }
                if (size && Op(i.Op) == Op::BinOp && i.Nature == Nature::Int && BinOpCode(i.Form) == BinOpCode::AND) {
                    const auto args = Args(i);
                    const auto mask = Ranges[args[1]];
                    const Instr &offset = code[size - 1];
                    if (mask.Lo == mask.Hi && mask.Lo > 0 && std::has_single_bit(uint32_t(mask.Lo) + 1) && Op(offset.Op) == Op::BinOp &&
                        offset.Nature == Nature::Int && offset.Dst == args[0] && uses[offset.Dst] == 1 && Layout.Registers.Slot[offset.Dst] == NoReg &&
                        (BinOpCode(offset.Form) == BinOpCode::Add || BinOpCode(offset.Form) == BinOpCode::Sub)) {
                        const auto operands = Args(offset);
                        const auto amount = Ranges[operands[1]];
                        if (Layout.Registers.Types[operands[0]] == Nature::Int && amount.Lo == amount.Hi && amount.Lo == (mask.Lo / 2 + 1)) {
                            constexpr size_t searchLimit = 32; // Bound compilation work per masked offset.
                            const size_t first = size > searchLimit ? size - searchLimit : 0;
                            for (size_t k = size - 1; k > first;) {
                                const Instr &previous = code[--k];
                                const Op op = Op(previous.Op);
                                if (op == Op::GuardBegin || op == Op::GuardEnd || op == Op::LoopBegin || op == Op::LoopEnd || previous.Dst == operands[0])
                                    break;
                                if (op != Op::BinOp || previous.Nature != Nature::Int || BinOpCode(previous.Form) != BinOpCode::AND) continue;
                                const auto prior = Args(previous);
                                if (prior[0] != operands[0] || Ranges[prior[1]].Lo != mask.Lo || Ranges[prior[1]].Hi != mask.Hi) continue;
                                if (std::ranges::any_of(std::span(code).subspan(k + 1, size - k - 1), [&](const Instr &other) {
                                        return other.Dst == previous.Dst;
                                    }))
                                    break;
                                // Adding or subtracting half a power-of-two modulus toggles its highest retained bit.
                                const Reg value = previous.Dst, constant = operands[1];
                                i.Form = uint8_t(BinOpCode::XOR);
                                i.Args = uint32_t(Operands.size());
                                Operands.insert(Operands.end(), {value, constant});
                                --size;
                                break;
                            }
                        }
                    }
                }
                code[size++] = i;
            }
            code.resize(size);
        }
    }

    void BoundIndices() {
        Ranges.resize(Plan.Regs);
        std::vector<uint8_t> definitions(Plan.Regs);
        for (const auto &code : Prepared) {
            int nested = 0;
            for (const Instr &i : code) {
                const Op op = Op(i.Op);
                if (op == Op::GuardBegin || op == Op::LoopBegin) ++nested;
                if (op == Op::GuardEnd || op == Op::LoopEnd) --nested;
                if (i.Dst != NoReg) definitions[i.Dst] = nested || definitions[i.Dst] ? 2 : 1;
            }
        }
        for (const auto &code : Prepared) {
            for (const Instr &i : code) {
                const Op op = Op(i.Op);
                // Skipped definitions retain earlier values.
                if (i.Dst == NoReg || i.Nature != Nature::Int || definitions[i.Dst] != 1) continue;
                auto &range = Ranges[i.Dst];
                const auto args = Args(i);
                if (op == Op::ConstInt) range = {int32_t(i.Imm), int32_t(i.Imm)};
                else if (op == Op::BinOp) {
                    const auto form = BinOpCode(i.Form);
                    if (form >= BinOpCode::GT && form <= BinOpCode::NE) range = {0, 1};
                    else if (form == BinOpCode::AND) {
                        for (Reg r : args)
                            if (Ranges[r].Lo >= 0) range = {0, std::min(range.Hi, Ranges[r].Hi)};
                    } else if ((form == BinOpCode::Div || form == BinOpCode::Rem) && Ranges[args[1]].Lo == Ranges[args[1]].Hi) {
                        const int32_t divisor = Ranges[args[1]].Lo;
                        const uint32_t magnitude = divisor < 0 ? 0u - uint32_t(divisor) : uint32_t(divisor);
                        const auto a = Ranges[args[0]];
                        if (form == BinOpCode::Div) {
                            if (!divisor) range = {0, 0};
                            else if (divisor != -1 || a.Lo != INT32_MIN) {
                                const int32_t lo = IntegerBinary(form, a.Lo, divisor), hi = IntegerBinary(form, a.Hi, divisor);
                                range = {std::min(lo, hi), std::max(lo, hi)};
                            }
                        } else if (magnitude) {
                            const int32_t limit = int32_t(magnitude - 1);
                            range = {NonnegativeRemainder(args[0], magnitude) ? 0 : -limit, limit};
                        } else range = Ranges[args[0]];
                    }
                } else if (op == Op::Extended && Ext(i.Form) == Ext::Abs && Layout.Registers.Types[args[0]] == Nature::Int) {
                    const auto a = Ranges[args[0]];
                    // Integer abs can produce INT32_MIN, which is divisible by every representable power of two.
                    range = a.Lo == INT32_MIN ? Bounds{INT32_MIN, INT32_MAX, true} : Bounds{0, std::max(std::abs(a.Lo), std::abs(a.Hi)), true};
                } else if (
                    op == Op::Extended && (Ext(i.Form) == Ext::Min || Ext(i.Form) == Ext::Max) && Layout.Registers.Types[args[0]] == Nature::Int &&
                    Layout.Registers.Types[args[1]] == Nature::Int
                ) {
                    const auto a = Ranges[args[0]], b = Ranges[args[1]];
                    if (Ext(i.Form) == Ext::Min) range = {std::min(a.Lo, b.Lo), std::min(a.Hi, b.Hi)};
                    else range = {std::max(a.Lo, b.Lo), std::max(a.Hi, b.Hi)};
                } else if (op == Op::Select2 || op == Op::Select3) {
                    range = Ranges[args[1]];
                    for (Reg r : args.subspan(2)) range = {std::min(range.Lo, Ranges[r].Lo), std::max(range.Hi, Ranges[r].Hi)};
                } else if (op == Op::IntCast && Layout.Registers.Types[args[0]] == Nature::Int) range = Ranges[args[0]];
            }
        }
    }

    std::vector<std::optional<Instr>> Steady(std::span<const Instr> code, uint32_t begin) {
        std::map<std::pair<uint32_t, uint32_t>, std::optional<int32_t>> fields;
        std::vector<bool> dynamic(Plan.Fields.size());
        for (const Instr &i : code) {
            const Op op = Op(i.Op);
            if (op == Op::GuardBegin || op == Op::LoopBegin || op == Op::FFun) return {};
            if (op != Op::StoreField || Plan.Fields[i.Imm].Nature != Nature::Int || Plan.Fields[i.Imm].Kind == FieldKind::Widget) continue;
            const auto index = Index(i);
            if (!index) {
                dynamic[i.Imm] = true;
                continue;
            }
            const Reg r = Args(i).back();
            std::optional<int32_t> value;
            if (Layout.Registers.Types[r] == Nature::Int && Ranges[r].Lo == Ranges[r].Hi) value = Ranges[r].Lo;
            auto [it, fresh] = fields.try_emplace({i.Imm, *index}, value);
            if (!fresh && it->second != value) it->second.reset();
        }
        std::erase_if(fields, [&](const auto &f) { return dynamic[f.first.first] || !f.second; });
        if (fields.empty() && Cached.empty()) return {};
        std::vector<std::optional<int32_t>> known(Plan.Regs);
        for (Reg r = 0; r < Plan.Regs; ++r)
            if (Ranges[r].Lo == Ranges[r].Hi) known[r] = Ranges[r].Lo;
        // Retain instruction positions for the existing allocation and call-save masks.
        std::vector<std::optional<Instr>> result;
        result.reserve(code.size());
        for (Instr i : code) {
            const Op op = Op(i.Op);
            const auto args = Args(i);
            if (op == Op::LoadField || op == Op::StoreField) {
                if (const auto index = Index(i)) {
                    const auto it = fields.find({i.Imm, *index});
                    if (it != fields.end()) {
                        if (op == Op::StoreField) {
                            result.emplace_back();
                            continue;
                        }
                        known[i.Dst] = it->second;
                    }
                }
            } else if (op == Op::BinOp && i.Nature == Nature::Int && known[args[0]]) {
                const auto constant = i.ArgCount == 1 ? std::optional(int32_t(i.Imm)) : known[args[1]];
                if (constant) known[i.Dst] = IntegerBinary(BinOpCode(i.Form), *known[args[0]], *constant);
            } else if ((op == Op::Select2 || op == Op::Select3) && known[args[0]]) {
                const int32_t selector = *known[args[0]];
                const Reg chosen = args[selector == 0 ? 1 : op == Op::Select3 && selector != 1 ? 3 : 2];
                i.Op = uint8_t(i.Nature == Nature::Int ? Op::IntCast : Op::FloatCast);
                i.Form = 0;
                i.Args = uint32_t(Operands.size());
                i.ArgCount = 1;
                Operands.push_back(chosen);
                known[i.Dst] = known[chosen];
            } else if (op == Op::IntCast && !i.Form) known[i.Dst] = known[args[0]];
            if (i.Dst != NoReg && i.Nature == Nature::Int && known[i.Dst]) {
                i.Op = uint8_t(Op::ConstInt);
                i.Imm = uint32_t(*known[i.Dst]);
                i.Args = i.ArgCount = 0;
            }
            result.push_back(i);
        }
        std::vector<uint32_t> uses(Plan.Regs);
        for (auto it = result.rbegin(); it != result.rend(); ++it) {
            if (!*it) continue;
            const Instr &i = **it;
            const Op op = Op(i.Op);
            const bool pure = op == Op::ConstInt || op == Op::ConstReal || op == Op::BinOp || op == Op::IntCast || op == Op::FloatCast || op == Op::Select2 ||
                op == Op::Select3 || (op == Op::LoadField && Plan.Fields[i.Imm].Kind != FieldKind::Widget);
            if (pure && !uses[i.Dst] && Layout.Registers.Slot[i.Dst] == NoReg) it->reset();
            else
                for (Reg r : Args(i)) ++uses[r];
        }
        size_t previous = result.size();
        for (size_t pc = 0; pc < result.size(); ++pc) {
            if (!result[pc]) continue;
            const Instr &i = *result[pc];
            const Op op = Op(i.Op);
            if (previous < result.size() && (op == Op::IntCast || op == Op::FloatCast) && !i.Form) {
                Instr &producer = *result[previous];
                const Reg source = Args(i)[0];
                const auto &dst = Alloc[i.Dst];
                if (producer.Dst == source && uses[source] == 1 && Layout.Registers.Slot[source] == NoReg && dst.Physical &&
                    Layout.Registers.Types[source] == i.Nature && std::ranges::none_of(Alloc, [&](const Allocation &a) {
                        return a.Type == dst.Type && a.Physical == dst.Physical && a.Begin <= begin + previous && a.End >= begin + previous;
                    })) {
                    producer.Dst = i.Dst;
                    result[pc].reset();
                    continue;
                }
            }
            previous = pc;
        }
        return result;
    }

    void Coalesce(std::span<const std::optional<Instr>> code, bool initial = false, uint32_t begin = 0) {
        struct Store {
            Reg Source = NoReg;
            uint32_t Pc = 0, Count = 0;
        };
        std::vector<Store> stores(Cached.size());
        std::vector<uint32_t> definitions(Plan.Regs, UINT32_MAX);
        for (uint32_t pc = 0; pc < code.size(); ++pc) {
            if (!code[pc]) continue;
            const Instr &i = *code[pc];
            if (i.Dst != NoReg) definitions[i.Dst] = pc;
            if (Op(i.Op) != Op::StoreField) continue;
            const auto index = Index(i);
            for (size_t k = 0; k < Cached.size(); ++k)
                if (Cached[k].Field == i.Imm && index == Cached[k].Index) stores[k] = {Args(i).back(), pc, stores[k].Count + 1};
        }
        const auto eligible = [&](size_t k) {
            return stores[k].Count == 1 && Alloc[Plan.Regs + k].Physical && Alloc[stores[k].Source].Type == Alloc[Plan.Regs + k].Type;
        };
        const auto available = [&](size_t k, uint32_t write) {
            for (uint32_t pc = 0; pc < code.size(); ++pc) {
                if (!code[pc]) continue;
                const Instr &i = *code[pc];
                if (Op(i.Op) != Op::LoadField || i.Imm != Cached[k].Field || Index(i) != Cached[k].Index || pc >= stores[k].Pc) continue;
                if (pc >= write || (Alloc[i.Dst].Physical == Alloc[Plan.Regs + k].Physical && Alloc[i.Dst].End > begin + write)) return false;
            }
            return true;
        };
        for (size_t k = 0; !initial && k < Cached.size(); ++k) {
            if (!eligible(k)) continue;
            const auto &target = Alloc[Plan.Regs + k];
            std::vector<size_t> group;
            uint32_t write = uint32_t(code.size());
            for (size_t j = 0; j < Cached.size(); ++j)
                if (eligible(j) && stores[j].Source == stores[k].Source) {
                    group.push_back(j);
                    write = std::min(write, stores[j].Pc);
                }
            if (group.front() != k || std::ranges::any_of(group, [&](size_t j) { return !available(j, write); })) continue;
            // The first sample writes the same value to both state slots.
            for (size_t j : group) {
                const auto other = Alloc[Plan.Regs + j];
                for (auto &a : Alloc)
                    if (a.Type == other.Type && a.Physical == other.Physical) a.Physical = target.Physical;
            }
        }
        for (size_t k = 0; k < Cached.size(); ++k) {
            if (!eligible(k)) continue;
            const Reg source = stores[k].Source;
            const uint32_t pc = definitions[source];
            if (pc == UINT32_MAX || Alloc[source].Begin != begin + pc || Layout.Registers.Slot[source] != NoReg || Op(code[pc]->Op) != Op::BinOp) continue;
            const auto &target = Alloc[Plan.Regs + k];
            bool safe = true;
            for (size_t j = 0; j < Cached.size(); ++j)
                if (Alloc[Plan.Regs + j].Type == target.Type && Alloc[Plan.Regs + j].Physical == target.Physical)
                    safe &= eligible(j) && stores[j].Source == source && available(j, pc);
            if (safe) Alloc[source].Physical = target.Physical;
        }
    }

    explicit Emit(const Program &program) : Plan(program.Plan), Layout(program.Layout), Def(Plan.Regs), Operands(Plan.Operands) {
        for (const auto &band : Plan.Bands)
            for (const Instr &i : band)
                if (i.Dst != NoReg) Def[i.Dst] = &i;
        for (size_t b = 0; b < Prepared.size(); ++b) {
            auto &code = Prepared[b];
            if (b == size_t(faustlens::Band::Init)) {
                code = Plan.Bands[b];
                continue;
            }
            std::vector<Reg> alias(Plan.Regs, NoReg);
            std::map<std::pair<uint32_t, uint32_t>, Reg> fields;
            bool stores = false;
            for (Instr i : Plan.Bands[b]) {
                const auto original = Plan.Args(i);
                if (std::ranges::any_of(original, [&](Reg r) { return alias[r] != NoReg; })) {
                    i.Args = uint32_t(Operands.size());
                    for (Reg r : original) Operands.push_back(alias[r] == NoReg ? r : alias[r]);
                }
                const Op op = Op(i.Op);
                if (op == Op::GuardBegin || op == Op::GuardEnd) fields.clear();
                if ((op == Op::LoadField || op == Op::StoreField) && Plan.Fields[i.Imm].Kind != FieldKind::Widget) {
                    if (const auto index = Index(i)) {
                        const auto key = std::pair{i.Imm, *index};
                        if (op == Op::StoreField) {
                            stores = true;
                            const Reg value = Args(i).back();
                            if (Layout.Registers.Types[value] == Plan.Fields[i.Imm].Nature) fields[key] = value;
                            else fields.erase(key);
                        } else {
                            const auto prior = fields.find(key);
                            if (prior != fields.end() && Layout.Registers.Slot[i.Dst] == NoReg) {
                                alias[i.Dst] = prior->second;
                                continue;
                            }
                            fields[key] = i.Dst;
                        }
                    } else if (op == Op::StoreField) std::erase_if(fields, [&](const auto &entry) { return entry.first.first == i.Imm; });
                }
                code.push_back(i);
            }
            if (!stores) continue;
            std::vector<std::vector<Instr>> before(code.size());
            std::vector<bool> moved(code.size());
            std::vector<uint32_t> defined(Plan.Regs);
            std::vector<uint32_t> fieldBarrier(Plan.Fields.size());
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> access;
            uint32_t barrier = 0;
            for (uint32_t pc = 0; pc < code.size(); ++pc) {
                const Instr &i = code[pc];
                const Op op = Op(i.Op);
                if (op == Op::GuardBegin || op == Op::GuardEnd || op == Op::FFun) barrier = pc + 1;
                if (i.Dst != NoReg) defined[i.Dst] = pc + 1;
                if (op != Op::LoadField && op != Op::StoreField) continue;
                const auto index = Index(i);
                if (!index || Plan.Fields[i.Imm].Kind == FieldKind::Widget) {
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
        BoundIndices();
        FoldInstructions();
        for (auto &code : Prepared)
            for (Instr &i : code)
                if (Op(i.Op) == Op::BinOp) SelectImmediate(i);
        std::vector<bool> used(Plan.Regs);
        for (const auto &code : Prepared)
            for (const Instr &i : code)
                for (Reg r : ReadArgs(i)) used[r] = true;
        for (auto &code : Prepared)
            std::erase_if(code, [&](const Instr &i) { return (Op(i.Op) == Op::ConstInt || Op(i.Op) == Op::ConstReal) && !used[i.Dst]; });
        const auto &control = Prepared[size_t(faustlens::Band::Control)];
        ControlEnd = uint32_t(control.size());
        auto &sample = Prepared[size_t(faustlens::Band::Sample)];
        sample.insert(sample.begin(), control.begin(), control.end());
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
    void Pool(bool branch) {
        if (Literals.empty()) return;
        const size_t skip = branch ? Branch(0x14000000) : 0;
        if (Words.size() % 2) Word(0xd503201f);
        std::map<uint64_t, size_t> addresses;
        for (const auto &literal : Literals) {
            const auto [it, fresh] = addresses.try_emplace(literal.Bits, Words.size());
            if (fresh) {
                Word(uint32_t(literal.Bits));
                Word(uint32_t(literal.Bits >> 32));
            }
            const size_t offset = it->second - literal.Word;
            if (offset >= (1u << 18)) throw std::runtime_error("native literal exceeds load range");
            Words[literal.Word] |= uint32_t(offset) << 5;
        }
        Literals.clear();
        if (branch) Target(skip, Words.size());
    }
    void Rewind(size_t words, size_t relocations) {
        Words.resize(words);
        Relocations.resize(relocations);
        while (!Literals.empty() && Literals.back().Word >= words) Literals.pop_back();
    }
    void Imm(uint64_t value, uint32_t reg = 9) {
        const uint32_t first = value ? std::countr_zero(value) / 16 : 0;
        Word(0xd2800000 | first << 21 | uint32_t((value >> (first * 16)) & 0xffff) << 5 | reg); // movz
        for (uint32_t k = first + 1; k < 4; ++k)
            if (const uint32_t bits = uint32_t((value >> (k * 16)) & 0xffff)) Word(0xf2800000 | k << 21 | bits << 5 | reg); // movk
    }
    size_t Address(Binding kind, uint32_t index, uint32_t reg) {
        const uint32_t word = uint32_t(Words.size());
        Relocations.push_back({kind, index, word, word, word + 4});
        for (uint32_t k = 0; k < 4; ++k) Word(Relocation::AddressWord(reg, k));
        return Relocations.size() - 1;
    }
    void AddOffset(uint32_t base, uint32_t bytes, uint32_t reg) {
        if (const auto imm = AddImmediate(bytes)) Word(0x91000000 | *imm | base << 5 | reg);
        else if (const auto high = AddImmediate(bytes & ~4095u)) {
            Word(0x91000000 | *high | base << 5 | reg);
            Word(0x91000000 | (bytes & 4095) << 10 | reg << 5 | reg);
        } else {
            Imm(bytes, reg);
            Word(0x8b000000 | reg << 16 | base << 5 | reg);
        }
    }
    void Mem(uint32_t op, uint32_t reg, uint32_t base, uint32_t bytes, uint32_t scale = 8) {
        if (bytes % scale || bytes / scale >= 4096) {
            const uint32_t low = bytes % scale ? 0 : bytes & 4095;
            AddOffset(base, bytes - low, 17);
            base = 17;
            bytes = low;
        }
        Word(op | (bytes / scale) << 10 | base << 5 | reg);
    }
    void ScalarMem(bool store, bool integer, uint32_t reg, uint32_t base, uint32_t bytes) {
        Mem(integer ? (store ? 0xb9000000 : 0xb9400000) : (store ? 0xfd000000 : 0xfd400000), reg, base, bytes, integer ? 4 : 8);
    }
    void Value(Reg r, bool store, bool integer = false, uint32_t reg = 0) {
        if (!integer && r == (store ? CallOutput : CallInput)) {
            Move(false, store ? reg : 0, store ? 0 : reg);
            return;
        }
        if (const uint32_t physical = Alloc[r].Physical) {
            Move(integer, store ? reg : physical, store ? physical : reg);
            return;
        }
        Storage(r, store, integer, reg);
    }
    void Storage(Reg r, bool store, bool integer, uint32_t reg) {
        const uint32_t slot = Layout.Registers.Slot.at(r);
        const uint32_t base = Context[slot == NoReg ? 7 : 0];
        const uint32_t bytes = (slot == NoReg ? r : slot) * 8;
        ScalarMem(store, integer, reg, base, bytes);
    }
    void Real(Reg r, uint32_t reg = 0) {
        if (Layout.Registers.Types[r] == Nature::Int) {
            Word(0x1e620000 | IntReg(r) << 5 | reg); // scvtf
        } else Value(r, false, false, reg);
    }
    void Integer(Reg r, uint32_t reg = 9) {
        if (Layout.Registers.Types[r] == Nature::Real) {
            Word(0x1e780000 | RealReg(r, 2) << 5 | reg); // fcvtzs
        } else Value(r, false, true, reg);
    }
    uint32_t RealReg(Reg r, uint32_t scratch = 0) {
        if (r == CallInput) return 0;
        if (Alloc[r].Physical && Layout.Registers.Types[r] == Nature::Real) return Alloc[r].Physical;
        Real(r, scratch);
        return scratch;
    }
    uint32_t IntReg(Reg r, uint32_t scratch = 9) {
        if (Alloc[r].Physical && Layout.Registers.Types[r] == Nature::Int) return Alloc[r].Physical;
        Integer(r, scratch);
        return scratch;
    }
    uint32_t Dst(Reg r, uint32_t scratch = 0) const { return r == CallOutput ? 0 : Alloc[r].Physical ? Alloc[r].Physical : scratch; }
    void Binary(uint32_t op, uint32_t dst, uint32_t x, uint32_t y) { Word(op | y << 16 | x << 5 | dst); }
    static uint32_t Condition(BinOpCode form, bool integer) {
        switch (form) {
            case BinOpCode::GT: return 12;
            case BinOpCode::LT: return integer ? 11 : 4;
            case BinOpCode::GE: return 10;
            case BinOpCode::LE: return integer ? 13 : 9;
            case BinOpCode::EQ: return 0;
            default: return 1;
        }
    }
    static std::optional<uint32_t> AddImmediate(uint32_t value) {
        if (value < 4096) return value << 10;
        if (!(value & 4095) && (value >> 12) < 4096) return (1u << 22) | (value >> 12) << 10;
        return {};
    }
    static std::optional<uint32_t> LogicalImmediate(uint32_t value) {
        if (!value || value == UINT32_MAX) return {};
        for (uint32_t width = 2; width <= 32; width *= 2) {
            const uint32_t mask = UINT32_MAX >> (32 - width);
            uint32_t bits = value & mask, repeated = bits;
            for (uint32_t n = width; n < 32; n *= 2) repeated |= repeated << n;
            if (repeated != value) continue;
            for (uint32_t rotation = 0; rotation < width; ++rotation) {
                if (bits && !(bits & (bits + 1)))
                    return ((width - rotation) & (width - 1)) << 16 | (((0u - 2 * width) | (uint32_t(std::popcount(bits)) - 1)) & 63) << 10;
                bits = (bits >> 1) | (bits & 1) << (width - 1);
            }
        }
        return {};
    }
    void SelectImmediate(Instr &i) const {
        const auto args = Args(i);
        if (Layout.Registers.Types[args[0]] != Nature::Int || Layout.Registers.Types[args[1]] != Nature::Int) return;
        const auto form = BinOpCode(i.Form);
        Reg source = args[0], constant = args[1];
        if (Ranges[constant].Lo != Ranges[constant].Hi &&
            (form == BinOpCode::Add || form == BinOpCode::Mul || form == BinOpCode::AND || form == BinOpCode::OR || form == BinOpCode::XOR))
            std::swap(source, constant);
        if (Ranges[constant].Lo != Ranges[constant].Hi) return;
        const int32_t value = Ranges[constant].Lo;
        const uint32_t magnitude = value < 0 ? 0u - uint32_t(value) : uint32_t(value);
        uint32_t op = 0;
        if (form >= BinOpCode::GT && form <= BinOpCode::NE) {
            if (const auto imm = AddImmediate(magnitude)) op = (value < 0 ? 0x31000000 : 0x71000000) | *imm;
        } else if (form == BinOpCode::Add || form == BinOpCode::Sub) {
            if (const auto imm = AddImmediate(magnitude)) op = (((form == BinOpCode::Sub) != (value < 0)) ? 0x51000000 : 0x11000000) | *imm;
        } else if (form == BinOpCode::AND || form == BinOpCode::OR || form == BinOpCode::XOR) {
            if (const auto imm = LogicalImmediate(uint32_t(value)))
                op = (form == BinOpCode::AND ? 0x12000000 : form == BinOpCode::OR ? 0x32000000 : 0x52000000) | *imm;
        } else if (form == BinOpCode::LeftShift || form == BinOpCode::RightShift || form == BinOpCode::LRightShift) {
            const uint32_t shift = uint32_t(value) & 31;
            op = form == BinOpCode::LeftShift ? 0x53000000 | ((32 - shift) & 31) << 16 | (31 - shift) << 10 :
                                                (form == BinOpCode::RightShift ? 0x13007c00 : 0x53007c00) | shift << 16;
        } else if (form != BinOpCode::Div && form != BinOpCode::Rem && form != BinOpCode::Mul) return;
        else if (magnitude && !std::has_single_bit(magnitude)) return;
        if (!op && form != BinOpCode::Div && form != BinOpCode::Rem && form != BinOpCode::Mul) return;
        i.Args += source == args[0] ? 0 : 1;
        i.ArgCount = 1;
        i.Imm = uint32_t(value);
        i.Aux = op;
    }
    void IntegerImmediate(const Instr &i) {
        const Reg source = Args(i)[0];
        const auto form = BinOpCode(i.Form);
        const int32_t value = int32_t(i.Imm);
        const uint32_t magnitude = value < 0 ? 0u - uint32_t(value) : uint32_t(value), op = i.Aux;
        const uint32_t x = IntReg(source), dst = i.Nature == Nature::Int ? Dst(i.Dst, 10) : 10;
        if (op && form >= BinOpCode::GT && form <= BinOpCode::NE) {
            Word(op | x << 5 | 31); // cmp / cmn
            Word(0x1a9f07e0 | (Condition(form, true) ^ 1) << 12 | dst); // cset
        } else if (op) Word(op | x << 5 | dst);
        else if (!magnitude || magnitude == 1) {
            if ((!magnitude && form != BinOpCode::Rem) || (magnitude == 1 && form == BinOpCode::Rem)) Move(true, 31, dst);
            else if (value < 0 && form != BinOpCode::Rem) Word(0x4b0003e0 | x << 16 | dst); // neg
            else Move(true, x, dst);
        } else {
            const uint32_t shift = std::countr_zero(magnitude);
            if (form == BinOpCode::Mul) {
                Word(0x53000000 | (32 - shift) << 16 | (31 - shift) << 10 | x << 5 | dst); // lsl
                if (value < 0) Word(0x4b0003e0 | dst << 16 | dst); // neg
            } else if (form == BinOpCode::Rem && NonnegativeRemainder(source, magnitude)) {
                Word(0x12000000 | *LogicalImmediate(magnitude - 1) | x << 5 | dst); // and
            } else {
                uint32_t biased = x;
                if (Ranges[source].Lo < 0) {
                    Word(0x131f7c0b | x << 5); // asr w11, x, #31
                    Word(0x0b40000b | 11u << 16 | (32 - shift) << 10 | x << 5); // add w11, x, w11, lsr #(32-shift)
                    biased = 11;
                }
                Word(0x13007c00 | shift << 16 | biased << 5 | (form == BinOpCode::Rem ? 11 : dst)); // asr
                if (form == BinOpCode::Rem) Word(0x4b0b0000 | shift << 10 | x << 5 | dst); // sub dst, x, w11, lsl #shift
                else if (value < 0) Word(0x4b0003e0 | dst << 16 | dst); // neg
            }
        }
        Result(i, true, dst);
    }
    void FieldAddress(uint32_t field, uint32_t reg) { AddOffset(Context[1], Layout.FieldAt[field] * 8, reg); }
    void Field(const Instr &i, std::span<const Reg> args, bool store) {
        const auto &f = Plan.Fields.at(i.Imm);
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
        const bool integer = f.Nature == Nature::Int;
        // Convert real store values before index clamping, which also uses w9.
        const uint32_t real = store && !integer ? RealReg(args.back()) : 0;
        uint32_t base = Context[1], offset = Layout.FieldAt[i.Imm] * 8, indexReg = 0;
        bool indexed = false;
        if (const auto index = Index(i)) offset += *index * 8;
        else {
            base = BaseReg(i.Imm);
            if (!base) {
                base = 11;
                FieldAddress(i.Imm, base);
            }
            indexReg = IntReg(args[0]);
            const auto range = Ranges[args[0]];
            const uint32_t limit = std::max(1u, f.Extent) - 1;
            if (range.Lo < 0 || uint32_t(range.Hi) > limit) {
                Imm(limit, 10);
                Binary(0x6b00001f, 0, indexReg, 10);
                Binary(0x1a803000, 9, indexReg, 10); // csel lo
                indexReg = 9;
            }
            indexed = f.Nature == Nature::Real;
            if (!indexed) {
                Word(0x8b000c0b | indexReg << 16 | base << 5); // add x11, base, index, lsl #3
                base = 11;
            }
            offset = 0;
        }
        const uint32_t reg = store ? (integer ? IntReg(args.back()) : real) : Dst(i.Dst, integer ? 9 : 0);
        if (indexed) Word((store ? 0xfc205800 : 0xfc605800) | indexReg << 16 | base << 5 | reg); // str/ldr dN, [base, index, uxtw #3]
        else ScalarMem(store, integer, reg, base, offset);
        if (!store) Value(i.Dst, true, integer, reg);
    }

    void CalleeRegisters(bool store) {
        for (uint32_t k = 0; k < Callee.size(); ++k) {
            const auto [integer, reg] = Callee[k];
            if (k < 64 && k + 1 < Callee.size() && Callee[k + 1].first == integer) {
                Word((integer ? 0xa9000000 : 0x6d000000) | (store ? 0 : 1u << 22) | k << 15 | Callee[k + 1].second << 10 | 31 << 5 | reg);
                ++k;
            } else Mem((integer ? 0xf9000000 : 0xfd000000) | (store ? 0 : 1u << 22), reg, 31, k * 8);
        }
    }
    void CallRegisters(bool store) {
        uint32_t slot = uint32_t(Callee.size());
        for (uint32_t mask = CallContext; mask; mask &= mask - 1) Mem(store ? 0xf9000000 : 0xf9400000, std::countr_zero(mask), 31, slot++ * 8);
        for (uint32_t mask = CallMask[Pc]; mask; mask &= mask - 1) Mem(store ? 0xfd000000 : 0xfd400000, std::countr_zero(mask), 31, slot++ * 8);
    }
    void Invoke(Binding kind, uint32_t index, bool integer = false) {
        const auto target = kind == Binding::Math ? std::ranges::find(MathTargets, index, &MathTarget::Index) : MathTargets.end();
        const uint32_t reg = target == MathTargets.end() ? 16 : target->Physical;
        if (reg == 16) Address(kind, index, reg);
        Word(0xd63f0000 | reg << 5); // blr
        if (integer) Word(0x2a0003e9); // mov w9, w0
        CallRegisters(false);
    }
    void Result(const Instr &i, bool integer, uint32_t reg) {
        const bool target = i.Nature == Nature::Int;
        if (integer != target) {
            Word((integer ? 0x1e620000 : 0x1e780009) | reg << 5);
            reg = target ? 9 : 0;
        }
        Value(i.Dst, true, target, reg);
    }
    void Extended(const Instr &i, std::span<const Reg> args) {
        const Ext form = Ext(i.Form);
        const bool integer = Layout.Registers.Types[args[0]] == Nature::Int;
        if (form == Ext::Pow) {
            if (const auto k = PowerExponent(Def[args[1]])) {
                const bool same = integer == (i.Nature == Nature::Int);
                const uint32_t dst = same ? Dst(i.Dst, integer ? 9 : 0) : integer ? 9 : 0;
                if (*k == 0) {
                    if (integer) Imm(1, dst);
                    else Word(0x1e6e1000 | dst); // fmov dN, #1.0
                } else {
                    const uint32_t scratch = integer ? 10 : 1;
                    uint32_t x = integer ? IntReg(args[0], scratch) : RealReg(args[0], scratch);
                    if (*k > 2 && x == dst) {
                        Move(integer, x, scratch);
                        x = scratch;
                    }
                    if (*k <= 1) Move(integer, x, dst);
                    for (int n = 1; n < *k; ++n) Binary(integer ? 0x1b007c00 : 0x1e600800, dst, n == 1 ? x : dst, x);
                }
                Result(i, integer, dst);
                return;
            }
        }
        if (integer && (form == Ext::Abs || form == Ext::Min || form == Ext::Max)) {
            const uint32_t x = IntReg(args[0]), dst = i.Nature == Nature::Int ? Dst(i.Dst, 9) : 9;
            if (form == Ext::Abs) {
                Word(0x7100001f | x << 5); // cmp
                Binary(0x5a805400, dst, x, x); // cneg mi
            } else {
                const uint32_t y = IntReg(args[1], 10);
                Binary(0x6b00001f, 0, x, y);
                Binary(form == Ext::Min ? 0x1a80b000 : 0x1a80c000, dst, x, y);
            }
            Result(i, true, dst);
            return;
        }
        const uint32_t unaryOp = UnaryOp(form);
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
            Result(i, false, dst);
            return;
        }
        if (form >= Ext::AssertBounds) throw std::runtime_error("invalid native math operation");
        Real(args[0]);
        if (args.size() > 1) Real(args[1], 1);
        CallRegisters(true);
        Invoke(Binding::Math, uint32_t(form));
        Result(i, false, 0);
    }
    void Foreign(const Instr &i, std::span<const Reg> args) {
        const auto &symbol = Plan.Foreign.at(i.Imm);
        const bool integer = i.Nature == Nature::Int;
        const uint32_t begin = uint32_t(Words.size());
        size_t relocation;
        if (symbol.Kind != ForeignKind::Function) {
            relocation = Address(Binding::Foreign, i.Imm, 11);
            Mem(integer ? 0xb9400000 : 0xfd400000, integer ? 9 : 0, 11, 0);
        } else {
            CallRegisters(true);
            uint32_t ints = 0, reals = 0;
            for (size_t k = 0; k < std::min<size_t>(args.size(), 2); ++k) {
                if (symbol.Args[k] == Nature::Int) Integer(args[k], 12 + ints++);
                else Real(args[k], reals++);
            }
            for (uint32_t k = 0; k < ints; ++k) Word(0x2a0003e0 | (12 + k) << 16 | k);
            relocation = Relocations.size();
            Invoke(Binding::Foreign, i.Imm, integer);
        }
        Relocations[relocation].Begin = begin;
        Relocations[relocation].End = uint32_t(Words.size());
        Value(i.Dst, true, integer, integer ? 9 : 0);
    }
    void Sound(const Instr &i, std::span<const Reg> args) {
        CallRegisters(true);
        for (uint32_t k = 0; k < args.size(); ++k) Integer(args[k], 12 + k);
        Mem(0xf9400000, 0, 6, 24);
        Imm(i.Imm, 1);
        for (uint32_t k = 0; k < args.size(); ++k) Word(0x2a0003e0 | (12 + k) << 16 | (2 + k));
        const bool integer = Op(i.Op) != Op::SoundfileRead;
        Invoke(Binding::Sound, uint32_t(Op(i.Op)), integer);
        Value(i.Dst, true, integer, integer ? 9 : 0);
    }
    void Instruction(const Instr &i, std::vector<size_t> &guards) {
        // Leave room for literal data within the signed 19-bit load displacement.
        if (!Literals.empty() && Words.size() - Literals.front().Word >= (1u << 16)) Pool(true);
        const auto args = Args(i);
        switch (Op(i.Op)) {
            case Op::ConstReal: {
                const uint64_t bits = uint64_t(i.Imm) | uint64_t(i.Aux) << 32;
                const uint32_t dst = Dst(i.Dst), exponent = (bits >> 52) & 2047;
                if (!bits) Word(0x9e6703e0 | dst); // fmov dN, xzr
                else if (!(bits & ((1ull << 48) - 1)) && exponent >= 1020 && exponent <= 1027) {
                    const uint32_t immediate = ((bits >> 48) & 127) | ((bits >> 56) & 128);
                    Word(0x1e601000 | immediate << 13 | dst); // fmov dN, #imm
                } else {
                    uint32_t parts = 0;
                    for (uint32_t k = 0; k < 4; ++k) parts += bool((bits >> (16 * k)) & 0xffff);
                    if (parts >= 3) {
                        Literals.push_back({Words.size(), bits});
                        Word(0x5c000000 | dst); // ldr dN, literal
                    } else {
                        Imm(bits);
                        Word(0x9e670120 | dst); // fmov dN, x9
                    }
                }
                Value(i.Dst, true, false, dst);
                return;
            }
            case Op::ConstInt: {
                const uint32_t dst = Dst(i.Dst, 9);
                Imm(i.Imm, dst);
                Value(i.Dst, true, true, dst);
                return;
            }
            case Op::Input:
            case Op::Output: {
                const bool output = Op(i.Op) == Op::Output;
                const uint32_t reg = output ? RealReg(args[0]) : Dst(i.Dst), base = Context[output ? 3 : 2];
                if (Connected) {
                    const uint32_t op = Advancing ? (output ? 0xfc008400 : 0xfc408400) : (output ? 0xfc207800 : 0xfc607800) | Context[5] << 16;
                    Word(op | ChannelReg(output, i.Imm) << 5 | reg);
                    if (!output) Value(i.Dst, true, false, reg);
                    return;
                }
                if (!output) Word(0x9e6703e0 | reg); // fmov dN, xzr
                uint32_t pointer = ChannelReg(output, i.Imm);
                std::optional<size_t> absent;
                if (!pointer) {
                    absent = Branch(0xb4000000 | base);
                    pointer = 9;
                    Mem(0xf9400000, pointer, base, i.Imm * 8);
                }
                const size_t missing = Branch(0xb4000000 | pointer);
                Word((output ? 0xfc207800 : 0xfc607800) | Context[5] << 16 | pointer << 5 | reg); // str/ldr indexed
                if (absent) Target(*absent, Words.size());
                Target(missing, Words.size());
                if (!output) Value(i.Dst, true, false, reg);
                return;
            }
            case Op::BinOp: {
                if (i.ArgCount == 1) {
                    IntegerImmediate(i);
                    return;
                }
                const bool integer = Layout.Registers.Types[args[0]] == Nature::Int;
                const uint32_t x = integer ? IntReg(args[0]) : RealReg(args[0]);
                const uint32_t y = integer ? IntReg(args[1], 10) : RealReg(args[1], 1);
                const auto form = BinOpCode(i.Form);
                if (form >= BinOpCode::GT && form <= BinOpCode::NE) {
                    const uint32_t condition = Condition(form, integer);
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
                            CallRegisters(true);
                            Invoke(Binding::Math, uint32_t(Ext::Fmod));
                            Result(i, false, 0);
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
                Result(i, integer, dst);
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
                const uint32_t selector = IntReg(args[0]), dst = Dst(i.Dst, integer ? 10 : 0);
                uint32_t value = integer ? IntReg(args.back(), 10) : RealReg(args.back());
                for (size_t k = args.size() - 2; k > 0; --k) {
                    const uint32_t candidate = integer ? IntReg(args[k], 11) : RealReg(args[k], 1);
                    Word(0x7100001f | selector << 5 | uint32_t(k - 1) << 10); // cmp
                    Binary(integer ? 0x1a800000 : 0x1e600c00, dst, candidate, value); // csel/fcsel eq
                    value = dst;
                }
                Value(i.Dst, true, integer, value);
                return;
            }
            case Op::LoopBegin: {
                Imm(i.Imm, 10);
                const size_t exit = Branch(0x3400000a, true);
                Imm(0);
                Value(i.Dst, true, true, 9);
                Loops.push_back({Words.size(), exit, i.Dst, i.Imm});
                return;
            }
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
                Real(args[0], Dst(i.Dst));
                Value(i.Dst, true, false, Dst(i.Dst));
                return;
            case Op::IntCast:
                if (i.Form) Word(0x1e580000 | (64u - i.Form) << 10 | RealReg(args[0]) << 5 | Dst(i.Dst, 9)); // fcvtzs fixed-point
                else Integer(args[0], Dst(i.Dst, 9));
                Value(i.Dst, true, true, Dst(i.Dst, 9));
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
    uint32_t Advance() {
        if (Advancing) {
            Word(0x71000400 | Context[5] << 5 | Context[5]); // subs remaining, remaining, #1
            return 0x54000001; // b.ne
        }
        Word(0x11000400 | Context[5] << 5 | Context[5]); // add frame, frame, #1
        Binary(0x6b00001f, 0, Context[5], Context[4]); // cmp frame, frames
        return 0x5400000b; // b.lt
    }
    void LoopBack(size_t body, bool unroll, bool initial = false) {
        constexpr uint32_t factor = 4;
        const size_t maxBody = Leaf ? 4 : 6; // Limit four copies to 64 bytes of leaf instructions or 96 bytes with calls.
        const size_t size = Words.size() - body;
        if (Advancing || !unroll || size > maxBody || (!Relocations.empty() && Relocations.back().Word >= body) ||
            (!Literals.empty() && Literals.back().Word >= body)) {
            Back(Advance(), body);
            return;
        }
        const std::vector<uint32_t> sample(Words.begin() + body, Words.end());
        Words.resize(body);
        const auto indexed = [&](uint32_t word) {
            const uint32_t op = word & 0xffe0fc00;
            return (op == 0xfc607800 || op == 0xfc207800) && ((word >> 16) & 31) == Context[5];
        };
        std::vector<uint32_t> pointers;
        for (uint32_t word : sample)
            if (indexed(word)) {
                const uint32_t base = (word >> 5) & 31;
                if (std::ranges::find(pointers, base) == pointers.end()) pointers.push_back(base);
            }
        if (pointers.size() + 1 >= factor) pointers.clear();
        if (!initial)
            for (uint32_t base : pointers) Binary(0x8b000c00, base, base, Context[5]); // add base, base, frame, lsl #3
        Word(0x5100000d | Context[4] << 5 | factor << 10); // sub w13, frames, #factor
        Binary(0x6b00001f, 0, Context[5], 13); // cmp frame, w13
        const size_t shortBlock = Branch(0x5400000c); // b.gt
        while (Words.size() % 4) Word(0xd503201f); // nop
        const size_t group = Words.size();
        const auto append = [&] {
            Words.insert(Words.end(), sample.begin(), sample.end());
            Word(0x11000400 | Context[5] << 5 | Context[5]); // add frame, frame, #1
        };
        for (uint32_t k = 0; k < factor; ++k) {
            if (pointers.empty()) append();
            else
                for (uint32_t word : sample) {
                    if (indexed(word)) word = ((word & (1u << 22)) ? 0xfd400000 : 0xfd000000) | k << 10 | (word & 1023);
                    Word(word);
                }
        }
        if (!pointers.empty()) {
            for (uint32_t base : pointers) Word(0x91000000 | factor * 8 << 10 | base << 5 | base); // add base, base, #bytes
            Word(0x11000000 | factor << 10 | Context[5] << 5 | Context[5]); // add frame, frame, #factor
        }
        if (!Leaf) Word(0x5100000d | Context[4] << 5 | factor << 10); // w13 is caller-saved.
        Binary(0x6b00001f, 0, Context[5], 13); // cmp frame, w13
        Back(0x5400000d, group); // b.le
        Binary(0x6b00001f, 0, Context[5], Context[4]); // cmp frame, frames
        const size_t done = Branch(0x5400000a); // b.ge
        Target(shortBlock, Words.size());
        for (uint32_t base : pointers) Binary(0xcb000c00, base, base, Context[5]); // sub base, base, frame, lsl #3
        const size_t tail = Words.size();
        append();
        Binary(0x6b00001f, 0, Context[5], Context[4]); // cmp frame, frames
        Back(0x5400000b, tail); // b.lt
        Target(done, Words.size());
    }
    void RunLoop(std::span<const Instr> code, faustlens::Band band, uint32_t begin, bool unroll = false) {
        if (code.empty()) return;
        Advancing = Connected && code.size() > 6 && std::ranges::none_of(code, [&](const Instr &i) {
                        const Op op = Op(i.Op);
                        if (op == Op::GuardBegin || op == Op::LoopBegin) return true;
                        return (op == Op::Input || op == Op::Output) &&
                            std::ranges::count_if(code, [&](const Instr &other) { return other.Op == i.Op && other.Imm == i.Imm; }) != 1;
                    });
        if (Advancing) Word(0x2a0003e0 | Context[4] << 16 | Context[5]); // mov remaining, frames
        if (Connected)
            while (Words.size() % 4) Word(0xd503201f); // nop
        const size_t loop = Words.size();
        std::vector<size_t> guards;
        Pc = begin;
        for (const Instr &i : code) {
            CallInput = CallValues[Pc];
            CallOutput = CallValues[Pc + 1];
            Instruction(i, guards);
            ++Pc;
        }
        CallInput = CallOutput = NoReg;
        if (!guards.empty()) throw std::runtime_error("unmatched native guard begin");
        const size_t end = Words.size(), relocations = Relocations.size();
        bool peeled = false;
        if (band == faustlens::Band::Sample && end - loop <= LoopWords) {
            if (const auto steady = Steady(code, begin); !steady.empty()) {
                const auto original = Alloc;
                // Cached registers are live across the block, so the original call-save masks cover reassigned values.
                Coalesce(steady, false, begin);
                const size_t done = Branch(Advance() ^ 1u, true); // b.eq or b.ge
                if (Connected)
                    while (Words.size() % 4) Word(0xd503201f); // nop
                const size_t body = Words.size();
                Pc = begin;
                for (const auto &i : steady) {
                    if (i) Instruction(*i, guards);
                    ++Pc;
                }
                if (Words.size() - body < end - loop) {
                    LoopBack(body, unroll);
                    Target(done, Words.size());
                    peeled = true;
                } else {
                    Alloc = original;
                    Rewind(end, relocations);
                }
            }
        }
        if (!peeled) LoopBack(loop, unroll, true);
        Advancing = false;
    }
    [[gnu::always_inline]] size_t Band(faustlens::Band band) {
        const auto &instructions = Prepared[size_t(band)];
        const uint32_t begin = band == faustlens::Band::Sample ? ControlEnd : 0;
        const auto code = std::span(instructions).subspan(begin);
        const size_t entry = Words.size();
        if (instructions.empty()) {
            Word(0xd65f03c0); // ret
            return entry;
        }
        Allocate(instructions, band);
        if (UsedContext & (1u << 6)) Word(0xaa0503e6); // mov x6, x5
        if (UsedContext & (1u << 7)) Mem(0xf9400000, 7, 5, 16);
        if (StackBytes) Word(0xd10003ff | StackBytes << 10);
        CalleeRegisters(true);
        for (uint32_t reg = 0; reg < Context.size(); ++reg)
            if (reg != 5 && Context[reg] != reg) Word(0xaa0003e0 | reg << 16 | Context[reg]); // mov pointer or frame count
        Word(0x2a1f03e0 | Context[5]); // mov frame, wzr
        Boundary(false, Transfers(false));
        for (const auto &c : Channels) {
            const uint32_t base = c.Output ? 3 : 2;
            Imm(0, c.Physical);
            const size_t absent = Branch(0xb4000000 | base);
            Mem(0xf9400000, c.Physical, base, c.Index * 8);
            Target(absent, Words.size());
        }
        for (const auto &base : Bases) FieldAddress(base.Field, base.Physical);
        for (const auto &target : MathTargets) Address(Binding::Math, target.Index, target.Physical);
        std::vector<size_t> guards;
        for (Pc = 0; Pc < begin; ++Pc) {
            CallInput = CallValues[Pc];
            CallOutput = CallValues[Pc + 1];
            Instruction(instructions[Pc], guards);
        }
        CallInput = CallOutput = NoReg;
        if (!guards.empty()) throw std::runtime_error("unmatched native control guard begin");
        Pool(true); // Resolve control literals before emitting alternative sample loops.
        std::optional<size_t> empty;
        if (band == faustlens::Band::Sample && !code.empty()) empty = Branch(0x34000000 | Context[4], true);
        const bool candidate = band == faustlens::Band::Sample && !Channels.empty() && code.size() <= LoopWords &&
            std::ranges::all_of(code,
                                [&](const Instr &i) { return (Op(i.Op) != Op::Input && Op(i.Op) != Op::Output) || ChannelReg(Op(i.Op) == Op::Output, i.Imm); });
        const auto emptyStores = empty && begin ? Transfers(true) : std::vector<Transfer>{};
        const auto original = candidate ? Alloc : std::vector<Allocation>{};
        std::optional<std::pair<std::vector<Transfer>, size_t>> connectedBoundary;
        std::optional<size_t> generalExit, connectedExit;
        const size_t dispatch = Words.size();
        if (candidate) Word(0x14000000);
        const size_t general = Words.size();
        RunLoop(code, band, begin);
        const auto generalStores = Transfers(true);
        if (candidate) {
            Words[dispatch] = 0xd503201f; // nop
            if (Words.size() - general <= (Leaf ? LoopWords : CallLoopWords)) {
                const size_t end = Words.size(), relocations = Relocations.size();
                const size_t done = Branch(0x14000000);
                const size_t checks = Words.size();
                for (const auto &c : Channels) Back(0xb4000000 | c.Physical, general);
                Alloc = original;
                Connected = true;
                const size_t connected = Words.size();
                RunLoop(code, band, begin, true);
                if (Words.size() - checks > LoopWords) {
                    Rewind(connected, relocations);
                    Alloc = original;
                    RunLoop(code, band, begin);
                }
                Connected = false;
                if (Words.size() - checks <= LoopWords) {
                    Words[dispatch] = 0x14000000;
                    Target(dispatch, checks);
                    generalExit = done;
                    const auto connectedStores = Transfers(true);
                    if (connectedStores != generalStores) {
                        connectedBoundary.emplace(connectedStores, Words.size());
                        Boundary(true, connectedStores);
                        connectedExit = Branch(0x14000000);
                    }
                } else {
                    Rewind(end, relocations);
                }
            }
        }
        const size_t generalBoundary = Words.size();
        if (generalExit) Target(*generalExit, generalBoundary);
        Boundary(true, generalStores);
        if (connectedExit) Target(*connectedExit, Words.size());
        if (empty) {
            if (!begin) Target(*empty, Words.size());
            else if (emptyStores == generalStores) Target(*empty, generalBoundary);
            else if (connectedBoundary && emptyStores == connectedBoundary->first) Target(*empty, connectedBoundary->second);
            else {
                const size_t done = Branch(0x14000000);
                Target(*empty, Words.size());
                Boundary(true, emptyStores);
                Target(done, Words.size());
            }
        }
        CalleeRegisters(false);
        if (StackBytes) Word(0x910003ff | StackBytes << 10);
        Word(0xd65f03c0); // ret
        Pool(false);
        return entry;
    }
};

} // namespace

std::expected<std::shared_ptr<const Program>, std::string> Program::Compile(faustlens::Plan p, UiNode ui) {
    try {
        auto program = std::shared_ptr<Program>(new Program(std::move(p), std::move(ui)));
        Emit emit{*program};
        program->Entries = {uint32_t(emit.Band(Band::Init)), uint32_t(emit.Band(Band::Sample))};
        program->Words = std::move(emit.Words);
        program->Relocations = std::move(emit.Relocations);
        return program;
    } catch (const std::runtime_error &e) { return std::unexpected(e.what()); }
}

} // namespace faustlens::arm64
