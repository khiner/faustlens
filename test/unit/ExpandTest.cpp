#include "Expand.h"
#include "Edits.h"
#include "boxview/Layout.h"
#include "boxview/Select.h"
#include "conformance/BoxCompare.h"
#include "query/Query.h"
#include "query/Snapshot.h"
#include "syntax/Printer.h"
#include "syntax/Splice.h"

#include "doctest.h"

#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

struct Fixture {
    Session S;
    std::string Path, Src;
    Snapshot Snap;
    boxview::Layout Layout{S.Terms, boxview::Metrics{}};
    boxview::Node Root;

    Fixture(std::string p, std::string source) : Path(std::move(p)), Src(std::move(source)) {
        S.SetBuffer(Path, Src);
        S.Process(Path);
        Snap = Publish(S, {Path});
        REQUIRE(Snap.File(Path) != nullptr);
        Root = Layout.Run(Body());
    }

    const FileView &F() const { return *Snap.File(Path); }
    ValueId Body() const { return ProcessBody(S.Terms, F().Root); }
    boxview::Selection At(std::string_view needle) const { return boxview::SelectAt(F(), Root, ProcessBodyRef(S.Terms, F()), uint32_t(Src.find(needle))); }

    RefId Ref(std::string_view needle) const { return Innermost(F(), uint32_t(Src.find(needle))); }
    Edit ExpandBody() { return app::Expand(S, F(), ProcessBodyRef(S.Terms, F())); }

    Edit Materialize(std::string_view needle) { return app::Expand(S, F(), boxview::SelectedRef(F(), At(needle))); }
    EditScript Script(const Edit &e) const { return SpliceContext(S.Terms, F().Text, F().Refs, F().Tokens).Splice(e); }
};

} // namespace

TEST_CASE("materialization preserves scope, symbolic parameters, and call specialization") {
    for (const auto &[source, needle] : std::vector<std::pair<std::string, std::string>>{
             {"a=1; b=a with {a=2;}; process=b,a;", ",a"},
             {"process(x)=x*2;", "*2"},
             {"process(x,y)=x+y;", "+y"},
             {"process= f; f(x)=x*2;", " f;"},
             {"f(x)=x*2; process=f(1),f(3);", "f(3)"},
             {"process(fl_slot1)=fl_slot1,\\(y).(y+fl_slot1);", "\\(y)"},
         }) {
        Fixture p("/scope.dsp", source);
        app::Workspace ws;
        ws.Open(p.Path, source);
        const BoxId before = p.S.Process(p.Path);
        REQUIRE_FALSE(p.S.Boxes.IsError(before));
        size_t at = source.find(needle);
        if (needle == ",a" || needle == " f;") ++at;
        auto selected = boxview::SelectAt(p.F(), p.Root, ProcessBodyRef(p.S.Terms, p.F()), uint32_t(at));
        const Edit edit = app::Expand(p.S, p.F(), boxview::SelectedRef(p.F(), selected));
        CAPTURE(source);
        REQUIRE_MESSAGE(edit, (edit.Declined ? edit.Declined : ""));
        REQUIRE(app::Apply(p.S.Terms, ws, p.F(), edit));
        p.S.SetBuffer(p.Path, ws.Find(p.Path)->Text());
        const BoxId after = p.S.Process(p.Path);
        REQUIRE_FALSE(p.S.Boxes.IsError(after));
        const auto equal = Isomorphic({p.S.Boxes, p.S.Terms}, before, {p.S.Boxes, p.S.Terms}, after);
        CHECK_MESSAGE(equal.has_value(), (equal ? "" : equal.error()));
        if (needle == ",a") CHECK(ws.Find(p.Path)->Text().ends_with("process=b,1;"));
        if (needle == "*2") CHECK(ws.Find(p.Path)->Text() == "process(x)=x,2 : *;");
    }
}

TEST_CASE("opening one of two equal stages does not open the other occurrence") {
    Fixture p("/one.dsp", "a=_*2; process=a,a;");
    const RefId body = ProcessBodyRef(p.S.Terms, p.F());
    const auto refs = p.F().Refs.Children(body);
    boxview::Layout layout(p.S.Terms, {});
    const Edit expanded = app::Expand(p.S, p.F(), refs[0]);
    REQUIRE(expanded);
    layout.Expansions = {{refs[0], expanded.Value}};
    const auto root = layout.Run(p.F().Refs, body);
    REQUIRE(root.Kids.size() == 2);
    CHECK_FALSE(root.Kids[0].Kids.empty());
    CHECK(root.Kids[1].Kids.empty());
}

TEST_CASE("expanding a node shows what it evaluates to") {
    Fixture p("/expand.dsp", "gain = 2; process = _ * gain;");
    const auto out = p.ExpandBody();
    REQUIRE(out);
    CHECK(PrintTerm(p.S.Terms, out.Value) == "_,2 : *");
}

