// State continuity across a live reload, compared against an instance that never reloaded.
#include "Live.h"
#include "query/Query.h"
#include "query/Snapshot.h"
#include "runtime/Interp.h"
#include "runtime/Migrate.h"
#include "signal/Plan.h"
#include "signal/Ui.h"

#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace faustlens;
using faustlens::app::Artifact;

namespace {

struct Instance {
    Session Session;
    Signals Sigs;
    Plan Plan;
    UiNode Ui;
    std::vector<uint32_t> At;
    std::optional<Interp> Dsp;

    void Run(int32_t frames, double impulse, std::vector<double> &out) {
        std::vector<double> in(frames, 0.0);
        if (impulse != 0) in[0] = impulse;
        out.assign(frames, 0.0);
        const double *const ip[] = {in.data()};
        double *const op[] = {out.data()};
        Dsp->Compute(frames, Dsp->Inputs() > 0 ? ip : nullptr, op);
    }
};

std::unique_ptr<Instance> Build(const std::string &src) {
    auto i = std::make_unique<Instance>();
    i->Session.SetBuffer("/r.dsp", src);
    const Graph g(i->Session, "/r.dsp", i->Sigs);
    REQUIRE(g.Ok);
    auto plan = g.Lower();
    REQUIRE(plan);
    i->Plan = *std::move(plan);
    i->Ui = g.Ui("r");

    // The ref tree is rebuilt on every reparse, so take the offsets before the old one goes.
    const Snapshot snap = Publish(i->Session, {"/r.dsp"});
    REQUIRE(snap.File("/r.dsp") != nullptr);
    i->At = app::FieldOffsets(i->Plan, snap.File("/r.dsp"));

    i->Dsp.emplace(i->Plan, i->Ui);
    i->Dsp->Init(44100);
    return i;
}

} // namespace

TEST_CASE("a no-op edit is sample-identical across the reload") {
    const std::string src = "process = (+ : *(0.9)) ~ _;";
    const std::unique_ptr<Instance> live = Build(src);
    std::vector<double> warm;
    live->Run(64, 1.0, warm);

    const std::unique_ptr<Instance> reloaded = Build(src);
    const Migration m = Migrate(live->Plan, *live->Dsp, live->At, reloaded->Plan, *reloaded->Dsp, reloaded->At);
    CHECK(m.Exact > 0);
    CHECK(m.Fresh == 0);
    CHECK(m.Shaped == 0);

    std::vector<double> kept, went;
    live->Run(64, 0.0, kept);
    reloaded->Run(64, 0.0, went);
    CHECK(kept == went);
    CHECK(kept[0] != 0.0);
}

TEST_CASE("a gain edit inside a feedback network does not cost the tail") {
    // A `Rec` hash covers its whole body, so only the shape pass can match a constant edit.
    const std::unique_ptr<Instance> live = Build("process = (+ : *(0.9)) ~ _;");
    std::vector<double> warm;
    live->Run(64, 1.0, warm);
    const double level = warm.back();
    REQUIRE(std::fabs(level) > 1e-3);

    const std::unique_ptr<Instance> edited = Build("process = (+ : *(0.8)) ~ _;");
    const Migration m = Migrate(live->Plan, *live->Dsp, live->At, edited->Plan, *edited->Dsp, edited->At);
    CHECK(m.Shaped > 0);
    CHECK(m.Exact == 0);
    CHECK(m.Fresh == 0);

    std::vector<double> tail;
    edited->Run(1, 0.0, tail);
    // The tail continues at the new coefficient: one frame on from `level`.
    CHECK(tail[0] == doctest::Approx(level * 0.8));

    // What it would have been without the pass.
    const std::unique_ptr<Instance> cold = Build("process = (+ : *(0.8)) ~ _;");
    std::vector<double> silence;
    cold->Run(1, 0.0, silence);
    CHECK(silence[0] == 0.0);
}

