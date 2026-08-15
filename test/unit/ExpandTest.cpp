#include "Expand.h"
#include "boxview/Layout.h"
#include "boxview/Select.h"
#include "eval/Lift.h"
#include "property/Corpus.h"
#include "query/Query.h"
#include "query/Snapshot.h"
#include "syntax/Printer.h"
#include "syntax/Splice.h"
#include "unit/Diagram.h"

#include "doctest.h"

#include <filesystem>
#include <map>
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
    ValueId ValueOf(std::string_view needle) const { return ValueAt(F(), uint32_t(Src.find(needle))); }
    boxview::Selection At(std::string_view needle) const { return boxview::SelectAt(F(), Root, uint32_t(Src.find(needle))); }

    Edit Materialize(std::string_view needle) { return app::Materialize(S, F(), At(needle)); }
    EditScript Script(const Edit &e) const { return SpliceContext(S.Terms, F().Text, F().Refs, F().Tokens).Splice(e.Target, e.Value); }
};

} // namespace

TEST_CASE("how often a drawn stage was evaluated under more than one environment") {
    size_t stages = 0, once = 0, several = 0, never = 0;
    std::map<std::string, size_t> several_by_kind;

    ForEachDiagram([&](Session &s, const FileView &f, ValueId, const boxview::Node &root) {
        s.Process(f.Path);
        Walk(root, [&](const boxview::Node &n) {
            ++stages;
            const Evaluator::Evaluated e = s.Eval.EvaluatedIn(n.Term);
            if (e.Envs == 0) ++never;
            else if (!e.Ambiguous) ++once;
            else {
                ++several;
                several_by_kind[std::string(KindName(s.Terms.KindOf(n.Term)))] += 1;
            }
        });
    });

    for (const auto &[kind, n] : several_by_kind) MESSAGE("  several: ", kind, ": ", n);
    MESSAGE(stages, " drawn stages: ", once, " whose environments agree, ", several, " where they differ, ", never, " never reached");
    // Taking the first environment is a rule, not a guess.
    CHECK(stages == 5992);
    CHECK(never == 0);
    CHECK(several == 16);
}

TEST_CASE("expanding a node shows what it evaluates to") {
    Fixture p("/expand.dsp", "gain = 2; process = _ * gain;");
    // `gain` is beta-reduced away, which is the evaluated view's lossiness.
    const Evaluator::Evaluated e = p.S.Eval.EvaluatedIn(p.Body());
    REQUIRE(e.Envs == 1);
    const Lifted out = Lift(p.S.Terms, p.S.Boxes, p.S.Eval.Eval(p.Body(), e.Env));
    REQUIRE(out.Term != NoTerm);
    CHECK(PrintTerm(p.S.Terms, out.Term) == "_,2 : *");
}

TEST_CASE("a stage the program never reached has no evaluated form") {
    Fixture const p("/unreached.dsp", "unused = 1 : 2; process = _;");
    const ValueId unused = p.ValueOf("1 : 2");
    REQUIRE(unused != NoTerm);
    CHECK(p.S.Eval.EvaluatedIn(unused).Envs == 0);
}

TEST_CASE("expansion is in place: the node stays, its children are the evaluated form") {
    Fixture p("/inplace.dsp", "gain = 2; process = _ : *(gain);");
    const ValueId stage = p.At("*(gain)").Value();
    REQUIRE(stage != NoTerm);

    const app::Expansion e = app::Expand(p.S, stage);
    REQUIRE(e.Term != NoTerm);
    CHECK_FALSE(e.Ambiguous);

    boxview::Layout opened(p.S.Terms, boxview::Metrics{});
    opened.Expansions = {{stage, e.Term}};
    const boxview::Node after = opened.Run(p.Body());

    // Still drawn under its own term, so selection and linking are unchanged.
    const boxview::Node *node = boxview::Layout::Find(after, stage);
    REQUIRE(node != nullptr);
    CHECK_FALSE(node->Evaluated);
    CHECK_FALSE(boxview::Layout::PathTo(after, stage).empty());
    CHECK(boxview::Layout::Find(p.Root, stage)->Kids.empty());
    REQUIRE_FALSE(node->Kids.empty());
    for (const boxview::Node &k : node->Kids) CHECK(k.Evaluated);
}

TEST_CASE("an expansion is recomputed, because the environment is what changed") {
    // The drawn value does not move when its definitions do, so a value-keyed cache would stale.
    Fixture p("/recompute.dsp", "gain = 2; process = _ * gain;");
    const ValueId body = p.Body();
    CHECK(PrintTerm(p.S.Terms, app::Expand(p.S, body).Term) == "_,2 : *");

    p.S.SetBuffer(p.Path, "gain = 3; process = _ * gain;");
    p.S.Process(p.Path);
    p.Snap = Publish(p.S, {p.Path});
    REQUIRE(p.Body() == body);
    CHECK(PrintTerm(p.S.Terms, app::Expand(p.S, body).Term) == "_,3 : *");
}

TEST_CASE("expanding what the program does not reach is declined with a reason") {
    Fixture p("/decline.dsp", "unused = 1 : 2; process = _;");
    const app::Expansion e = app::Expand(p.S, p.ValueOf("1 : 2"));
    CHECK(e.Term == NoTerm);
    CHECK(e.Declined != nullptr);
}

TEST_CASE("materialize rewrites the source to what the node evaluates to") {
    Fixture p("/materialize.dsp", "gain = 2;\nprocess = _ * gain : foo;\nfoo = _;\n");
    const Edit e = p.Materialize("_ * gain");
    REQUIRE(e.Target != NoRef);
    const EditScript script = p.Script(e);
    // The parentheses are needed: the desugared `:` sits at the level of the `:` holding it,
    // on the side right-associativity disfavours.
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
