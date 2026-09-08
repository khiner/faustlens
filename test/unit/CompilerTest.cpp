#include "Compiler.h"
#include "Edits.h"
#include "doctest.h"
#include "syntax/Printer.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace faustlens;
using namespace faustlens::app;

namespace {

Compiler::Request Request(std::string text) {
    Compiler::Request request;
    request.Root = "/worker.dsp";
    request.Buffers[request.Root] = std::make_shared<const std::string>(std::move(text));
    request.OpenFiles = {request.Root};
    request.DocumentRevision = request.ViewRevision = 1;
    return request;
}

std::unique_ptr<Compiler::Publication> Await(Compiler &worker, uint64_t ticket) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    do {
        if (auto result = worker.Poll()) {
            REQUIRE(result->Ticket == ticket);
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    FAIL("compiler did not publish within 20 seconds");
    return {};
}

std::string Materialize(Compiler::Publication &result, const std::string &path) {
    REQUIRE_MESSAGE(result.Materialized, result.Refused);
    const auto *file = result.Snap.File(path);
    REQUIRE(file);
    Workspace ws;
    ws.Open(path, file->Text);
    REQUIRE(Apply(result.Terms, ws, *file, *result.Materialized));
    return ws.Find(path)->Text();
}

} // namespace

TEST_CASE("the compiler publishes only the latest request with independent term storage") {
    Compiler worker;
    auto first = Await(worker, worker.Submit(Request("name=1; process=name;")));
    REQUIRE(first->Audio.Status.Compiled);
    const auto *file = first->Snap.File("/worker.dsp");
    REQUIRE(file);
    const std::string printed = PrintTerm(first->Terms, file->Root);
    uint64_t ticket = 0;
    for (int i = 0; i < 80; ++i) ticket = worker.Submit(Request("process=" + std::to_string(i) + ";"));
    auto latest = Await(worker, ticket);
    REQUIRE(latest->Audio.Status.Compiled);
    CHECK(latest->Snap.File("/worker.dsp")->Text == "process=79;");
    worker.Stop();
    CHECK(PrintTerm(first->Terms, file->Root) == printed);
    // Detect dangling interned string views after worker destruction.
    for (uint32_t i = 0; i < first->Terms.Strings.Strings.size(); ++i) CHECK(first->Terms.Strings.Intern(first->Terms.Strings.At(i)) == i);
    first->Terms.Strings.Intern("only in the first UI snapshot");
    CHECK_FALSE(latest->Terms.Strings.Ids.contains("only in the first UI snapshot"));
}

TEST_CASE("worker materialization is contextual and bound to the document revision") {
    Compiler worker;
    const std::string text = "a=1; b=a with {a=2;}; process=b,a;";
    auto request = Request(text);
    auto first = Await(worker, worker.Submit(request));
    const auto *file = first->Snap.File(request.Root);
    REQUIRE(file);
    request.ViewText = text;
    request.Materialize = Innermost(*file, uint32_t(text.rfind('a')));
    SUBCASE("a selected shadowed occurrence expands in its own scope") {
        auto result = Await(worker, worker.Submit(request));
        CHECK(Materialize(*result, request.Root) == "a=1; b=a with {a=2;}; process=b,1;");
    }
    SUBCASE("same text with changed dependencies cannot use an old selection") {
        ++request.DocumentRevision;
        auto result = Await(worker, worker.Submit(request));
        CHECK_FALSE(result->Materialized);
        CHECK_FALSE(result->Refused.empty());
    }
    SUBCASE("a changed buffer cannot use the old reference tree") {
        request.Buffers[request.Root] = std::make_shared<const std::string>("process=4;");
        auto result = Await(worker, worker.Submit(request));
        CHECK_FALSE(result->Materialized);
        CHECK_FALSE(result->Refused.empty());
    }
}

TEST_CASE("worker publications retain last good audio and apply the latest controls") {
    Compiler worker;
    Live live;
    auto request = Request("process=hslider(\"gain\",0.1,0,1,0.01);");
    auto first = Await(worker, worker.Submit(request));
    REQUIRE(live.Accept(first->Audio).Compiled);
    const auto good = live.Current;
    request.Current = good;
    request.Buffers[request.Root] = std::make_shared<const std::string>("process=(;");
    auto broken = Await(worker, worker.Submit(request));
    CHECK_FALSE(live.Accept(broken->Audio).Compiled);
    CHECK(live.Current == good);
    CHECK(broken->Snap.File(request.Root)->Text == "process=(;");

    request.Buffers[request.Root] = std::make_shared<const std::string>("process=hslider(\"gain\",0.1,0,1,0.01)*2;");
    auto repaired = Await(worker, worker.Submit(request));
    REQUIRE(repaired->Audio.Next);
    controls::Values values;
    const auto &next = *repaired->Audio.Next;
    ForEachWidget(next.Ui, [&](const UiNode &widget) {
        controls::Record(values, next.Plan.Label(widget.WidgetLabel), widget, 0.75);
        return true;
    });
    REQUIRE(values.size() == 1);
    REQUIRE(live.Accept(repaired->Audio, values).Compiled);
    const auto sliders = live.Current->Dsp->ControlsOfKind(UiKind::HSlider);
    REQUIRE(sliders.size() == 1);
    CHECK(live.Current->Dsp->Control(sliders[0]) == doctest::Approx(0.75));
    CHECK(live.Current != good);
    CHECK_FALSE(live.Accept(first->Audio).Compiled);
}

TEST_CASE("coalesced requests preserve disk invalidation and opening a file preserves selection") {
    const auto dir = std::filesystem::temp_directory_path() / "faustlens_worker_import";
    std::filesystem::create_directories(dir);
    const auto library = std::filesystem::weakly_canonical(dir / "gain.lib").string();
    std::ofstream(library) << "g=0.5;";
    Compiler worker;
    Live live;
    auto request = Request("import(\"gain.lib\"); process=g;");
    request.Root = (dir / "root.dsp").string();
    request.Buffers = {{request.Root, std::make_shared<const std::string>("import(\"gain.lib\"); process=g;")}};
    request.OpenFiles = {request.Root};
    auto first = Await(worker, worker.Submit(request));
    REQUIRE(live.Accept(first->Audio).Compiled);
    const auto hash = live.Current->Hash;
    request.Current = live.Current;
    std::ofstream(library) << "g=0.25;";
    request.Touched = {library};
    worker.Submit(request);
    request.Touched.clear();
    request.OpenFiles.push_back(library);
    auto opened = Await(worker, worker.Submit(request));
    REQUIRE(opened->Audio.Next);
    CHECK(opened->Audio.Next->Hash != hash);
    REQUIRE(opened->Snap.File(library));
    CHECK(opened->Snap.File(library)->Text == "g=0.25;");
    // Opening an unchanged overlay preserves the selection's document revision.
    request.Buffers[library] = std::make_shared<const std::string>("g=0.25;");
    request.ViewText = *request.Buffers[request.Root];
    request.Materialize = Innermost(*opened->Snap.File(request.Root), uint32_t(request.ViewText.rfind('g')));
    auto expanded = Await(worker, worker.Submit(request));
    CHECK(Materialize(*expanded, request.Root).ends_with("process=0.25;"));
    worker.Stop();
    std::filesystem::remove_all(dir);
}
