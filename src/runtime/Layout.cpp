#include "runtime/Layout.h"

#include <algorithm>
#include <stdexcept>

namespace faustlens {

RegisterLayout::RegisterLayout(const Plan &p) : Slot(p.Regs, NoReg), Types(p.Regs, Nature::Real) {
    for (Reg r : p.Operands)
        if (r >= p.Regs) throw std::runtime_error("invalid Plan operand register");
    std::vector<int> defined(p.Regs, -1);
    std::vector<bool> retained(p.Regs, false), initialized(p.Regs, false);
    for (size_t b = 0; b < p.Bands.size(); ++b) {
        int guards = 0;
        for (const Instr &i : p.Bands[b]) {
            if (size_t(i.Args) + i.ArgCount > p.Operands.size() || (i.Dst != NoReg && i.Dst >= p.Regs))
                throw std::runtime_error("invalid Plan instruction operands");
            for (Reg r : p.Args(i))
                if (defined[r] >= 0 && defined[r] != int(b)) retained[r] = true;
            if (Op(i.Op) == Op::GuardBegin) ++guards;
            if (Op(i.Op) == Op::GuardEnd) --guards;
            if (i.Dst == NoReg) continue;
            defined[i.Dst] = int(b);
            Types[i.Dst] = i.Nature;
            initialized[i.Dst] = b == size_t(Band::Init);
            // Guarded definitions can retain their previous value across calls.
            if (guards) retained[i.Dst] = true;
        }
    }
    for (Reg r = 0; r < p.Regs; ++r)
        if (retained[r]) {
            Slot[r] = uint32_t(Persistent.size());
            Persistent.push_back(r);
            Init.push_back(initialized[r]);
        }
}

InstanceLayout::InstanceLayout(const Plan &p) : Registers(p) {
    FieldAt.resize(p.Fields.size());
    uint32_t at = 0;
    for (size_t f = 0; f < p.Fields.size(); ++f) {
        FieldAt[f] = at;
        const uint32_t extent = std::max<uint32_t>(1, p.Fields[f].Extent);
        if (extent > UINT32_MAX / 8 - at) throw std::runtime_error("DSP state exceeds 32-bit byte offsets");
        at += extent;
    }
    StateSize = at;

    InitWritesField.assign(p.Fields.size(), 0);
    for (const Instr &i : p.Band(Band::Init)) {
        if (Op(i.Op) == Op::StoreField && i.Imm < p.Fields.size()) InitWritesField[i.Imm] = 1;
    }
}

} // namespace faustlens
