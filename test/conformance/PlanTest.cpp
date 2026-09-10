#include "signal/Plan.h"
#include "conformance/Sweep.h"
#include "signal/Schedule.h"

#include "doctest.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

namespace fs = std::filesystem;

constexpr Band AllBands[] = {Band::Init, Band::Control, Band::Sample};

struct Check {
    std::vector<std::string> Bad;
    std::vector<uint8_t> Defined;

    void Say(const std::string &what) {
        if (!std::ranges::contains(Bad, what)) Bad.push_back(what);
    }

    void Walk(const Plan &p, Band b) {
        const std::vector<Instr> &code = p.Band(b);
        std::vector<Reg> guards;
        int loops = 0;
        for (const Instr &i : code) {
            const Op op = Op(i.Op);
            const std::string where = std::string(OpName(op));
            for (const Reg r : p.Args(i)) {
                if (r == NoReg || r >= p.Regs || !Defined[r]) {
                    Say("`" + where + "` reads a register that has no value");
                    break;
                }
            }
            switch (op) {
                case Op::LoopBegin:
                    if (b != Band::Init) Say("a bounded loop outside the init band");
                    if (i.Imm == 0) Say("a bounded loop with no bound");
                    ++loops;
                    break;
                case Op::LoopEnd:
                    if (--loops < 0) Say("a `loop.end` with no `loop.begin`");
                    break;
                case Op::GuardBegin:
                    if (i.ArgCount != 1) Say("a `guard.begin` with no condition");
                    else guards.push_back(p.Args(i)[0]);
                    break;
                case Op::GuardEnd:
                    if (guards.empty()) Say("a `guard.end` with no `guard.begin`");
                    else guards.pop_back();
                    break;
                case Op::Input:
                    if (int32_t(i.Imm) >= p.Inputs) Say("an input read past the declared channel count");
                    break;
                case Op::FConst:
                case Op::FVar:
                case Op::FFun:
                    if (i.Imm >= p.Foreign.size()) {
                        Say("`" + where + "` on a foreign symbol that does not exist");
                    } else if (op == Op::FFun && i.ArgCount != p.Foreign[i.Imm].Args.size()) {
                        Say("a foreign call whose arity is not its signature's");
                    }
                    break;
                case Op::SoundfileLength:
                case Op::SoundfileRate:
                case Op::SoundfileRead:
                    if (i.Imm >= p.Fields.size() || p.Fields[i.Imm].Kind != FieldKind::Soundfile) Say("a soundfile access on a field that is not one");
                    break;
                case Op::LoadField:
                case Op::StoreField: {
                    if (i.Imm >= p.Fields.size()) {
                        Say("`" + where + "` on a field that does not exist");
                        break;
                    }
                    const Field &f = p.Fields[i.Imm];
                    const uint32_t want = op == Op::LoadField ? 1u : 2u;
                    if (i.ArgCount > want) Say("`" + where + "` with too many operands");
                    if (i.ArgCount == want && f.Extent == 1) Say("an indexed `" + where + "` on a scalar field");
                    if (i.ArgCount < want && f.Extent != 1) Say("an unindexed `" + where + "` on an array field");
                    break;
                }
                default: break;
            }
            if (i.Dst != NoReg) {
                if (i.Dst >= p.Regs) Say("`" + where + "` writes a register out of range");
                else Defined[i.Dst] = 1;
            }
        }
        if (loops != 0) Say("a `loop.begin` with no `loop.end`");
        if (!guards.empty()) Say("a `guard.begin` with no `guard.end`");
    }

    void Run(const Plan &p) {
        Defined.assign(p.Regs, 0);
        // Check register definitions in band execution order.
        for (const Band b : AllBands) Walk(p, b);

        std::vector<uint8_t> touched(p.Fields.size(), 0);
        int outs = 0;
        for (const std::vector<Instr> &code : p.Bands)
            for (const Instr &i : code) {
                const Op op = Op(i.Op);
                if ((op == Op::LoadField || op == Op::StoreField || op == Op::SoundfileLength || op == Op::SoundfileRate || op == Op::SoundfileRead) &&
                    i.Imm < touched.size())
                    touched[i.Imm] = 1;
                if (op == Op::Output) ++outs;
            }
        // Permit unread fields exposed through the UI.
        for (size_t f = 0; f < touched.size(); ++f)
            if (!touched[f] && p.Fields[f].Kind != FieldKind::Widget && p.Fields[f].Kind != FieldKind::Soundfile) Say("a field nothing reads or writes");
        if (outs != p.Outputs) Say("output instruction count differs from the channel count");
        for (const Field &f : p.Fields) {
            if (f.Kind == FieldKind::Soundfile && f.Desc >= p.Soundfiles.size()) Say("a soundfile field with no descriptor");
            if (f.Kind == FieldKind::Table && f.Desc != NoDesc && f.Desc >= p.Waves.size()) Say("a waveform field with no samples");
        }
    }
};

} // namespace

TEST_CASE("Plan hashing separates programs that differ only in an instruction's sub-code") {
    auto hash = [](const char *source) {
        Program const prog(fs::path("form.dsp"), source);
        REQUIRE(prog.Ok);
        const auto plan = prog.Lower();
        REQUIRE(plan);
        return Hash(*plan);
    };
    // Isolate Instr::form changes with identically structured bands.
    CHECK(hash("process = _ + _;") != hash("process = _ - _;"));
    CHECK(hash("process = _ < _;") != hash("process = _ > _;"));
}

TEST_CASE("Plans are well formed across the reference corpus") {
    const auto paths = DspPaths();
    REQUIRE(paths.size() == 94);
    for (const auto &path : paths) {
        INFO(path.string());
        const Program prog(path);
        REQUIRE(prog.Ok);
        const auto result = prog.Lower();
        REQUIRE_MESSAGE(result, (result ? "" : result.error()));
        const Plan &plan = *result;
        Check check;
        check.Run(plan);
        for (const auto &error : check.Bad) FAIL_CHECK(error);

        const auto maxd = MaxDelays(prog.Sigs, InferIntervals(prog.Sigs), prog.Outs);
        REQUIRE_MESSAGE(maxd, (maxd ? "" : maxd.error()));
        std::map<std::pair<SigId, uint32_t>, int> available;
        for (const auto &line : DelayLines(prog.Sigs, *maxd, InferNatures(prog.Sigs), prog.Outs)) {
            CHECK(line.Extent > uint32_t(line.MaxDelay));
            ++available[{line.Sig, line.Extent}];
        }
        // Unused projections can reduce the allocated delay fields.
        for (const Field &field : plan.Fields)
            if (field.Kind == FieldKind::Delay && field.Loop == NoLoop) {
                INFO(field.Sig, field.Extent);
                CHECK(--available[{field.Sig, field.Extent}] >= 0);
            }
    }
}
