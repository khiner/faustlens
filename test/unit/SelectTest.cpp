// The link between a file's bytes and the stages the box view draws.
#include "boxview/Select.h"
#include "property/Corpus.h"
#include "query/Query.h"
#include "unit/Diagram.h"

#include "doctest.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>

using namespace faustlens;
using namespace faustlens::boxview;
using namespace faustlens::test;

TEST_CASE("a pattern-defined process does not expose an arbitrary clause as its diagram") {
    for (const char *text : {"process(0)=1; process(x)=x;", "process(x,x)=x;", "process(1)=2;"}) {
        Session s;
        const std::string path = "/pattern.dsp";
        s.SetBuffer(path, text);
        const Snapshot snap = Publish(s, {path});
        const FileView &f = *snap.File(path);
        CHECK(ProcessBody(s.Terms, f.Root) == NoTerm);
        CHECK(ProcessBodyRef(s.Terms, f) == NoRef);
    }
}

TEST_CASE("text and diagram selection have the same occurrence ancestry") {
    Session s;
    const std::string path = "/ancestry.dsp";
    s.SetBuffer(path, "process = (_ : +(1)) , (_ : *(2));");
    const Snapshot snap = Publish(s, {path});
    const FileView &f = *snap.File(path);
    const RefId body = ProcessBodyRef(s.Terms, f);
    const Node root = Layout(s.Terms, {}).Run(ProcessBody(s.Terms, f.Root));
    auto text = SelectAt(f, root, body, uint32_t(f.Text.rfind('_')));
    auto diagram = SelectPath(f, root, body, std::vector<uint32_t>{1, 0});
    REQUIRE(text.Chain == diagram.Chain);
    SelectOut(text, f);
    SelectOut(diagram, f);
    CHECK(SelectedRef(f, text) == SelectedRef(f, diagram));
    CHECK(SelectedNode(f, root, body, text) == &root.Kids[1]);
    SelectIn(text, f);
    CHECK(SelectedNode(f, root, body, text) == &root.Kids[1].Kids[0]);
}

TEST_CASE("selection closes over the reference corpus") {
    // A composition's span begins where its first child's does, so a stage's
    // anchor must be a byte no child covers.
    size_t checked = 0, unmarked = 0, reresolved = 0;
    std::map<std::string, size_t> unmarked_by_kind, reresolved_by_kind;

    ForEachDiagram([&](Session &s, const FileView &f, ValueId, const Node &root) {
        Walk(root, [&](const Node &n) {
            ++checked;
            const std::optional<uint32_t> at = OffsetOf(f, n.Term);
            if (!at) {
                ++unmarked;
                ++unmarked_by_kind[std::string(KindName(s.Terms.KindOf(n.Term)))];
                return;
            }
            if (ValueAt(f, *at) != n.Term) {
                ++reresolved;
                ++reresolved_by_kind[std::string(KindName(s.Terms.KindOf(n.Term)))];
            }
        });
    });

    MESSAGE("checked ", checked, " stages: ", unmarked, " name no byte range, ", reresolved, " do not resolve back");
    for (const auto &[kind, n] : unmarked_by_kind) MESSAGE("  unmarked  ", kind, ": ", n);
    for (const auto &[kind, n] : reresolved_by_kind) MESSAGE("  re-resolve ", kind, ": ", n);

    CHECK(unmarked == 0);

    CHECK(reresolved == 0);
}

TEST_CASE("the selection names one occurrence of a value drawn twice") {
    Session s;
    const std::string path = "/sel.dsp";
    s.SetBuffer(path, "process = _ , _;\n");
    const Snapshot snap = Publish(s, {path});
    const FileView *f = snap.File(path);
    REQUIRE(f != nullptr);
    const ValueId body = ProcessBody(s.Terms, f->Root);
    REQUIRE(body != NoTerm);
    Layout layout(s.Terms, Metrics{});
    const Node root = layout.Run(body);
    REQUIRE(root.Kids.size() == 2);
    REQUIRE(root.Kids[0].Term == root.Kids[1].Term);

    const size_t second = f->Text.rfind('_');
    REQUIRE(second != std::string::npos);
    const Selection sel = SelectAt(*f, root, ProcessBodyRef(s.Terms, *f), uint32_t(second));
    const Node *at = SelectedNode(*f, root, ProcessBodyRef(s.Terms, *f), sel);
    REQUIRE(at != nullptr);
    CHECK(at == &root.Kids[1]);
    // Searching by value alone finds the first box instead, which is the difference.
    CHECK(Layout::Find(root, sel.Value()) == &root.Kids[0]);

    const size_t first = f->Text.find('_');
    const Selection back = SelectAt(*f, root, ProcessBodyRef(s.Terms, *f), uint32_t(first));
    CHECK(SelectedNode(*f, root, ProcessBodyRef(s.Terms, *f), back) == &root.Kids[0]);
}

TEST_CASE("every byte of a diagram's source selects a stage") {
    // A diagram collapses everything but compositions into one stage, so selection walks
    // outwards from the innermost ref.
    size_t bytes = 0, dead = 0;

    ForEachDiagram([&](Session &, const FileView &f, ValueId body, const Node &root) {
        const std::optional<uint32_t> anchor = OffsetOf(f, body);
        REQUIRE(anchor.has_value());
        const RefId body_ref = f.Refs.Innermost(*anchor);
        REQUIRE(body_ref != NoRef);
        const TermRef &br = f.Refs.Refs[body_ref];

        for (uint32_t at = br.OuterBegin; at < br.OuterEnd; ++at) {
            ++bytes;
            if (SelectAt(f, root, body_ref, at).Empty()) ++dead;
        }
    });

    MESSAGE("over ", bytes, " body bytes, ", dead, " select no stage");

    CHECK(dead == 0);
}