TEST_CASE("unused definitions can be expanded in their lexical scope") {
    Fixture p("/unused.dsp", "unused = 2+3; f(x)=x*2; process = _;");
    const auto constant = app::Expand(p.S, p.F(), p.Ref("+3"));
    REQUIRE(constant);
    CHECK(PrintTerm(p.S.Terms, constant.Value) == "5");
    const auto function = app::Expand(p.S, p.F(), p.Ref("*2"));
    REQUIRE(function);
    CHECK(PrintTerm(p.S.Terms, function.Value) == "x,2 : *");
}

TEST_CASE("expansion is in place: the node stays, its children are the evaluated form") {
    Fixture p("/inplace.dsp", "gain = 2; process = _ : *(gain);");
    const ValueId stage = p.At("*(gain)").Value();
    REQUIRE(stage != NoTerm);

    const RefId ref = boxview::SelectedRef(p.F(), p.At("*(gain)"));
    const Edit e = app::Expand(p.S, p.F(), ref);
    REQUIRE(e.Value != NoTerm);

    boxview::Layout opened(p.S.Terms, boxview::Metrics{});
    opened.Expansions = {{ref, e.Value}};
    const boxview::Node after = opened.Run(p.F().Refs, ProcessBodyRef(p.S.Terms, p.F()));

    const boxview::Node *node = boxview::Layout::Find(after, stage);
    REQUIRE(node != nullptr);
    CHECK_FALSE(node->Evaluated);
    CHECK_FALSE(boxview::Layout::PathTo(after, stage).empty());
    CHECK(boxview::Layout::Find(p.Root, stage)->Kids.empty());
    REQUIRE_FALSE(node->Kids.empty());
    for (const boxview::Node &k : node->Kids) CHECK(k.Evaluated);
}

TEST_CASE("an expansion is recomputed, because the environment is what changed") {
    // A dependency edit preserves the selected value id but changes its evaluation.
    Fixture p("/recompute.dsp", "gain = 2; process = _ * gain;");
    const ValueId body = p.Body();
    CHECK(PrintTerm(p.S.Terms, p.ExpandBody().Value) == "_,2 : *");

    p.S.SetBuffer(p.Path, "gain = 3; process = _ * gain;");
    p.S.Process(p.Path);
    p.Snap = Publish(p.S, {p.Path});
    REQUIRE(p.Body() == body);
    CHECK(PrintTerm(p.S.Terms, p.ExpandBody().Value) == "_,3 : *");
}

TEST_CASE("an invalid unused expression is declined for its evaluation error") {
    Fixture p("/decline.dsp", "unused = 1 : 2; process = _;");
    const Edit e = app::Expand(p.S, p.F(), p.Ref(": 2"));
    CHECK(e.Value == NoTerm);
    CHECK(e.Declined != nullptr);
}

TEST_CASE("materialize rewrites the source to what the node evaluates to") {
    Fixture p("/materialize.dsp", "gain = 2;\nprocess = _ * gain : foo;\nfoo = _;\n");
    const Edit e = p.Materialize("_ * gain");
    REQUIRE(e.Target != NoRef);
    const EditScript script = p.Script(e);
    // Parenthesize sequential composition on the nonassociative side.
    CHECK(ApplyScript(p.Src, script) == "gain = 2;\nprocess = (_,2 : *) : foo;\nfoo = _;\n");
    for (const Replacement &r : script) {
        CHECK(r.Begin >= p.F().Refs.Refs[e.Target].OuterBegin);
        CHECK(r.End <= p.F().Refs.Refs[e.Target].OuterEnd);
    }
}

TEST_CASE("materializing what is already its evaluated form changes nothing") {
    Fixture p("/idempotent.dsp", "process = _,2 : *;\n");
    const Edit e = p.Materialize("_,2 : *");
    REQUIRE(e.Target != NoRef);
    CHECK(e.Value == p.F().Refs.Refs[e.Target].ValueId);
    CHECK(p.Script(e).empty());
}

TEST_CASE("materialize declines with the lift's own reason") {
    Fixture p("/nomat.dsp", "process = _ : undefined_name;\n");
    const Edit e = p.Materialize("undefined_name");
    CHECK(e.Target == NoRef);
    CHECK(e.Declined != nullptr);
}

TEST_CASE("materialized text compiles to the circuit it was lifted from") {
    Fixture p("/endtoend.dsp", "process = par(i, 3, _ * i);\n");
    const BoxId before = p.S.Process(p.Path);
    REQUIRE_FALSE(p.S.Boxes.IsError(before));
    const Edit e = p.Materialize("par(");
    REQUIRE(e.Target != NoRef);
    p.S.SetBuffer(p.Path, ApplyScript(p.Src, p.Script(e)));
    const BoxId after = p.S.Process(p.Path);
    REQUIRE_FALSE(p.S.Boxes.IsError(after));
    CHECK(after == before);
}
