// The editor buffer and the published snapshot: diagnostic marking, cursor resolution.
#include "editor/Workspace.h"
#include "query/Snapshot.h"
#include "syntax/Parser.h"
#include "syntax/Splice.h"

#include "doctest.h"

#include <string>

using namespace faustlens;
using namespace faustlens::app;

namespace {

Snapshot Compile(Session &s, const std::string &path, const std::string &src) {
    s.SetBuffer(path, src);
    s.Process(path);
    return Publish(s, {path});
}

} // namespace

TEST_CASE("a diagnostic is marked at every occurrence of its value id") {
    // Three spellings, three marks. One spelling used three times, one mark.
    Session s;
    const std::string src = "process = missing, missing, (1 : missing);\n";
    const Snapshot snap = Compile(s, "/p.dsp", src);
    const FileView *f = snap.File("/p.dsp");
    REQUIRE(f != nullptr);

    const Diagnostic *unbound = nullptr;
    for (const Diagnostic &d : snap.Diags)
        if (d.Code == Code::EvalUnboundName) unbound = &d;
    REQUIRE(unbound != nullptr);
    CHECK(unbound->Payload == "missing");

    const std::vector<Span> marks = Marks(*f, *unbound);
    CHECK(marks.size() == 3);
    for (const Span &m : marks) CHECK(src.substr(m.Begin, m.End - m.Begin) == "missing");
    CHECK(marks[0].Begin < marks[1].Begin);
    CHECK(marks[1].Begin < marks[2].Begin);
}

TEST_CASE("a lexer diagnostic falls back to its byte range") {
    // The lexer raises before any term exists, so there is no subject to mark.
    Session s;
    const Snapshot snap = Compile(s, "/p.dsp", "process = 1; /* unterminated\n");
    const FileView *f = snap.File("/p.dsp");
    REQUIRE(f != nullptr);
    for (const Diagnostic &d : snap.Diags) {
        if (d.Code != Code::SynUnterminatedComment) continue;
        const std::vector<Span> marks = Marks(*f, d);
        REQUIRE(marks.size() == 1);
        CHECK(marks[0].Begin == d.Begin);
    }
}

TEST_CASE("a cursor resolves to the innermost ref") {
    Session s;
    const std::string src = "process = aaa : (bbb , ccc);\n";
    const Snapshot snap = Compile(s, "/p.dsp", src);
    const FileView *f = snap.File("/p.dsp");
    REQUIRE(f != nullptr);

    const auto at = [&](const char *needle, uint32_t into) {
        const auto off = uint32_t(src.find(needle) + into);
        const RefId r = Innermost(*f, off);
        REQUIRE(r != NoRef);
        const TermRef &t = f->Refs.Refs[r];
        return src.substr(t.SpanBegin, t.SpanEnd - t.SpanBegin);
    };
    CHECK(at("aaa", 1) == "aaa");
    CHECK(at("bbb", 2) == "bbb");
    CHECK(at("ccc", 0) == "ccc");
    CHECK(at(" : ", 1) == "aaa : (bbb , ccc)");
    CHECK(at("bbb , ccc", 4) == "bbb , ccc");
    CHECK(Innermost(*f, uint32_t(src.size()) + 10) == NoRef);
}

TEST_CASE("the buffer applies an edit script as one undoable unit") {
    Terms t;
    const std::string src = "process = a : b;";
    const ParseResult r = Parse(t, src);
    REQUIRE(r.Diags.empty());
    const SpliceContext ctx(t, src, r.Refs, r.Tokens);

    RefId seq = NoRef;
    for (RefId i = 0; i < r.Refs.Refs.size(); ++i)
        if (t.KindOf(r.Refs.Refs[i].ValueId) == Kind::Seq) seq = i;
    REQUIRE(seq != NoRef);
    const ValueId v = r.Refs.Refs[seq].ValueId;
    const ValueId x = t.MakeLeaf(Kind::Ident, t.InternStr("x"));
    const ValueId outer[] = {t.Child(v, 0), t.Make(Kind::Seq, {x, t.Child(v, 1)})};

    // Through the workspace, since the history spans files while a buffer is one.
    Workspace ws;
    ws.Open("/p.dsp", src);
    ws.Find("/p.dsp")->SetCursor(uint32_t(src.find('b')));
    REQUIRE(ws.Edit("/p.dsp", ctx.Splice(seq, t.Make(Kind::Seq, outer))));
    const Buffer &b = *ws.Find("/p.dsp");
    CHECK(b.Text() == "process = a : x : b;");
    // The cursor moved with the text rather than resetting.
    CHECK(b.Text().substr(b.Cursor, 1) == "b");
    CHECK(ws.UndoStack.size() == 1);

    REQUIRE(ws.Undo().Ok);
    CHECK(b.Text() == src);
    CHECK(b.Text().substr(b.Cursor, 1) == "b");
    REQUIRE(ws.Redo().Ok);
    CHECK(b.Text() == "process = a : x : b;");
}

TEST_CASE("a cursor inside a replaced region lands on its edge") {
    Buffer b("process = aaa : b;");
    b.SetCursor(11); // inside `aaa`
    EditScript script;
    script.push_back({10, 13, "z"});
    b.Apply(script);
    CHECK(b.Text() == "process = z : b;");
    CHECK(b.Cursor == 11);
}

TEST_CASE("the snapshot carries what the view reads, at one revision") {
    Session s;
    s.SetBuffer("/lib.lib", "gain = 2;\n");
    const Snapshot snap = Compile(s, "/main.dsp", "import(\"/lib.lib\");\nprocess = gain;\n");
    CHECK(snap.Revision == s.Revision);
    const FileView *f = snap.File("/main.dsp");
    REQUIRE(f != nullptr);
    CHECK(!f->Tokens.empty());
    CHECK(!f->Refs.Refs.empty());
    CHECK(f->Refs.Refs[f->Refs.Root()].ValueId == f->Root);
    // Highlighting reads this token vector rather than re-lexing, so it must tile.
    uint32_t at = 0;
    for (const Token &tok : f->Tokens) {
        if (tok.Kind == Tok::Eof) continue;
        CHECK(tok.Begin == at);
        at = tok.End;
    }
    CHECK(at == f->Text.size());
}
