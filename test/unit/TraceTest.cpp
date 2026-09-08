#include "Trace.h"
#include "Live.h"
#include "query/Query.h"
#include "query/Snapshot.h"
#include "unit/Ui.h"

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace faustlens;
using namespace faustlens::test;
using faustlens::app::Artifact;

namespace {

struct Fixture {
    Session Session;
    app::Live Live;
    std::string Path;
    std::filesystem::path Dir, Lib;

    Fixture(std::string p, const std::string &src) : Path(std::move(p)) {
        Session.SetBuffer(Path, src);
        REQUIRE(Live.Reload(Session, Path).Compiled);
    }

    // Use a real path for traces into imported files.
    Fixture(const char *name, const char *lib_name, const std::string &lib_src, const std::string &src)
        : Dir(std::filesystem::temp_directory_path() / name), Lib(Dir / lib_name) {
        std::filesystem::remove_all(Dir);
        std::filesystem::create_directories(Dir);
        std::ofstream(Lib) << lib_src;
        Path = (Dir / "m.dsp").string();
        std::ofstream(Path) << src;
        Session.AddSearchPath(Dir);
        Session.SetBuffer(Path, src);
        REQUIRE(Live.Reload(Session, Path).Compiled);
    }

    app::Trace Of(std::string_view label) {
        const Artifact *a = Live.Current.get();
        REQUIRE(a != nullptr);
        const UiNode *w = Widget(a->Ui, label);
        REQUIRE(w != nullptr);
        return app::TraceControl(Session, a->Plan, w->WidgetLabel);
    }

    std::vector<std::string> Text(const app::Trace &t) {
        const Snapshot snap = Publish(Session, {t.Path});
        const FileView *f = snap.File(t.Path);
        REQUIRE(f != nullptr);
        std::vector<std::string> out;
        for (const Span &sp : app::TraceMarks(*f, t)) out.push_back(f->Text.substr(sp.Begin, sp.End - sp.Begin));
        return out;
    }
};

} // namespace

TEST_CASE("a control traces back to the bytes that declare it") {
    Fixture f(
        "/s.dsp",
        "gain = hslider(\"gain\", 0.1, 0, 1, 0.01);\n"
        "process = _ * gain;\n"
    );
    const app::Trace t = f.Of("gain");
    REQUIRE(t);
    CHECK(t.Control == "gain");
    CHECK(t.Path == "/s.dsp");
    CHECK(t.Controls == 1);
    // Trace the whole widget application, including bounds.
    CHECK(f.Text(t) == std::vector<std::string>{"hslider(\"gain\", 0.1, 0, 1, 0.01)"});
}

TEST_CASE("a bargraph traces back too, and it is the other direction") {
    Fixture f("/s.dsp", "process = abs : vbargraph(\"level\", 0, 1);\n");
    const app::Trace t = f.Of("level");
    REQUIRE(t);
    CHECK(t.Controls == 1);
    CHECK(f.Text(t) == std::vector<std::string>{"vbargraph(\"level\", 0, 1)"});
}

TEST_CASE("one declaration, eight controls, and one line to show for them") {
    Fixture f("/s.dsp", "process = par(i, 8, _ * hslider(\"g%i\", 0.5, 0, 1, 0.01));\n");
    const app::Trace a = f.Of("g0"), h = f.Of("g7");
    REQUIRE(a);
    REQUIRE(h);
    CHECK(a.Terms == h.Terms);
    CHECK(a.Terms.size() == 1);
    CHECK(a.Controls == 8);
    CHECK(f.Text(a) == std::vector<std::string>{"hslider(\"g%i\", 0.5, 0, 1, 0.01)"});
    CHECK(f.Text(h) == f.Text(a));
}

TEST_CASE("a declaration written twice is marked twice") {
    Fixture f(
        "/s.dsp",
        "process = _ * hslider(\"g\", 0.5, 0, 1, 0.01),\n"
        "          _ * hslider(\"g\", 0.5, 0, 1, 0.01);\n"
    );
    const app::Trace t = f.Of("g");
    REQUIRE(t);
    CHECK(t.Terms.size() == 1);
    CHECK(t.Controls == 1);
    const std::vector<std::string> marks = f.Text(t);
    CHECK(marks.size() == 2);
    for (const std::string &m : marks) CHECK(m == "hslider(\"g\", 0.5, 0, 1, 0.01)");
}

TEST_CASE("a control declared in a library traces into the library") {
    Fixture f("faustlens_trace_lib", "amp.lib", "amp = _ * hslider(\"drive\", 0.5, 0, 1, 0.01);\n", "import(\"amp.lib\");\nprocess = amp;\n");
    const app::Trace t = f.Of("drive");
    REQUIRE(t);
    CHECK(t.Path == std::filesystem::weakly_canonical(f.Lib).string());
    CHECK(f.Text(t) == std::vector<std::string>{"hslider(\"drive\", 0.5, 0, 1, 0.01)"});
    const Snapshot both = Publish(f.Session, {f.Path, t.Path});
    CHECK(app::TraceMarks(*both.File(f.Path), t).empty());
}

TEST_CASE("interning marks call sites the program never reached, and that is the call chain's") {
    Fixture f(
        "faustlens_trace_shared", "two.lib",
        "one = _ * hslider(\"amt\", 0.5, 0, 1, 0.01);\n"
        "two = _ + hslider(\"amt\", 0.5, 0, 1, 0.01);\n",
        "import(\"two.lib\");\nprocess = one;\n"
    );
    const app::Trace t = f.Of("amt");
    REQUIRE(t);
    CHECK(t.Terms.size() == 1);
    CHECK(t.Controls == 1);
    CHECK(f.Text(t).size() == 2);
}

TEST_CASE("a trace survives the edit that moved the bytes under it") {
    Fixture f(
        "/s.dsp",
        "gain = hslider(\"gain\", 0.1, 0, 1, 0.01);\n"
        "process = _ * gain;\n"
    );
    const app::Trace t = f.Of("gain");
    REQUIRE(t);
    const uint32_t before = app::TraceMarks(*Publish(f.Session, {t.Path}).File(t.Path), t)[0].Begin;

    f.Session.SetBuffer(
        "/s.dsp",
        "// a line that was not there\n"
        "gain = hslider(\"gain\", 0.1, 0, 1, 0.01);\n"
        "process = _ * gain;\n"
    );
    REQUIRE(f.Live.Reload(f.Session, "/s.dsp").Compiled);
    const std::vector<std::string> marks = f.Text(t);
    REQUIRE(marks.size() == 1);
    CHECK(marks[0] == "hslider(\"gain\", 0.1, 0, 1, 0.01)");
    CHECK(app::TraceMarks(*Publish(f.Session, {t.Path}).File(t.Path), t)[0].Begin > before);
}

TEST_CASE("a control the plan cannot attribute answers with nothing, not with byte zero") {
    Fixture f("/s.dsp", "process = _ * hslider(\"gain\", 0.1, 0, 1, 0.01);\n");
    const Artifact *a = f.Live.Current.get();
    const app::Trace t = app::TraceControl(f.Session, a->Plan, 9999);
    CHECK_FALSE(t);
    CHECK(t.Terms.empty());
    CHECK(t.Path.empty());
    CHECK(t.Controls == 0);
}
