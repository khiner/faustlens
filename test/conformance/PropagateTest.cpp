// Propagation over the reference corpus, and `.sig` isomorphism against the dumps.
#include "signal/Propagate.h"
#include "conformance/SigCompare.h"
#include "conformance/Sweep.h"
#include "query/Query.h"

#include "doctest.h"

#include <filesystem>
#include <format>
#include <set>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

namespace fs = std::filesystem;

int SigSweep(const char *suffix, const std::string &label, bool normalized) {
    INFO(label);
    int agreed = 0, differed = 0;
    Census census;

    ForEachDump<SigFile>(
        suffix, ParseSig, census, differed,
        [&](const std::string &name, const SigFile &theirs, Program &prog) {
            const auto same = SigIsomorphic(prog.Sigs, prog.Outs, theirs);
            if (same) {
                ++agreed;
                return;
            }
            ++differed;
            const std::string &why = same.error();
            const size_t colon = why.find(": ours is");
            census.Add(colon == std::string::npos ? why : why.substr(0, colon), name + " -- " + why);
        },
        normalized
    );

    MESSAGE("`.sig` ", label, ": ", agreed, " agree, ", differed, " differ");
    census.Report();
    CHECK(agreed + differed == 94);
    return agreed;
}

} // namespace

TEST_CASE("propagation covers the reference corpus") {
    std::vector<std::string> failures;
    int programs = 0, recursive = 0;
    size_t nodes = 0;

    // Not `Program`: the raw `Run` output is the subject, so nothing may run before the checks.
    for (const fs::path &p : DspPaths()) {
        const std::string name = p.stem().string();
        Session s;
        s.AddSearchPath(ImpulseDir() / "dsp");
        const BoxId box = s.Process(fs::weakly_canonical(p).string());
        if (s.Boxes.IsError(box)) {
            failures.push_back(name + ": did not evaluate");
            continue;
        }
        const Arity a = s.Boxes.ArityOf(box);
        if (!a.Known) {
            failures.push_back(name + ": arity is not determined");
            continue;
        }

        Signals sigs;
        Propagator prop(s.Boxes, s.Terms, sigs);
        const std::vector<SigId> outs = prop.Run(box, a.Ins);

        if (!prop.Diags.empty()) {
            failures.push_back(name + ": " + prop.Diags.front().Payload);
            continue;
        }
        if (outs.size() != size_t(a.Outs)) {
            failures.push_back(std::format("{}: {} outputs against {} promised", name, outs.size(), a.Outs));
            continue;
        }
        bool poisoned = false;
        for (const SigId o : outs) poisoned = poisoned || sigs.IsError(o);
        if (poisoned) {
            failures.push_back(name + ": an output is poisoned");
            continue;
        }

        // A reachable empty group means a reserved id escaped into the graph.
        std::vector<SigId> live;
        Reachable(sigs, outs, [&](SigId id) {
            live.push_back(id);
            return true;
        });
        for (const SigId i : live)
            if (sigs.KindOf(i) == SigKind::Rec && sigs.Get(i).ChildCount == 0) failures.push_back(name + ": an open group is reachable");

        nodes += live.size();
        for (SigId i = 0; i < sigs.Size(); ++i)
            if (sigs.KindOf(i) == SigKind::Rec) {
                ++recursive;
                break;
            }
        ++programs;
    }

    for (const std::string &f : failures) MESSAGE(f);
    CHECK(failures.empty());
    CHECK(programs == 94);
    MESSAGE("propagated ", programs, " programs, ", nodes, " reachable nodes, ", recursive, " with a recursive group");
}

// `FAUST_SIG_NO_NORM` suppresses only sum normalization, so the unnormalized dump is
// still simplified.
TEST_CASE("`.sig` isomorphism against the unnormalized dump") { CHECK(SigSweep(".nonorm.sig", "unnormalized", false) == 94); }

// The 20 that remain are exactly the watch list below.
TEST_CASE("`.sig` isomorphism against the normalized dump") { CHECK(SigSweep(".sig", "normalized", true) >= 74); }

// Association order is deferred, split by where it lands: outside a recursion the 2e-06
// comparison absorbs it.
namespace {

// Over-approximated: a constant shared into a recursion is marked too.
std::vector<uint8_t> FeedbackCone(const Signals &s) {
    std::vector<SigId> branches;
    for (SigId i = 0; i < s.Size(); ++i) {
        if (s.KindOf(i) != SigKind::Rec) continue;
        for (const SigId c : s.Children(i)) branches.push_back(c);
    }
    // `Proj` reads back into its own group, so the graph is cyclic here.
    std::vector<uint8_t> in(s.Size(), 0);
    Reachable(s, branches, [&](SigId id) {
        in[id] = 1;
        return true;
    });
    return in;
}

// Ours clamps a table index the reference proved in range and left alone.
bool IsClamp(const std::string &why) {
    return why.find("[max vs ") != std::string::npos && (why.find("sigRDTbl") != std::string::npos || why.find("sigWRTbl") != std::string::npos);
}

} // namespace

// `outside` is weaker than it looks: the comparison stops at the first disagreement.
TEST_CASE("association-order residue is recorded and classified") {
    int clamps = 0, feedback = 0, outside = 0;
    std::vector<std::string> failures, rows;
    std::set<std::string> seen;

    for (const fs::path &p : DspPaths()) {
        const std::string name = p.stem().string();
        const fs::path dump = OracleDir() / (name + ".sig");
        if (!fs::is_regular_file(dump)) continue;
        const auto theirs = ParseSig(ReadText(dump));
        if (!theirs) continue;

        Program const prog(p);
        if (!prog.Ok) continue;
        const Signals &sigs = prog.Sigs;

        SigId at = NoSig;
        const auto same = SigIsomorphic(sigs, prog.Outs, *theirs, &at);
        if (same) continue;
        const std::string &why = same.error();
        if (IsClamp(why)) {
            ++clamps;
            continue;
        }

        const bool inside = at != NoSig && FeedbackCone(sigs)[at];
        inside ? ++feedback : ++outside;
        seen.insert(name);
        rows.push_back(std::format("{}{}", inside ? "  feedback " : "  outside  ", name));

        const Deferred *d = Pinned(name);
        if (!d) failures.push_back(name + ": differs by association order, not on the watch list");
        else if (d->Feedback != inside)
            failures.push_back(
                std::format("{}: was recorded {} a feedback path, now {}", name, d->Feedback ? "inside" : "outside", inside ? "inside" : "outside")
            );
    }

    for (const Deferred &d : AssociationOrder)
        if (!seen.contains(d.Name)) failures.push_back(std::format("{}: pinned as differing, no longer does", d.Name));

    for (const std::string &r : rows) MESSAGE(r);
    for (const std::string &f : failures) MESSAGE(f);
    MESSAGE(
        "differing only by sum association order: ", feedback + outside, " programs, ", feedback, " inside a feedback path and ", outside,
        " outside. Clamps: ", clamps
    );

    CHECK(failures.empty());
    CHECK(feedback == 11);
    CHECK(outside == 9);
    // A clamp reappearing means interval analysis lost a bound it used to prove.
    CHECK(clamps == 0);
}
