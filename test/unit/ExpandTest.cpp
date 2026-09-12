#include "eval/Expand.h"
#include "conformance/BoxCompare.h"
#include "doctest.h"
#include "query/Query.h"
#include "query/Snapshot.h"
#include "syntax/Splice.h"

using namespace faustlens;

TEST_CASE("materialization preserves lexical scope and program semantics") {
    for (const auto &[source, needle] : std::vector<std::pair<std::string, std::string>>{
             {"a=1; b=a with {a=2;}; process=b,a;", "b,a"},
             {"a=1; process=a+0 with {a=2;};", "a+0"},
             {"process(x,y)=x+y;", "x+y"},
             {"f(x)=x*2; process=f(3);", "f(3)"},
             {"process=par(i,3,_*i);", "par("},
             {"process(fl_slot1)=fl_slot1,\\(y).(y+fl_slot1);", "\\(y)"}
         }) {
        CAPTURE(source);
        Session session;
        session.SetBuffer("/expand.dsp", source);
        const auto before{session.Process("/expand.dsp")};
        const auto snapshot{Publish(session, {"/expand.dsp"})};
        const auto &file{*snapshot.File("/expand.dsp")};
        const auto selected{Innermost(file, uint32_t(source.find(needle)))};
        const auto edit{Expand(session, file, selected)};
        REQUIRE_MESSAGE(edit, (edit.Declined ? edit.Declined : ""));
        const auto script{SpliceContext(session.Terms, file.Text, file.Refs, file.Tokens).Splice(edit)};
        session.SetBuffer(file.Path, ApplyScript(source, script));
        const auto after{session.Process(file.Path)};
        REQUIRE_FALSE(session.Boxes.IsError(after));
        CHECK(test::Isomorphic({session.Boxes, session.Terms}, before, {session.Boxes, session.Terms}, after).has_value());
        const auto stale{Expand(session, file, selected)};
        if (!script.empty()) CHECK_FALSE(stale);
    }
}
TEST_CASE("materialization rejects unresolved expressions and preserves an evaluated expression") {
    Session session;
    for (const auto &[source, valid] : std::vector<std::pair<std::string, bool>>{{"process=_,2:*;", true}, {"process=_:undefined_name;", false}}) {
        session.SetBuffer("/expand.dsp", source);
        const auto snapshot{Publish(session, {"/expand.dsp"})};
        const auto &file{*snapshot.File("/expand.dsp")};
        const auto edit{Expand(session, file, ProcessBodyRef(session.Terms, file))};
        CHECK(bool(edit) == valid);
        if (valid) CHECK(SpliceContext(session.Terms, file.Text, file.Refs, file.Tokens).Splice(edit).empty());
        else CHECK(edit.Declined != nullptr);
    }
}
