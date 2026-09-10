#include "signal/Propagate.h"
#include "conformance/SigCompare.h"
#include "conformance/Sweep.h"

using namespace faustlens;
using namespace faustlens::test;

TEST_CASE("propagation preserves arity and completes recursive groups across the corpus") {
    const auto paths = DspPaths();
    REQUIRE(paths.size() == 94);
    for (const auto &path : paths) {
        INFO(path.string());
        Session session;
        session.AddSearchPath(ImpulseDir() / "dsp");
        const auto box = session.Process(std::filesystem::weakly_canonical(path).string());
        REQUIRE_FALSE(session.Boxes.IsError(box));
        const auto arity = session.Boxes.ArityOf(box);
        REQUIRE(arity.Known);
        Signals signals;
        Propagator prop(session.Boxes, session.Terms, signals);
        const auto outputs = prop.Run(box, arity.Ins);
        CHECK(prop.Diags.empty());
        CHECK(outputs.size() == size_t(arity.Outs));
        for (const auto output : outputs) CHECK_FALSE(signals.IsError(output));
        Reachable(signals, outputs, [&](SigId id) {
            if (signals.KindOf(id) == SigKind::Rec) CHECK(signals.Get(id).ChildCount > 0);
            return true;
        });
    }
}

// FAUST_SIG_NO_NORM disables sum normalization while retaining simplification.
TEST_CASE("signal graphs match the unnormalized reference corpus") {
    ForEachDump<SigFile>(
        ".nonorm.sig", ParseSig,
        [](const SigFile &reference, Program &program) {
            const auto same = SigIsomorphic(program.Sigs, program.Outs, reference);
            CHECK_MESSAGE(same, (same ? "" : same.error()));
        },
        false
    );
}
