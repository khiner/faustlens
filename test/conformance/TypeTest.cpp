// The `.type` reader, and the comparison projected from it: nature, variability and the
// interval's bounds. Computability, vectorability, boolean and lsb are not computed
// here, so they are not compared.
#include "signal/Type.h"
#include "conformance/Sweep.h"
#include "conformance/TypeParse.h"
#include "signal/Promote.h"

#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

namespace fs = std::filesystem;

void Walk(const TypeEntry &t, const std::function<void(const TypeEntry &)> &f) {
    f(t);
    for (const TypeEntry &m : t.Members) Walk(m, f);
}

char NatureChar(Nature n) { return n == Nature::Int ? 'N' : 'R'; }

char VariabilityChar(Variability v) {
    switch (v) {
        case Variability::Konst: return 'K';
        case Variability::Block: return 'B';
        case Variability::Samp: return 'S';
    }
    return '?';
}

// A tuplet prints no nature, so it is skipped while its members are not.
std::set<std::string> TheirPairs(const TypeFile &f) {
    std::set<std::string> pairs;
    for (const TypeEntry &t : f.Types)
        Walk(t, [&](const TypeEntry &e) {
            if (e.Shape != TypeEntry::Shape::Tuplet) pairs.insert(std::string{e.Nature, e.Variability});
        });
    return pairs;
}

// Only the nodes the dump has an entry for: the reference's printer skips a table's
// generator subgraph, and `Rec` prints no nature.
void WalkOurs(const Signals &s, std::span<const SigId> roots, const std::function<void(SigId)> &f) {
    Reachable(s, roots, [&](SigId id) {
        if (s.KindOf(id) != SigKind::Rec) f(id);
        return s.KindOf(id) != SigKind::Gen;
    });
}

std::set<std::string> OurPairs(const Signals &s, std::span<const SigId> roots) {
    const std::vector<Nature> nat = InferNatures(s);
    const std::vector<Variability> var = InferVariability(s);
    std::set<std::string> pairs;
    WalkOurs(s, roots, [&](SigId id) { pairs.insert(std::string{NatureChar(nat[id]), VariabilityChar(var[id])}); });
    return pairs;
}

// The dump prints bounds at default `ostream` precision, `[0, nexttoward(100, 0)]` as `0,100`.
std::string Bound(double v) {
    if (std::isnan(v)) return "nan";
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

std::string TripleOf(char nature, char variability, const Interval &i) { return std::string{nature, variability} + " " + Bound(i.Lo) + "," + Bound(i.Hi); }

std::map<std::string, int> TheirTriples(const TypeFile &f) {
    std::map<std::string, int> out;
    for (const TypeEntry &t : f.Types)
        Walk(t, [&](const TypeEntry &e) {
            if (e.Shape == TypeEntry::Shape::Tuplet) return;
            ++out[std::string{e.Nature, e.Variability} + " " + Bound(e.Lo) + "," + Bound(e.Hi)];
        });
    return out;
}

std::map<std::string, int> OurTriples(const Signals &s, std::span<const SigId> roots) {
    const std::vector<Nature> nat = InferNatures(s);
    const std::vector<Variability> var = InferVariability(s);
    const std::vector<Interval> iv = InferIntervals(s);
    std::map<std::string, int> out;
    WalkOurs(s, roots, [&](SigId id) { ++out[TripleOf(NatureChar(nat[id]), VariabilityChar(var[id]), iv[id])]; });
    return out;
}

std::string Show(const std::set<std::string> &s) {
    const std::string out = Join(s);
    return out.empty() ? "-" : out;
}

} // namespace

TEST_CASE("the `.type` reader is total over the reference corpus") {
    int files = 0;
    size_t entries = 0;
    std::vector<std::string> failures;
    std::set<std::string> codes;
    std::set<std::string> projected;
    std::map<std::string, size_t> shapes;

    for (const fs::path &p : PathsIn(OracleDir(), ".type")) {
        const auto f = ParseType(ReadText(p));
        if (!f) {
            failures.push_back(p.stem().string() + ": " + f.error());
            continue;
        }
        ++files;
        entries += f->Types.size();
        for (const TypeEntry &t : f->Types) {
            projected.insert(TypeKey(t));
            switch (t.Shape) {
                case TypeEntry::Shape::Simple: ++shapes["simple"]; break;
                case TypeEntry::Shape::Tuplet: ++shapes["tuplet"]; break;
                case TypeEntry::Shape::Table: ++shapes["table"]; break;
            }
            Walk(t, [&](const TypeEntry &n) { codes.insert(n.Code); });
        }
    }

    for (const std::string &f : failures) MESSAGE(f);
    MESSAGE("`.type` read over ", files, " files, ", entries, " entries, ", projected.size(), " distinct under the projection");
    for (const auto &[shape, n] : shapes) MESSAGE("  ", shape, ": ", n);
    MESSAGE("  headers: ", Join(codes));

    CHECK(failures.empty());
    CHECK(files == 94);

    // A two-letter header is a tuplet, and `?` is a dimension the reference left undetermined.
    const std::set<std::string> known = {
        "BE",    "SE",
        "SI", // tuplets
        "NBEV?", "NBEVN", "NBIVN", "NKCVN", "NKIV?", "NKIVN",
        "NSCS?", //
        "NSCSN", "NSCVN", "NSES?", "NSESN", "NSEV?", "NSEVN",
        "NSIS?", //
        "NSISN", "RBCVN", "RBESN", "RBEV?", "RBEVN", "RKCVN",
        "RKIV?", //
        "RKIVN", "RSCSN", "RSCVN", "RSES?", "RSESN", "RSEV?",
        "RSEVN", //
        "RSIS?", "RSISN", "RSIVN",
    };
    PinVocabulary("header", codes, known);
}

