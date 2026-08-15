// Plan over the corpus: totality, well-formedness, and `.fir` band parity.
#include "signal/Plan.h"
#include "conformance/FirParse.h"
#include "conformance/Sweep.h"
#include "signal/Schedule.h"

#include "doctest.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <span>
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
        // One register file across all bands, so band order is definition order.
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
        // A widget field is the interface, so it may exist unread.
        for (size_t f = 0; f < touched.size(); ++f)
            if (!touched[f] && p.Fields[f].Kind != FieldKind::Widget && p.Fields[f].Kind != FieldKind::Soundfile) Say("a field nothing reads or writes");
        if (outs != p.Outputs) Say("one output per channel is not what was emitted");
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
    // `+` and `-` differ only in `Instr::form`, and lower to identically shaped bands.
    CHECK(hash("process = _ + _;") != hash("process = _ - _;"));
    CHECK(hash("process = _ < _;") != hash("process = _ > _;"));
}

TEST_CASE("Plan: total over the corpus, and well formed") {
    int lowered = 0, clean = 0, surplus = 0;
    Census census;
    size_t regs = 0, fields = 0, ops[3] = {0, 0, 0}, loops = 0, guards = 0;
    size_t analysed_lines = 0, planned_lines = 0;
    std::map<std::string, int> by_field;
    std::set<std::string> symbols, sounds;

    for (const fs::path &path : DspPaths()) {
        const std::string name = path.stem().string();

        Program const prog(path);
        if (!prog.Ok) {
            census.Add("did not evaluate");
            continue;
        }
        const Signals &sigs = prog.Sigs;
        const std::vector<SigId> &outs = prog.Outs;

        const auto result = prog.Lower();
        if (!result) {
            census.Add("rejected: " + result.error(), name);
            continue;
        }
        const Plan &plan = *result;
        ++lowered;
        regs += plan.Regs;
        fields += plan.Fields.size();
        for (size_t b = 0; b < plan.Bands.size(); ++b) ops[b] += plan.Bands[b].size();
        std::map<std::pair<SigId, uint32_t>, int> mine;
        for (const Field &f : plan.Fields) {
            static const char *const Names[] = {"delay", "table", "widget", "soundfile", "perm"};
            ++by_field[Names[size_t(f.Kind)]];
            if (f.Loop != NoLoop) ++by_field["  of those, owned by a fill loop"];
            if (f.Kind == FieldKind::Delay && f.Loop == NoLoop) {
                ++mine[{f.Sig, f.Extent}];
                ++planned_lines;
            }
        }
        // Emission takes only the projections something reads, so only a surplus disagrees.
        if (const auto maxd = MaxDelays(sigs, InferIntervals(sigs), outs)) {
            const std::vector<DelayLine> lines = DelayLines(sigs, *maxd, InferNatures(sigs), outs);
            std::map<std::pair<SigId, uint32_t>, int> theirs;
            for (const DelayLine &l : lines) ++theirs[{l.Sig, l.Extent}];
            analysed_lines += lines.size();
            for (const auto &[k, n] : mine)
                if (theirs[k] < n) ++surplus;
        }
        for (const Instr &i : plan.Band(Band::Init))
            if (Op(i.Op) == Op::LoopBegin) ++loops;
        for (const Instr &i : plan.Band(Band::Sample))
            if (Op(i.Op) == Op::GuardBegin) ++guards;
        for (const ForeignDesc &d : plan.Foreign) {
            static const char *const Kind[] = {"fconstant", "fvariable", "ffunction"};
            std::string sig = d.Name + "(";
            for (size_t i = 0; i < d.Args.size(); ++i) sig += std::format("{}{}", i ? "," : "", d.Args[i] == Nature::Int ? "int" : "real");
            symbols.insert(std::format("{} {}) -> {}", Kind[size_t(d.Kind)], sig, d.Result == Nature::Int ? "int" : "real"));
        }
        for (const SoundfileDesc &d : plan.Soundfiles) {
            std::string urls;
            for (const std::string &u : d.Urls) urls += (urls.empty() ? "" : ";") + u;
            sounds.insert(std::format("{}: {} channels, urls [{}]", name, d.Channels, urls));
        }

        Check c;
        c.Run(plan);
        if (c.Bad.empty()) {
            ++clean;
            continue;
        }
        for (const std::string &b : c.Bad) census.Add(b, name);
    }

    MESSAGE(
        "Plan: ", lowered, " of 94 lowered, ", clean, " well formed; ", regs, " registers, ", fields, " fields, ", ops[0], "/", ops[1], "/", ops[2],
        " instructions in init/control/sample, ", loops, " fill loops, ", guards, " guards"
    );
    MESSAGE("  fields: ", JoinCounts(by_field));
    MESSAGE("  foreign symbols the registry must cover: ", symbols.size());
    for (const std::string &y : symbols) MESSAGE("    ", y);
    for (const std::string &y : sounds) MESSAGE("  soundfile ", y);
    MESSAGE(
        "  delay lines: ", planned_lines, " allocated against the analysis's ", analysed_lines, "; ", surplus,
        " programs allocate one the analysis does not have"
    );
    census.Report();

    // Reference Faust accepts all 94, so nothing here may be rejected.
    CHECK(lowered == 94);
    CHECK(clean == 94);
    CHECK(surplus == 0);
}