TEST_CASE("a widget's state is not this pass's to carry") {
    // An edit to a declared `init` must be heard, so carrying the old value would swallow it.
    const std::unique_ptr<Instance> live = Build(
        "gain = hslider(\"gain\", 0.1, 0, 1, 0.01);\n"
        "process = _ * gain;\n"
    );
    const std::vector<uint32_t> sliders = live->Dsp->ControlsOfKind(UiKind::HSlider);
    REQUIRE(sliders.size() == 1);
    live->Dsp->SetControl(sliders[0], 0.75);

    const std::unique_ptr<Instance> edited = Build(
        "gain = hslider(\"gain\", 0.1, 0, 1, 0.01);\n"
        "process = _ * gain * 2.0;\n"
    );
    Migrate(live->Plan, *live->Dsp, live->At, edited->Plan, *edited->Dsp, edited->At);
    const std::vector<uint32_t> after = edited->Dsp->ControlsOfKind(UiKind::HSlider);
    REQUIRE(after.size() == 1);
    CHECK(edited->Dsp->Control(after[0]) == doctest::Approx(0.1));
}

TEST_CASE("a lengthened delay keeps the history it had") {
    // The length comes off the edge, not the node, so identity holds when the line grows.
    const std::unique_ptr<Instance> live = Build("process = _ @ 8;");
    std::vector<double> warm;
    live->Run(4, 1.0, warm); // the impulse is 4 frames back and 4 frames out

    const std::unique_ptr<Instance> longer = Build("process = _ @ 8;");
    const Migration same = Migrate(live->Plan, *live->Dsp, live->At, longer->Plan, *longer->Dsp, longer->At);
    CHECK(same.Resized == 0);

    const std::unique_ptr<Instance> grown = Build("process = _ @ 64;");
    const Migration m = Migrate(live->Plan, *live->Dsp, live->At, grown->Plan, *grown->Dsp, grown->At);
    CHECK(m.Exact > 0);
    CHECK(m.Resized == 1);

    // `@8` is a nine-slot copy line and `@64` a 128 ring, so the impulse lands at 4 and at 60.
    std::vector<double> kept;
    live->Run(8, 0.0, kept);
    CHECK(kept[4] == doctest::Approx(1.0));
    std::vector<double> held;
    grown->Run(64, 0.0, held);
    CHECK(held[60] == doctest::Approx(1.0));
}

TEST_CASE("a program that does not compile is not swapped in: the last good one keeps playing") {
    Session s;
    app::Live live;
    s.SetBuffer("/l.dsp", "process = _ * 0.5;");
    const app::Live::Result first = live.Reload(s, "/l.dsp");
    REQUIRE(first.Compiled);
    REQUIRE(live.Current.get() != nullptr);
    const uint64_t good = live.Current->Hash;

    SUBCASE("a syntactically broken edit changes nothing that plays") {
        s.SetBuffer("/l.dsp", "process = _ * ;");
        const app::Live::Result r = live.Reload(s, "/l.dsp");
        CHECK_FALSE(r.Compiled);
        CHECK_FALSE(r.Swapped);
        CHECK_FALSE(r.Why.empty());
        CHECK(live.Current.get()->Hash == good);
    }
    SUBCASE("an arity error is the same answer") {
        s.SetBuffer("/l.dsp", "process = _ , _ : + : + ;");
        const app::Live::Result r = live.Reload(s, "/l.dsp");
        CHECK_FALSE(r.Compiled);
        CHECK(live.Current.get()->Hash == good);
    }
    SUBCASE("a missing `process` is the same answer") {
        s.SetBuffer("/l.dsp", "notprocess = _;");
        const app::Live::Result r = live.Reload(s, "/l.dsp");
        CHECK_FALSE(r.Compiled);
        CHECK(live.Current.get()->Hash == good);
    }
    SUBCASE("and the next good edit does swap") {
        s.SetBuffer("/l.dsp", "process = _ * ;");
        live.Reload(s, "/l.dsp");
        s.SetBuffer("/l.dsp", "process = _ * 0.25;");
        const app::Live::Result r = live.Reload(s, "/l.dsp");
        CHECK(r.Compiled);
        CHECK_FALSE(r.Unchanged);
        CHECK(live.Current.get()->Hash != good);
    }
}

