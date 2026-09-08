#include "editor/Text.h"
#include "doctest.h"

using namespace faustlens;
using namespace faustlens::app;

TEST_CASE("a stale text draft cannot overwrite an external edit") {
    Workspace ws;
    ws.Open("/t.dsp", "process=_;");
    TextDraft draft;
    draft.Sync(*ws.Find("/t.dsp"));
    draft.Text = "process=1;";
    REQUIRE(ws.Edit("/t.dsp", {{8, 9, "2"}}));
    CHECK_FALSE(draft.Commit(ws, "/t.dsp"));
    CHECK(ws.Find("/t.dsp")->Text() == "process=2;");
    CHECK(ws.UndoStack.size() == 1);
    draft.Sync(*ws.Find("/t.dsp"));
    draft.Cursor = draft.Anchor = 3;
    CHECK_FALSE(draft.Commit(ws, "/t.dsp"));
    CHECK(ws.Find("/t.dsp")->Cursor == 3);
    CHECK(ws.UndoStack.size() == 1);
}