// Operators and math functions only. `Load`/`Store`/`Declare` just name reference temporaries.
namespace {

// The reference's spelling in ours: `max_`/`min_`/`max_i`/`min_i` lose their suffix,
// `fabs` is `abs`, `<container>_faustpower<N>_<f|i>` is `pow`, a sub container's
// lifecycle calls plumbing.
std::string TheirOp(const std::string &name, const std::string &container) {
    if (name.starts_with(std::format("{}_faustpower", container))) return "pow";
    for (const char *verb : {"new", "delete", "instanceInit", "fill"})
        if (name.starts_with(std::format("{}{}", verb, container))) return {};
    if (name == "fabs") return "abs";
    std::string n = name;
    if (n.size() > 2 && (n.ends_with("_i"))) n.resize(n.size() - 2);
    else if (n.size() > 1 && n.back() == '_') n.pop_back();
    return n;
}

using Ops = std::map<std::string, int>;

// Loop counters are skipped: the outer is the frame loop, the one nested loop a shift array.
void TheirSection(std::span<const FirStmt> stmts, const std::string &container, Ops &out, std::set<std::string> &dropped) {
    const std::function<void(const FirTerm &)> term = [&](const FirTerm &t) {
        if (t.Kind == FirTerm::Kind::Call) {
            if (t.Name == "BinopInst" && !t.Args.empty()) ++out[t.Args[0].Name];
            else if (t.Name == "Select2Inst") ++out["select"];
            else if (t.Name == "NegInst") ++out["neg"];
            else if (t.Name == "FunCallInst" || t.Name == "MethodFunCallInst") {
                const std::string op = t.Args.empty() ? std::string() : TheirOp(t.Args[0].Name, container);
                if (!op.empty()) ++out[op];
                else if (!t.Args.empty()) dropped.insert(t.Args[0].Name);
            }
        }
        for (const FirTerm &a : t.Args) term(a);
        for (const FirTerm &i : t.Index) term(i);
    };
    const std::function<void(std::span<const FirStmt>, int)> walk = [&](std::span<const FirStmt> v, int depth) {
        for (const FirStmt &s : v) {
            if (s.Term.Name == "ForLoopInst") {
                if (depth > 0) continue;
                for (const FirStmt &c : s.Body)
                    if (c.Term.Name == "BlockInst") walk(c.Body, depth + 1);
                continue;
            }
            term(s.Term);
            walk(s.Body, depth);
        }
    };
    walk(stmts, 0);
}

struct Bands {
    Ops Band[3], Gen;
};

Bands Theirs(const FirFile &f, std::set<std::string> &dropped) {
    Bands out;
    // Sub containers have no end marker, so track where they stop.
    bool sub = false;
    for (const FirSection &s : f.Sections) {
        if (s.Name == "Sub container") {
            sub = true;
            continue;
        }
        if (s.Name == "User Interface") sub = false;
        if (sub) {
            if (s.Name == "Compute DSP") TheirSection(s.Stmts, f.Container, out.Gen, dropped);
            continue;
        }
        if (s.Name == "Init") TheirSection(s.Stmts, f.Container, out.Band[0], dropped);
        else if (s.Name == "Compute control") TheirSection(s.Stmts, f.Container, out.Band[1], dropped);
        else if (s.Name == "Compute DSP" || s.Name == "Post compute DSP") TheirSection(s.Stmts, f.Container, out.Band[2], dropped);
    }
    return out;
}

Bands Ours(const Plan &p) {
    // Which registers hold minus one, so a multiply by it reads as a negation.
    std::vector<uint8_t> minus_one(p.Regs, 0);
    for (const std::vector<Instr> &code : p.Bands)
        for (const Instr &i : code) {
            if (i.Dst == NoReg) continue;
            if (Op(i.Op) == Op::ConstInt) {
                int32_t v;
                std::memcpy(&v, &i.Imm, 4);
                minus_one[i.Dst] = v == -1;
            } else if (Op(i.Op) == Op::ConstReal) {
                const uint64_t bits = uint64_t(i.Imm) | (uint64_t(i.Aux) << 32);
                double v;
                std::memcpy(&v, &bits, 8);
                minus_one[i.Dst] = v == -1.0;
            }
        }

    Bands out;
    for (const Band b : AllBands) {
        int depth = 0;
        for (const Instr &i : p.Band(b)) {
            const Op op = Op(i.Op);
            if (op == Op::LoopBegin) {
                ++depth;
                continue;
            }
            if (op == Op::LoopEnd) {
                --depth;
                continue;
            }
            Ops &into = depth > 0 ? out.Gen : out.Band[size_t(b)];
            if (op == Op::BinOp) {
                const BinOpCode c = BinOpCode(i.Form);
                const std::span<const Reg> a = p.Args(i);
                if (c == BinOpCode::Mul && a.size() == 2 && (minus_one[a[0]] || minus_one[a[1]])) ++into["neg"];
                else ++into[std::string(BinOpName(c))];
            } else if (op == Op::Extended) {
                ++into[std::string(ExtName(Ext(i.Form)))];
            } else if (op == Op::Select2 || op == Op::Select3) {
                ++into["select"];
            } else if (op == Op::SoundfileRead) {
                // The reference materializes the part offset per read, one `+` per channel.
                ++into["+"];
            } else if (op == Op::FFun) {
                ++into[p.Foreign[i.Imm].Name];
            }
        }
    }
    return out;
}

bool FirstDifference(const Ops &mine, const Ops &theirs, std::string &op, int &x, int &y) {
    std::set<std::string> keys;
    for (const auto &[k, n] : mine) keys.insert(k);
    for (const auto &[k, n] : theirs) keys.insert(k);
    for (const std::string &k : keys) {
        const auto a = mine.find(k), b = theirs.find(k);
        x = a == mine.end() ? 0 : a->second;
        y = b == theirs.end() ? 0 : b->second;
        if (x != y) {
            op = k;
            return true;
        }
    }
    return false;
}

} // namespace

