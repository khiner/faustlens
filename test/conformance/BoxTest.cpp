// `.box` is ordinary Faust source, so it and the original `.dsp` both go through our
// evaluator and the check is isomorphism.
#include "conformance/BoxCompare.h"
#include "conformance/Sweep.h"
#include "query/Query.h"

#include "doctest.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

namespace fs = std::filesystem;

BoxId Compile(Session &s, const fs::path &file) {
    s.AddSearchPath(ImpulseDir() / "dsp");
    return s.Process(fs::weakly_canonical(file).string());
}

// Both differ by one `float(N)` folded on one side and not the other, in opposite
// directions. `tester.box` holds both `(65536 : float)` and `65536.0`, so it is not a
// property of the expression.
bool IsPinnedDifference(const std::string &name) { return name == "reverb_designer" || name == "zita_rev1"; }

} // namespace

TEST_CASE("`.box` isomorphism over the reference corpus") {
    REQUIRE_MESSAGE(fs::is_directory(OracleDir()), "run test/conformance/regenerate_oracle.sh first");

    std::vector<std::string> failures, missing, pins;
    int compared = 0, pinned = 0;
    for (const fs::path &p : DspPaths()) {
        const std::string name = p.stem().string();
        const fs::path box = OracleDir() / (name + ".box");
        if (!fs::is_regular_file(box)) {
            missing.push_back(name);
            continue;
        }

        Session ours;
        const BoxId a = Compile(ours, p);
        if (ours.Boxes.IsError(a)) {
            failures.push_back(name + ": .dsp did not evaluate (" + FirstError(ours) + ")");
            continue;
        }
        Session theirs;
        const BoxId b = Compile(theirs, box);
        if (theirs.Boxes.IsError(b)) {
            failures.push_back(name + ": .box did not evaluate (" + FirstError(theirs) + ")");
            continue;
        }

        // Checked for the pinned programs too: their residue is in the graph, not the header.
        if (const auto declares = SameDeclares(ours.Metadata, theirs.Metadata); !declares) {
            failures.push_back(name + ": " + declares.error());
            continue;
        }

        const BoxSide left{ours.Boxes, ours.Terms}, right{theirs.Boxes, theirs.Terms};
        const auto same = Isomorphic(left, a, right, b);
        if (IsPinnedDifference(name)) {
            // Agreement is news too: the open thread closed.
            if (same) failures.push_back(name + ": pinned difference no longer differs");
            else pins.push_back(name + ": " + same.error());
            ++pinned;
            continue;
        }
        if (!same) {
            failures.push_back(name + ": " + same.error());
            continue;
        }
        ++compared;
    }

    for (const std::string &s : pins) MESSAGE("pinned -- ", s);
    for (const std::string &s : failures) MESSAGE(s);
    MESSAGE("`.box` isomorphism over ", compared, " programs, ", pinned, " pinned as differing, ", missing.size(), " without a reference file");
    CHECK(failures.empty());
    CHECK(compared == 92);
    CHECK(pinned == 2);
}
