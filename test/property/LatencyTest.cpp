// Edit-to-audio latency by phase, reported not asserted: a ratchet measures the machine.
#include "Live.h"
#include "property/Corpus.h"
#include "query/Query.h"

#include "doctest.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

namespace fs = std::filesystem;

// The cheap edit a live editor makes: one character in one literal, touching no import.
std::string PerturbLast(const std::string &text) {
    std::string out = text;
    for (size_t i = out.size(); i-- > 0;)
        if (out[i] >= '0' && out[i] <= '8' && i > 0 && out[i - 1] != '"') {
            out[i] = char(out[i] + 1);
            return out;
        }
    return out;
}

struct Profile {
    std::string Name;
    app::Live::Timings First, Edit;
    int Fields = 0;
    int Instructions = 0;
};

Profile Measure(const std::string &name) {
    Profile p;
    p.Name = name;
    const fs::path file = ImpulseDir() / "dsp" / (name + ".dsp");
    const std::string source = ReadText(file);
    REQUIRE_FALSE(source.empty());
    const std::string path = fs::weakly_canonical(file).string();

    Session s;
    s.AddSearchPath(ImpulseDir() / "dsp");
    app::Live live;

    s.SetBuffer(path, source);
    const app::Live::Result cold = live.Reload(s, path);
    REQUIRE(cold.Compiled);
    p.First = cold.Timings;

    // The measurement: every query already answered once, asked again after one edit.
    s.SetBuffer(path, PerturbLast(source));
    const app::Live::Result warm = live.Reload(s, path);
    REQUIRE(warm.Compiled);
    CHECK_FALSE(warm.Unchanged);
    p.Edit = warm.Timings;

    const app::Artifact *a = live.Current.get();
    p.Fields = int(a->Plan.Fields.size());
    p.Instructions = 0;
    for (const std::vector<Instr> &band : a->Plan.Bands) p.Instructions += int(band.size());
    return p;
}

std::string Line(const app::Live::Timings &t) {
    char buf[256];
    std::snprintf(
        buf, sizeof buf,
        "parse %.1f evaluate %.1f propagate %.1f lower %.1f artifact %.1f "
        "instance %.1f init %.1f migrate %.1f release %.1f = %.1f ms",
        t.Parse, t.Evaluate, t.Propagate, t.Lower, t.Artifact, t.Instance, t.Init, t.Migrate, t.Release, t.Total
    );
    return buf;
}

} // namespace

TEST_CASE("edit-to-audio latency, broken out by stage") {
    // `osc` shows a reload's fixed cost next to a program with almost none of its own.
    for (const std::string &name : {"zita_rev1", "freeverb", "harpe", "osc"}) {
        const Profile p = Measure(name);
        MESSAGE(
            p.Name << " (" << p.Fields << " fields, " << p.Instructions << " instructions)\n    first: " << Line(p.First) << "\n    edit:  " << Line(p.Edit)
        );
        const app::Live::Timings &t = p.Edit;
        const double parts = t.Parse + t.Evaluate + t.Propagate + t.Lower + t.Artifact + t.Instance + t.Init + t.Migrate + t.Release;
        CHECK(parts <= t.Total + 0.001);
        CHECK(parts >= t.Total * 0.9);
    }
}
