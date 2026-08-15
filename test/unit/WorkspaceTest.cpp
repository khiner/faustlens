// One history over the whole editable state: every open file's text and the control values.
#include "editor/Workspace.h"
#include "controls/Store.h"

#include "doctest.h"

#include <string>

using namespace faustlens;
using faustlens::app::Workspace;
using faustlens::controls::Restorable;
using faustlens::controls::Value;
using faustlens::controls::Values;

namespace {

Value Slider(double v) { return {v, Restorable::Continuous}; }

bool Put(Workspace &ws, const std::string &path, uint32_t begin, uint32_t end, std::string_view with) {
    EditScript one;
    one.push_back({begin, end, std::string(with)});
    return ws.Edit(path, one);
}

} // namespace

TEST_CASE("an undo step spans every open file") {
    Workspace ws;
    ws.Open("/a.dsp", "process = _;");
    ws.Open("/lib.lib", "g = 0.5;");

    REQUIRE(Put(ws, "/a.dsp", 0, 7, "PROCESS"));
    REQUIRE(Put(ws, "/lib.lib", 4, 7, "0.25"));
    CHECK(ws.UndoStack.size() == 2);

    Workspace::Step s = ws.Undo();
    REQUIRE(s.Ok);
    CHECK(s.Texts == std::vector<std::string>{"/lib.lib"});
    CHECK(ws.Find("/lib.lib")->Text() == "g = 0.5;");
    // Unwinding is chronological, so the other file's earlier edit stands.
    CHECK(ws.Find("/a.dsp")->Text() == "PROCESS = _;");

    s = ws.Undo();
    REQUIRE(s.Ok);
    CHECK(s.Texts == std::vector<std::string>{"/a.dsp"});
    CHECK(ws.Find("/a.dsp")->Text() == "process = _;");
    CHECK_FALSE(ws.Undo().Ok);
}

TEST_CASE("a step names only the files whose bytes moved") {
    // What the caller republishes. Naming every open file costs a reparse of each.
    Workspace ws;
    ws.Open("/a.dsp", "process = _;");
    ws.Open("/b.dsp", "process = _;");
    REQUIRE(Put(ws, "/a.dsp", 0, 7, "PROCESS"));

    const Workspace::Step s = ws.Undo();
    REQUIRE(s.Ok);
    CHECK(s.Texts == std::vector<std::string>{"/a.dsp"});
}

TEST_CASE("a file opened after a step is left alone by it") {
    Workspace ws;
    ws.Open("/a.dsp", "process = _;");
    REQUIRE(Put(ws, "/a.dsp", 0, 7, "PROCESS"));
    ws.Open("/late.lib", "g = 1;");

    const Workspace::Step s = ws.Undo();
    REQUIRE(s.Ok);
    CHECK(ws.Find("/a.dsp")->Text() == "process = _;");
    CHECK(ws.Find("/late.lib")->Text() == "g = 1;");
}

TEST_CASE("one gesture is one entry") {
    Workspace ws;
    ws.Open("/a.dsp", "process = _;");

    for (double v = 0.1; v < 0.5; v += 0.1) ws.Controls["/g"] = Slider(v);
    CHECK(ws.UndoStack.size() == 0);

    CHECK(ws.CommitGesture(ws.Controls));
    CHECK(ws.UndoStack.size() == 1);
}

TEST_CASE("a gesture that returns to where it started is no entry") {
    Workspace ws;
    ws.Open("/a.dsp", "process = _;");
    ws.Controls["/g"] = Slider(0.25);
    REQUIRE(ws.CommitGesture(ws.Controls));

    ws.Controls["/g"] = Slider(0.9);
    ws.Controls["/g"] = Slider(0.25);
    CHECK_FALSE(ws.CommitGesture(ws.Controls));
    CHECK(ws.UndoStack.size() == 1);
}

TEST_CASE("undoing a gesture restores the store and asks for no recompile") {
    Workspace ws;
    ws.Open("/a.dsp", "process = _;");
    ws.Controls["/g"] = Slider(0.75);
    REQUIRE(ws.CommitGesture(ws.Controls));

    const Workspace::Step s = ws.Undo();
    REQUIRE(s.Ok);
    CHECK(s.Texts.empty());
    CHECK(ws.Find("/a.dsp")->Text() == "process = _;");
    // An entry is a state, not a delta, so an unmentioned path is a control never moved.
    CHECK(ws.Controls.empty());

    const Workspace::Step r = ws.Redo();
    REQUIRE(r.Ok);
    CHECK(r.Texts.empty());
    CHECK(ws.Controls.at("/g").V == doctest::Approx(0.75));
}

TEST_CASE("text edits and gestures unwind in reverse chronological order") {
    Workspace ws;
    ws.Open("/a.dsp", "process = _;");

    ws.Controls["/g"] = Slider(0.5);
    REQUIRE(ws.CommitGesture(ws.Controls));
    REQUIRE(Put(ws, "/a.dsp", 0, 7, "PROCESS"));
    ws.Controls["/g"] = Slider(0.9);
    REQUIRE(ws.CommitGesture(ws.Controls));
    REQUIRE(ws.UndoStack.size() == 3);

    Workspace::Step s = ws.Undo();
    CHECK(s.Texts.empty());
    CHECK(ws.Controls.at("/g").V == doctest::Approx(0.5));
    CHECK(ws.Find("/a.dsp")->Text() == "PROCESS = _;");

    s = ws.Undo();
    CHECK(s.Texts == std::vector<std::string>{"/a.dsp"});
    CHECK(ws.Find("/a.dsp")->Text() == "process = _;");
    // A text edit does not move a control, so the gesture below keeps its value.
    CHECK(ws.Controls.at("/g").V == doctest::Approx(0.5));

    s = ws.Undo();
    CHECK(s.Texts.empty());
    CHECK(ws.Controls.empty());
    CHECK_FALSE(ws.Undo().Ok);
}

TEST_CASE("entries share the files they did not change") {
    // 200 gestures over 200KB of open files, none of which is copied per entry.
    Workspace ws;
    ws.Open("/big.dsp", std::string(100000, 'x'));
    ws.Open("/also.lib", std::string(100000, 'y'));
    for (int i = 0; i < 200; ++i) {
        ws.Controls["/g"] = Slider(i);
        REQUIRE(ws.CommitGesture(ws.Controls));
    }
    CHECK(ws.UndoStack.size() == 200);
    for (int i = 0; i < 200; ++i) CHECK(ws.Undo().Texts.empty());
}

TEST_CASE("an undone text edit still names its file where the bytes match") {
    // Erring the other way would call a real edit a gesture.
    Workspace ws;
    ws.Open("/a.dsp", "process = _;");
    REQUIRE(Put(ws, "/a.dsp", 0, 1, "P"));
    REQUIRE(Put(ws, "/a.dsp", 0, 1, "p"));
    CHECK(ws.Undo().Texts == std::vector<std::string>{"/a.dsp"});
}