// `*` marks a program on `AssociationOrder`, whose graph is not the reference's anyway.
TEST_CASE("`.fir` band projection: which band each computation landed in") {
    static const char *const BandName[] = {"init", "control", "sample"};
    int agreed = 0, differed = 0, deferred = 0;
    int by_band[3] = {0, 0, 0}, gen_agreed = 0, gen_total = 0;
    Census census;
    std::set<std::string> dropped;
    std::vector<std::string> differing, open_questions;

    ForEachDump<FirFile>(".fir", ParseFir, census, differed, [&](const std::string &name, const FirFile &fir, Program &prog) {
        const auto plan = prog.Lower();
        if (!plan) {
            ++differed;
            census.Add("rejected: " + plan.error());
            return;
        }

        const Bands theirs = Theirs(fir, dropped);
        const Bands mine = Ours(*plan);

        if (!theirs.Gen.empty() || !mine.Gen.empty()) {
            ++gen_total;
            if (theirs.Gen == mine.Gen) ++gen_agreed;
        }

        std::string reason, detail;
        for (int b = 0; b < 3; ++b) {
            std::string op;
            int x = 0, y = 0;
            if (!FirstDifference(mine.Band[b], theirs.Band[b], op, x, y)) {
                ++by_band[b];
                continue;
            }
            if (!reason.empty()) continue;
            reason = std::format("{}: `{}` {}", BandName[b], op, x > y ? "over" : "short");
            detail = std::format("{} -- {} against {}", name, x, y);
        }
        if (reason.empty()) {
            ++agreed;
            return;
        }
        ++differed;
        const bool watched = Pinned(name) != nullptr;
        deferred += watched;
        differing.push_back(name + (watched ? "*" : ""));
        census.Add(reason, detail);
        if (!watched) open_questions.push_back(detail + " on " + reason);
    });

    MESSAGE(
        "`.fir` bands: ", agreed, " of 94 agree on all three; by band ", by_band[0], "/", by_band[1], "/", by_band[2],
        " init/control/sample; generator bodies ", gen_agreed, " of ", gen_total, "; ", deferred, " of the ", differed,
        " differing are on the association-order watch list, marked *"
    );
    MESSAGE("  differing: ", Join(differing));
    for (const std::string &q : open_questions) MESSAGE("  open: ", q);
    MESSAGE("  calls read as plumbing rather than computation: ", Join(dropped));
    census.Report();

    CHECK(agreed + differed == 94);

    // A ratchet. Fourteen of the eighteen differing are on the association-order list, the
    // rest `harpe`, `phasor`, `norm1` and `math_simp`.
    CHECK(agreed >= 76);
}
