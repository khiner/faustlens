// Constants are not compared: the reference's `fConst` sharing analysis is one we do not have.
#include "signal/Schedule.h"
#include "conformance/FirParse.h"
#include "conformance/Sweep.h"

#include "doctest.h"

#include <expected>
#include <format>
#include <map>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

// Reference field names carry class and nature: `?Rec` is recursive history, `?Vec` a delay.
bool IsDelayField(const std::string &name, char &nature) {
    static const char *Prefixes[] = {"fRec", "iRec", "fVec", "iVec"};
    for (const char *p : Prefixes) {
        const std::string s = p;
        if (name.starts_with(s)) {
            nature = name[0];
            return true;
        }
    }
    return false;
}

// Names are not compared: the reference numbers its fields in emission order.
std::string LineKey(char nature, uint32_t extent) { return std::format("{}[{}]", nature, extent); }

// A table program emits a `DSP struct` per sub container, so read the one named for the container.
// `iota` is a second result, not an error: the reference names its counter `IOTA0`.
std::expected<std::map<std::string, int>, std::string> TheirLines(const FirFile &f, bool &iota) {
    std::map<std::string, int> out;
    bool found = false;
    for (const FirSection &s : f.Sections) {
        if (s.Name != "DSP struct") continue;
        for (const FirStmt &st : s.Stmts) {
            if (st.Term.Name != "DeclareStructTypeInst" || st.Term.Args.empty()) continue;
            const FirTerm &ty = st.Term.Args[0];
            if (ty.Name != "StructType" || ty.Args.empty()) continue;
            if (ty.Args[0].Name != f.Container) continue;
            found = true;
            for (size_t i = 1; i < ty.Args.size(); ++i) {
                const FirTerm &pair = ty.Args[i];
                if (pair.Kind != FirTerm::Kind::Call || pair.Args.size() != 2) continue;
                const std::string &decl = pair.Args[0].Name;
                const std::string &name = pair.Args[1].Name;
                // The name is `IOTA0`, not `IOTA`.
                if (name.starts_with("IOTA")) iota = true;
                char nature = 'f';
                if (!IsDelayField(name, nature)) continue;
                const size_t br = decl.find('[');
                if (br == std::string::npos) return std::unexpected("delay field `" + name + "` has no extent");
                ++out[LineKey(nature, std::stoul(decl.substr(br + 1)))];
            }
        }
    }
    if (!found) return std::unexpected("no `DSP struct` for container `" + f.Container + "`");
    return out;
}

} // namespace

TEST_CASE("`.fir` projection: delay lines and their shapes") {
    int agreed = 0, differed = 0, rejected = 0;
    size_t lines = 0, scalars = 0, rings = 0;
    Census census;
    std::map<int32_t, int> by_max;
    std::vector<std::string> differing;

    ForEachDump<FirFile>(".fir", ParseFir, census, differed, [&](const std::string &name, const FirFile &fir, Program &prog) {
        bool their_iota = false;
        const auto read = TheirLines(fir, their_iota);
        if (!read) {
            ++differed;
            census.Add(read.error());
            return;
        }
        const std::map<std::string, int> &theirs = *read;
        const Signals &sigs = prog.Sigs;
        const std::vector<SigId> &outs = prog.Outs;

        // Reference Faust accepted all 94, so any rejection here is ours.
        const auto maxd = MaxDelays(sigs, InferIntervals(sigs), outs);
        if (!maxd) {
            ++differed;
            ++rejected;
            census.Add("rejected: " + maxd.error(), name);
            return;
        }

        const std::vector<DelayLine> ours = DelayLines(sigs, *maxd, InferNatures(sigs), outs);
        std::map<std::string, int> mine;
        bool our_iota = false;
        for (const DelayLine &l : ours) {
            ++mine[LineKey(l.Nature == Nature::Int ? 'i' : 'f', l.Extent)];
            our_iota = our_iota || l.Ring;
            ++by_max[l.MaxDelay];
            if (l.Ring) ++rings;
        }
        lines += ours.size();
        for (const int32_t d : *maxd)
            if (d == 0) ++scalars;

        if (mine == theirs && our_iota == their_iota) {
            ++agreed;
            return;
        }
        ++differed;
        differing.push_back(name);
        std::string missing, extra;
        for (const auto &[k, n] : theirs) {
            const auto it = mine.find(k);
            if ((it == mine.end() ? 0 : it->second) < n && missing.empty()) missing = k;
        }
        for (const auto &[k, n] : mine) {
            const auto it = theirs.find(k);
            if ((it == theirs.end() ? 0 : it->second) < n && extra.empty()) extra = k;
        }
        std::string blame;
        for (const DelayLine &l : ours)
            if (LineKey(l.Nature == Nature::Int ? 'i' : 'f', l.Extent) == extra) {
                blame = std::format(" on {} @{}", SigKindName(sigs.KindOf(l.Sig)), l.MaxDelay);
                break;
            }
        std::string reason = std::format("short of `{}`, over on `{}`{}", missing.empty() ? "-" : missing, extra.empty() ? "-" : extra, blame);
        if (our_iota != their_iota) reason += our_iota ? "; we ring where they do not" : "; they ring where we do not";
        std::string what;
        for (const DelayLine &l : ours) what += std::format(" {}@{}", SigKindName(sigs.KindOf(l.Sig)), l.MaxDelay);
        census.Add(reason, std::format("{} -- ours {} | theirs {}{}", name, JoinCounts(mine), JoinCounts(theirs), ours.size() <= 8 ? " |" + what : ""));
    });

    MESSAGE(
        "`.fir` delay lines: ", agreed, " of 94 agree, ", differed, " differ (", rejected, " rejected for an unbounded index); ", lines, " lines allocated, ",
        rings, " of them rings, ", scalars, " signals needing no array"
    );
    std::string spread;
    for (const auto &[d, n] : by_max) spread += std::format("{}{}×{}", spread.empty() ? "" : " ", d, n);
    MESSAGE("  by maximum delay: ", spread);
    MESSAGE("  differing: ", Join(differing));
    census.Report();

    CHECK(agreed + differed == 94);
    // Not a ratchet: a rejection is a program reference Faust compiles and we do not.
    CHECK(rejected == 0);

    // A ratchet. Ten of the eleven differences are on `propagate_test.cpp`'s association-order
    // list. `harpe` is the exception: isomorphic graph, twice the lines.
    CHECK(agreed >= 83);
}
