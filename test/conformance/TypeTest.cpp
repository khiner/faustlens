// Compare the supported projection of reference types: nature, variability, and bounds.
#include "signal/Type.h"
#include "conformance/Sweep.h"
#include "conformance/TypeParse.h"
#include "signal/Promote.h"

#include "doctest.h"

#include <cmath>
#include <filesystem>
#include <functional>
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

// Compare tuplet members because tuplets have no printed nature.
std::set<std::string> TheirPairs(const TypeFile &f) {
    std::set<std::string> pairs;
    for (const TypeEntry &t : f.Types)
        Walk(t, [&](const TypeEntry &e) {
            if (e.Shape != TypeEntry::Shape::Tuplet) pairs.insert(std::string{e.Nature, e.Variability});
        });
    return pairs;
}

// Exclude table generators and Rec nodes absent from typed dump entries.
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

// Match default ostream bound precision, which prints nexttoward(100, 0) as 100.
std::string Bound(double v) {
    if (std::isnan(v)) return "nan";
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

std::string TripleOf(char nature, char variability, const Interval &i) { return std::string{nature, variability} + " " + Bound(i.Lo) + "," + Bound(i.Hi); }

std::set<std::string> TheirTriples(const TypeFile &f) {
    std::set<std::string> out;
    for (const TypeEntry &t : f.Types)
        Walk(t, [&](const TypeEntry &e) {
            if (e.Shape == TypeEntry::Shape::Tuplet) return;
            out.insert(std::string{e.Nature, e.Variability} + " " + Bound(e.Lo) + "," + Bound(e.Hi));
        });
    return out;
}

std::set<std::string> OurTriples(const Signals &s, std::span<const SigId> roots) {
    const std::vector<Nature> nat = InferNatures(s);
    const std::vector<Variability> var = InferVariability(s);
    const std::vector<Interval> iv = InferIntervals(s);
    std::set<std::string> out;
    WalkOurs(s, roots, [&](SigId id) { out.insert(TripleOf(NatureChar(nat[id]), VariabilityChar(var[id]), iv[id])); });
    return out;
}

} // namespace

TEST_CASE("the `.type` reader is total over the reference corpus") {
    int files = 0;
    std::vector<std::string> failures;
    std::set<std::string> codes;

    for (const fs::path &p : PathsIn(OracleDir(), ".type")) {
        const auto f = ParseType(ReadText(p));
        if (!f) {
            failures.push_back(p.stem().string() + ": " + f.error());
            continue;
        }
        ++files;
        for (const TypeEntry &t : f->Types) {
            Walk(t, [&](const TypeEntry &n) { codes.insert(n.Code); });
        }
    }

    for (const std::string &f : failures) MESSAGE(f);
    CHECK(failures.empty());
    CHECK(files == 94);

    // Interpret two-letter headers as tuplets and ? as an undetermined dimension.
    const std::set<std::string> known = {
        "BE",    "SE",    "SI",    "NBEV?", "NBEVN", "NBIVN", "NKCVN", "NKIV?", "NKIVN", "NSCS?", "NSCSN", "NSCVN", "NSES?", "NSESN", "NSEV?", "NSEVN", "NSIS?",
        "NSISN", "RBCVN", "RBESN", "RBEV?", "RBEVN", "RKCVN", "RKIV?", "RKIVN", "RSCSN", "RSCVN", "RSES?", "RSESN", "RSEV?", "RSEVN", "RSIS?", "RSISN", "RSIVN",
    };
    CheckVocabulary("header", codes, known);
}

TEST_CASE("nature and variability match the reference corpus") {
    ForEachDump<TypeFile>(".type", ParseType, [](const TypeFile &reference, Program &program) {
        CHECK(OurPairs(program.Sigs, program.Outs) == TheirPairs(reference));
    });
}

TEST_CASE("type bounds match the reference corpus") {
    ForEachDump<TypeFile>(".type", ParseType, [](const TypeFile &reference, Program &program) {
        CHECK(OurTriples(program.Sigs, program.Outs) == TheirTriples(reference));
    });
}