TEST_CASE("an edit that does not change the compiled program skips the swap entirely") {
    Session s;
    app::Live live;
    s.SetBuffer("/l.dsp", "process = (+ : *(0.5)) ~ _;");
    REQUIRE(live.Reload(s, "/l.dsp").Compiled);
    const Artifact *was = live.Current.get();

    SUBCASE("the same text is the same Plan") {
        s.SetBuffer("/l.dsp", "process = (+ : *(0.5)) ~ _;");
        const app::Live::Result r = live.Reload(s, "/l.dsp");
        CHECK(r.Unchanged);
        CHECK_FALSE(r.Swapped);
        CHECK(live.Current.get() == was); // not even rebuilt
    }
    SUBCASE("whitespace and comments are not the program") {
        // The Plan hash leaves out `Field::origin`, or this would swap on a comment.
        s.SetBuffer(
            "/l.dsp",
            "// a note to self\n"
            "\n"
            "process = (+ : *(0.5))   ~ _;\n"
        );
        const app::Live::Result r = live.Reload(s, "/l.dsp");
        CHECK(r.Unchanged);
        CHECK(live.Current.get() == was);
    }
    SUBCASE("a renamed intermediate is not the program either") {
        s.SetBuffer("/l.dsp", "loop(x) = x * 0.5;\nprocess = (+ : loop) ~ _;");
        const app::Live::Result r = live.Reload(s, "/l.dsp");
        CHECK(r.Unchanged);
    }
    SUBCASE("a changed coefficient is") {
        s.SetBuffer("/l.dsp", "process = (+ : *(0.6)) ~ _;");
        const app::Live::Result r = live.Reload(s, "/l.dsp");
        CHECK_FALSE(r.Unchanged);
        CHECK(r.Compiled);
        // With no device the instance is replaced in place, so the migration ran.
        CHECK(r.Migration.Shaped > 0);
    }
}

TEST_CASE("an edit to an imported file reaches the loop") {
    // A library's parse is memoized against a `ChangedAt` only this session moves.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "faustlens_import_reload";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path lib = dir / "gain.lib", main = dir / "m.dsp";
    // The resolution key is the canonical path, not what `temp_directory_path` hands out.
    const std::string lib_key = std::filesystem::weakly_canonical(lib).string();
    const auto write = [](const std::filesystem::path &p, const std::string &text) { std::ofstream(p) << text; };
    write(lib, "g = 0.5;\n");
    write(main, "import(\"gain.lib\");\nprocess = _ * g;\n");

    Session s;
    s.AddSearchPath(dir);
    app::Live live;
    s.SetBuffer(main.string(), "import(\"gain.lib\");\nprocess = _ * g;\n");
    REQUIRE(live.Reload(s, main.string()).Compiled);
    const uint64_t before = live.Current->Hash;
    const std::vector<std::string> parsed = s.Parsed();
    CHECK(std::ranges::contains(parsed, lib_key));

    SUBCASE("without being told, the library edit is invisible") {
        write(lib, "g = 0.25;\n");
        CHECK(live.Reload(s, main.string()).Unchanged);
        CHECK(live.Current.get()->Hash == before);
    }
    SUBCASE("an unsaved buffer over the library is heard without touching disk") {
        s.SetBuffer(lib_key, "g = 0.25;\n");
        const app::Live::Result r = live.Reload(s, main.string());
        CHECK(r.Compiled);
        CHECK_FALSE(r.Unchanged);
        CHECK(live.Current.get()->Hash != before);
        CHECK(ReadFile(lib) == "g = 0.5;\n"); // the buffer did it, not the disk

        s.ClearBuffer(lib_key);
        CHECK(live.Reload(s, main.string()).Compiled);
        CHECK(live.Current.get()->Hash == before);
    }
    SUBCASE("`Touch` is what makes it visible") {
        write(lib, "g = 0.25;\n");
        s.Touch(lib_key);
        const app::Live::Result r = live.Reload(s, main.string());
        CHECK(r.Compiled);
        CHECK_FALSE(r.Unchanged);
        CHECK(live.Current.get()->Hash != before);
    }
    std::filesystem::remove_all(dir);
}
