#include "query/Origins.h"
#include "doctest.h"
#include "signal/Plan.h"

using namespace faustlens;
TEST_CASE("control origins distinguish repeated declarations from iterated controls") {
    const struct {
        const char *Source, *Label;
        size_t Controls, Spans;
    } cases[]{
        {"unused=hslider(\"g\",.5,0,1,.01); process=_*hslider(\"g\",.5,0,1,.01);", "g", 1, 2}, {"process=par(i,8,_*hslider(\"g%i\",.5,0,1,.01));", "g0", 8, 1}
    };
    for (const auto &[source, label, controls, spans] : cases) {
        CAPTURE(source);
        Session session;
        session.SetBuffer("/origins.dsp", source);
        Signals signals;
        const auto plan{Graph(session, "/origins.dsp", signals).Lower()};
        REQUIRE(plan);
        const auto found{std::ranges::find(plan->Labels, label)};
        REQUIRE(found != plan->Labels.end());
        const auto origins{FindControlOrigins(session, *plan, uint32_t(found - plan->Labels.begin()))};
        CHECK(origins.Controls == controls);
        REQUIRE(origins.Files.size() == 1);
        CHECK(origins.Files[0].Spans.size() == spans);
        CHECK(origins.Ambiguous == (spans > 1));
        const auto snapshot{Publish(session, {"/origins.dsp"})};
        const auto marks{OriginSpans(*snapshot.File("/origins.dsp"), origins.Terms)};
        CHECK(marks.size() == spans);
        CHECK(std::ranges::is_sorted(marks, {}, &Span::Begin));
        CHECK(FindControlOrigins(session, *plan, UINT32_MAX).Files.empty());
    }
}
