#include "conformance/BoxCompare.h"
#include "conformance/Sweep.h"

using namespace faustlens;
using namespace faustlens::test;

TEST_CASE("reference diagrams preserve metadata and channel counts") {
    const auto paths = DspPaths();
    REQUIRE(paths.size() == 94);
    for (const auto &path : paths) {
        INFO(path.string());
        const auto box = OracleDir() / (path.stem().string() + ".box");
        REQUIRE(std::filesystem::is_regular_file(box));
        const Program program(path), printed(box);
        REQUIRE(program.Ok);
        REQUIRE(printed.Ok);
        CHECK(program.Arity.Ins == printed.Arity.Ins);
        CHECK(program.Arity.Outs == printed.Arity.Outs);
        const auto declares = SameDeclares(program.Session.Metadata, printed.Session.Metadata);
        CHECK_MESSAGE(declares, (declares ? "" : declares.error()));
    }
}
