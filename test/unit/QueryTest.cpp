#include "query/Query.h"
#include "conformance/BoxCompare.h"

#include "doctest.h"

#include <format>
#include <string>

using namespace faustlens;
using namespace faustlens::test;

namespace {

std::string Shape(Session &s, BoxId b) { return PrintBox({s.Boxes, s.Terms}, b, 6); }

Session Workspace() {
    Session s;
    s.SetBuffer("/lib.lib", "gain = 2;\n");
    s.SetBuffer("/main.dsp", "import(\"/lib.lib\");\nprocess = gain;\n");
    s.SetBuffer("/unrelated.dsp", "process = 7;\n");
    return s;
}

} // namespace

TEST_CASE("an import resolves through the map, not the VFS") {
    Session s = Workspace();
    CHECK(Shape(s, s.Process("/main.dsp")) == "Int(2)");
}

TEST_CASE("editing a library file invalidates exactly its dependents") {
    Session s = Workspace();
    REQUIRE(Shape(s, s.Process("/main.dsp")) == "Int(2)");
    s.Process("/unrelated.dsp");

    const uint32_t main_before = s.Recomputes(QueryKind::FileEnv, "/main.dsp");
    const uint32_t other_before = s.Recomputes(QueryKind::FileEnv, "/unrelated.dsp");

    s.SetBuffer("/lib.lib", "gain = 3;\n");
    CHECK(Shape(s, s.Process("/main.dsp")) == "Int(3)");
    s.Process("/unrelated.dsp");

    CHECK(s.Recomputes(QueryKind::FileEnv, "/main.dsp") == main_before + 1);
    CHECK(s.Recomputes(QueryKind::FileEnv, "/unrelated.dsp") == other_before);
}

TEST_CASE("the early cutoff fires on an edit that re-derives the same terms") {
    // Cutoff equality is the value component alone. Ref spans and tokens shift on any edit.
    Session s = Workspace();
    REQUIRE(Shape(s, s.Process("/main.dsp")) == "Int(2)");
    const uint32_t before = s.Recomputes(QueryKind::FileEnv, "/main.dsp");
    const uint32_t terms_before = s.Recomputes(QueryKind::Terms, "/lib.lib");

    s.SetBuffer("/lib.lib", "\n\ngain   =   2;   // a comment\n");
    CHECK(Shape(s, s.Process("/main.dsp")) == "Int(2)");

    CHECK(s.Recomputes(QueryKind::Terms, "/lib.lib") == terms_before + 1);
    CHECK(s.Recomputes(QueryKind::FileEnv, "/main.dsp") == before);
}

TEST_CASE("a failed resolution is retried when something could have changed") {
    // Caching the failure permanently is the bug this guards.
    Session s;
    s.SetBuffer("/main.dsp", "import(\"/late.lib\");\nprocess = later;\n");
    const BoxId first = s.Process("/main.dsp");
    CHECK(s.Boxes.IsError(first));
    CHECK(Raised(s, Code::ResFileNotFound));

    s.SetBuffer("/late.lib", "later = 4;\n");
    CHECK(Shape(s, s.Process("/main.dsp")) == "Int(4)");
}

TEST_CASE("an import cycle is a diagnostic, and its result is never cached") {
    // A cycle's result depends on which file was queried first, so it stays volatile.
    for (const std::pair<const char *, const char *> entry : {std::pair{"/a.lib", "a"}, std::pair{"/b.lib", "b"}}) {
        CAPTURE(entry.first);
        Session s;
        s.SetBuffer("/a.lib", "import(\"/b.lib\");\na = 1;\n");
        s.SetBuffer("/b.lib", "import(\"/a.lib\");\nb = 2;\n");
        s.SetBuffer("/main.dsp", std::format("import(\"{}\");\nprocess = {};\n", entry.first, entry.second));
        s.Process("/main.dsp");
        CHECK(Raised(s, Code::ResImportCycle));
    }
}

TEST_CASE("an unsaved buffer shadows the file it would be saved to") {
    Session s;
    s.SetBuffer("/main.dsp", "import(\"stdfaust.lib\");\nprocess = 1;\n");
    CHECK(!s.Boxes.IsError(s.Process("/main.dsp"))); // resolves to the embedded copy
    s.SetBuffer("stdfaust.lib", "shadowed = 1;\n");
    CHECK(!s.Boxes.IsError(s.Process("/main.dsp")));
}

TEST_CASE("`Terms` carries the ref tree and the token vector past the cutoff") {
    Session s = Workspace();
    s.Process("/main.dsp");
    const TermsResult &t = s.TermsOf("/main.dsp");
    CHECK(!t.Refs.Refs.empty());
    CHECK(!t.Tokens.empty());
    CHECK(t.Refs.Refs[t.Refs.Root()].ValueId == t.Root);
}