// A weak ratchet: few distinct pairs corpus-wide. Tuplets are excluded, our graph has no list node.
TEST_CASE("`(nature, variability)` pairs agree per program") {
    int agreed = 0, differed = 0;
    std::set<std::string> ours_all, theirs_all;
    Census census;

    ForEachDump<TypeFile>(".type", ParseType, census, differed, [&](const std::string &name, const TypeFile &theirs, Program &prog) {
        const std::set<std::string> mine = OurPairs(prog.Sigs, prog.Outs);
        const std::set<std::string> yours = TheirPairs(theirs);
        ours_all.insert(mine.begin(), mine.end());
        theirs_all.insert(yours.begin(), yours.end());

        if (mine == yours) {
            ++agreed;
            return;
        }
        ++differed;
        std::string reason;
        for (const std::string &q : mine)
            if (!yours.contains(q)) reason += (reason.empty() ? "ours only: " : ", ") + q;
        std::string missing;
        for (const std::string &q : yours)
            if (!mine.contains(q)) missing += (missing.empty() ? "theirs only: " : ", ") + q;
        if (!missing.empty()) reason += (reason.empty() ? "" : "; ") + missing;
        census.Add(reason, name + " -- ours " + Show(mine) + ", theirs " + Show(yours));
    });

    MESSAGE("`.type` pairs: ", agreed, " agree, ", differed, " differ");
    MESSAGE("  ours: ", Show(ours_all));
    MESSAGE("  theirs: ", Show(theirs_all));
    census.Report();

    CHECK(agreed + differed == 94);
    CHECK(agreed == 94);
}

// The reference holds entries for its signal-list spine and `sigOutput` wrapper, which we lack.
TEST_CASE("`.type` bounds agree per program") {
    int agreed = 0, differed = 0, values = 0;
    size_t theirs_total = 0, matched = 0;
    Census census;

    ForEachDump<TypeFile>(".type", ParseType, census, differed, [&](const std::string &name, const TypeFile &theirs, Program &prog) {
        const std::map<std::string, int> mine = OurTriples(prog.Sigs, prog.Outs);
        const std::map<std::string, int> yours = TheirTriples(theirs);
        for (const auto &[q, n] : yours) {
            theirs_total += n;
            const auto it = mine.find(q);
            matched += std::min(n, it == mine.end() ? 0 : it->second);
        }

        // Which triples occur asks about the rules, how many of each about the graph's shape.
        bool same_values = mine.size() == yours.size();
        for (const auto &[q, n] : mine)
            if (!yours.contains(q)) same_values = false;
        if (same_values) ++values;

        if (mine == yours) {
            ++agreed;
            return;
        }
        ++differed;
        // One witness apiece, so the census names a rule and not a program.
        std::string missing, extra;
        for (const auto &[q, n] : yours) {
            const auto it = mine.find(q);
            if ((it == mine.end() ? 0 : it->second) < n && missing.empty()) missing = q;
        }
        for (const auto &[q, n] : mine) {
            const auto it = yours.find(q);
            if ((it == yours.end() ? 0 : it->second) < n && extra.empty()) extra = q;
        }
        census.Add(std::format("short of `{}`, over on `{}`", missing.empty() ? "-" : missing, extra.empty() ? "-" : extra), name);
    });

    MESSAGE(
        "`.type` bounds: ", values, " of 94 programs agree on which triples occur, ", agreed, " on how many of each; ", matched, " of ", theirs_total,
        " reference entries matched"
    );
    census.Report();

    CHECK(agreed + differed == 94);
    CHECK(values == 94);
    // Reported and not checked: it counts nodes, we have fewer, and better faithfulness lowers it.
}
