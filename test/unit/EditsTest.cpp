#include "Edits.h"
#include "boxview/Layout.h"
#include "query/Query.h"

#include "doctest.h"

#include <string>

using namespace faustlens;
using namespace faustlens::app;

namespace {

struct Open {
    Session Session;
    std::string Path = "/edits_test.dsp";
    Workspace Ws;
    Snapshot Snap;
    boxview::Selection Sel;

    explicit Open(std::string text) {
        Ws.Open(Path, std::move(text));
        Publish();
    }

    const std::string &Text() const { return Ws.Find(Path)->Text(); }
    Buffer &Buffer() { return *Ws.Find(Path); }

    void Publish() {
        Session.SetBuffer(Path, Text());
        Session.Process(Path);
        Snap = ::faustlens::Publish(Session, {Path});
    }

    const FileView &View() const {
        const FileView *f = Snap.File(Path);
        REQUIRE(f != nullptr);
        return *f;
    }

    void SelectAt(std::string_view needle) { Caret(Text().find(needle)); }
    void SelectLast(std::string_view needle) { Caret(Text().rfind(needle)); }

    void Caret(size_t at) {
        REQUIRE(at != std::string::npos);
        const ValueId body = ProcessBody(Session.Terms, View().Root);
        REQUIRE(body != NoTerm);
        boxview::Layout layout(Session.Terms, boxview::Metrics{});
        const boxview::Node root = layout.Run(body);
        Sel = boxview::SelectAt(View(), root, ProcessBodyRef(Session.Terms, View()), uint32_t(at));
    }

    bool Commit(const Edit &e) {
        const bool ok = Apply(Session.Terms, Ws, View(), e);
        if (ok) Publish();
        return ok;
    }
    bool Do(Key key) { return Commit(EditFor(Session.Terms, View(), Sel, key)); }
    bool Drag(uint32_t in, uint32_t out) { return Commit(RewireDrag(Session.Terms, View(), Sel, in, out)); }
};

} // namespace

TEST_CASE("a key press against a selection is a term rewrite plus a splice") {
    Open f("process = a : b;\n");
    f.SelectAt("a");
    CHECK(f.Do(Key::Sequence));
    CHECK(f.Text() == "process = a : _ : b;\n");
    CHECK(f.Do(Key::Parallel));
    CHECK(f.Text() == "process = a,_ : _ : b;\n");
}

TEST_CASE("delete removes the stage the selection names") {
    Open f("process = a : b : c;\n");
    f.SelectAt("b");
    CHECK(f.Do(Key::Remove));
    CHECK(f.Text() == "process = a : c;\n");
}

TEST_CASE("an edit is one undoable unit, and undo is a text operation") {
    Open f("process = a : b;\n");
    f.SelectAt("b");
    REQUIRE(f.Do(Key::Sequence));
    REQUIRE(f.Text() == "process = a : b : _;\n");
    CHECK(f.Ws.Undo().Ok);
    CHECK(f.Text() == "process = a : b;\n");
    CHECK(f.Ws.Redo().Ok);
    CHECK(f.Text() == "process = a : b : _;\n");
}

TEST_CASE("a selection names an occurrence, not a value") {
    Open f("process = a : a;\n");
    const FileView &v = f.View();
    f.SelectAt("a");
    const RefId first = boxview::SelectedRef(v, f.Sel);
    f.SelectLast("a");
    const RefId second = boxview::SelectedRef(v, f.Sel);
    REQUIRE(first != NoRef);
    REQUIRE(second != NoRef);
    CHECK(first != second);
    CHECK(v.Refs.Refs[first].ValueId == v.Refs.Refs[second].ValueId);
    CHECK(f.Do(Key::Sequence));
    CHECK(f.Text() == "process = a : a : _;\n");
}

TEST_CASE("a click inside an application selects the stage the diagram draws") {
    // Select the widget from its argument span.
    Open f("process = hslider(\"gain\", 0, 0, 1, 0.1);\n");
    f.SelectAt("0.1");
    const FileView &v = f.View();
    const RefId stage = boxview::SelectedRef(v, f.Sel);
    REQUIRE(stage != NoRef);
    CHECK(f.Session.Terms.KindOf(v.Refs.Refs[stage].ValueId) == Kind::NumericWidget);
    CHECK(Innermost(v, f.Sel.Caret) != stage);
    CHECK(TextOf(f.Session.Terms, v, f.Sel) == "0.1");
}

TEST_CASE("Enter offers a field only where an edit can change the text") {
    Open f("process = hslider(\"gain\", 0, 0, 1, 0.1) : foo;\n");
    f.SelectAt("0.1");
    CHECK(TextOf(f.Session.Terms, f.View(), f.Sel) == "0.1");
    f.SelectAt("gain");
    CHECK(TextOf(f.Session.Terms, f.View(), f.Sel) == "\"gain\"");
    f.SelectAt("foo");
    CHECK(TextOf(f.Session.Terms, f.View(), f.Sel).empty());
}

TEST_CASE("the inline field commits through the catalogue, and declines") {
    Open f("process = hslider(\"gain\", 0, 0, 1, 0.1);\n");
    f.SelectAt("0.1");
    const Edit bad = EditForText(f.Session.Terms, f.View(), f.Sel, "banana");
    CHECK(bad.Target == NoRef);
    CHECK(bad.Declined != nullptr);
    const Edit good = EditForText(f.Session.Terms, f.View(), f.Sel, "0.01");
    REQUIRE(Apply(f.Session.Terms, f.Ws, f.View(), good));
    CHECK(f.Text() == "process = hslider(\"gain\", 0, 0, 1, 0.01);\n");
}

TEST_CASE("an edit script is refused against bytes its links do not address") {
    // Reject refs from a different source revision.
    Open f("process = a : b;\n");
    f.SelectAt("b");
    const Edit e = EditFor(f.Session.Terms, f.View(), f.Sel, Key::Sequence);
    REQUIRE(e.Target != NoRef);
    f.Buffer().Replace(0, 0, "// a line the view has never seen\n");
    const std::string before = f.Text();
    CHECK_FALSE(Apply(f.Session.Terms, f.Ws, f.View(), e));
    CHECK(f.Text() == before);
    f.Publish();
    f.SelectAt("b");
    CHECK(f.Do(Key::Sequence));
    CHECK(f.Text() == "// a line the view has never seen\nprocess = a : b : _;\n");
}

TEST_CASE("an identity edit writes nothing and costs no revision") {
    Open f("process = hslider(\"gain\", 0, 0, 1, 0.1);\n");
    f.SelectAt("0.1");
    const uint64_t before = f.Session.Revision;
    const Edit same = EditForText(f.Session.Terms, f.View(), f.Sel, "0.1");
    CHECK_FALSE(Apply(f.Session.Terms, f.Ws, f.View(), same));
    CHECK(f.Session.Revision == before);
    CHECK(f.Ws.UndoStack.size() == 0);
}

TEST_CASE("every composition key reaches its composition") {
    struct Case {
        Key Key;
        const char *Text;
    };
    for (const Case c :
         {Case{Key::Sequence, "process = a : _;\n"}, Case{Key::Parallel, "process = a,_;\n"}, Case{Key::Split, "process = a <: _;\n"},
          Case{Key::Merge, "process = a :> _;\n"}, Case{Key::Recursive, "process = a ~ _;\n"}}) {
        Open f("process = a;\n");
        f.SelectAt("a");
        CAPTURE(c.Text);
        CHECK(f.Do(c.Key));
        CHECK(f.Text() == c.Text);
    }
    CHECK(KeyForChar(':') == Key::Sequence);
    CHECK(KeyForChar(',') == Key::Parallel);
    CHECK(KeyForChar('<') == Key::Split);
    CHECK(KeyForChar('>') == Key::Merge);
    CHECK(KeyForChar('~') == Key::Recursive);
    CHECK(KeyForChar('q') == Key::None);
}

TEST_CASE("a click in the diagram selects the occurrence it drew") {
    Open f(
        "other = _, _;\n"
        "process = other <: _, _;\n"
    );
    const FileView &v = f.View();
    const ValueId body = ProcessBody(f.Session.Terms, v.Root);
    boxview::Layout layout(f.Session.Terms, boxview::Metrics{});
    const boxview::Node root = layout.Run(body);

    std::vector<uint32_t> hit;
    const boxview::Node *drawn = &root;
    while (!drawn->Kids.empty()) {
        hit.push_back(uint32_t(drawn->Kids.size() - 1));
        drawn = &drawn->Kids.back();
    }
    REQUIRE(f.Session.Terms.KindOf(drawn->Term) == Kind::Prim);

    f.Sel = boxview::SelectPath(v, root, ProcessBodyRef(f.Session.Terms, v), hit);
    const RefId at = boxview::SelectedRef(v, f.Sel);
    REQUIRE(at != NoRef);
    const size_t process_at = f.Text().find("process");
    CHECK(v.Refs.Refs[at].SpanBegin > process_at);
}

TEST_CASE("a drag between two ports toggles the connection it names") {
    Open f("process = route(2, 2, 1, 1);\n");
    f.SelectAt("route");

    CHECK(f.Drag(2, 2));
    CHECK(f.Text() == "process = route(2,2,1,1,2,2);\n");

    f.SelectAt("route");
    CHECK(f.Drag(1, 1));
    CHECK(f.Text() == "process = route(2,2,2,2);\n");
}

TEST_CASE("a drag that names no connection changes nothing") {
    Open f("process = route(2, 2, 1, 1);\n");

    SUBCASE("a selection that is not a route") {
        f.SelectAt("process");
        CHECK_FALSE(f.Drag(1, 2));
        CHECK(f.Text() == "process = route(2, 2, 1, 1);\n");
    }
    SUBCASE("the connection that is already there") {
        f.SelectAt("route");
        REQUIRE(f.Drag(1, 1));
        CHECK(f.Text() == "process = route(2,2);\n");
    }
}
