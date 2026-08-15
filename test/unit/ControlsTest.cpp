// Widget metadata and the position-to-value curves of the reference's `gui/ValueConverter.h`.
#include "controls/Style.h"

#include "doctest.h"

#include <cfloat>
#include <cmath>

using namespace faustlens;
using namespace faustlens::controls;

namespace {

UiNode Slider(double min, double max, double step = 0) {
    UiNode n;
    n.Kind = UiKind::HSlider;
    n.Min = min;
    n.Max = max;
    n.Step = step;
    return n;
}

} // namespace

TEST_CASE("the four metadata keys the control surface acts on are read, and the rest carried") {
    UiNode n = Slider(0, 1);
    n.Meta["style"] = {"knob"};
    n.Meta["scale"] = {"log"};
    n.Meta["unit"] = {"Hz"};
    n.Meta["midi"] = {"ctrl 7"};
    const Style s = StyleOf(n);
    CHECK(s.Knob);
    CHECK(s.Scale == Scale::Log);
    CHECK(s.Unit == "Hz");
    CHECK_FALSE(s.Hidden);
    CHECK(n.Meta.contains("midi"));
}

TEST_CASE("`[hidden]` hides, and only an explicit zero does not") {
    UiNode n = Slider(0, 1);
    CHECK_FALSE(StyleOf(n).Hidden);
    n.Meta["hidden"] = {"1"};
    CHECK(StyleOf(n).Hidden);
    // `[hidden]` with no value extracts as the empty string, and still hides.
    n.Meta["hidden"] = {""};
    CHECK(StyleOf(n).Hidden);
    n.Meta["hidden"] = {"0"};
    CHECK_FALSE(StyleOf(n).Hidden);
}

TEST_CASE("the position-to-value curve is the reference's") {
    SUBCASE("linear spans the declared range") {
        const UiNode n = Slider(-6, 6);
        CHECK(ToValue(n, Scale::Linear, 0.0) == doctest::Approx(-6));
        CHECK(ToValue(n, Scale::Linear, 0.5) == doctest::Approx(0));
        CHECK(ToValue(n, Scale::Linear, 1.0) == doctest::Approx(6));
        CHECK(ToPosition(n, Scale::Linear, 0.0) == doctest::Approx(0.5));
    }
    SUBCASE("log is linear in the logarithm") {
        const UiNode n = Slider(20, 20000);
        CHECK(ToValue(n, Scale::Log, 0.0) == doctest::Approx(20));
        CHECK(ToValue(n, Scale::Log, 0.5) == doctest::Approx(std::sqrt(20.0 * 20000.0)));
        CHECK(ToValue(n, Scale::Log, 1.0) == doctest::Approx(20000));
    }
    SUBCASE("a log range starting at zero is floored, not undefined") {
        // The reference floors both endpoints at `DBL_EPSILON` rather than rejecting.
        const UiNode n = Slider(0, 20000);
        CHECK(std::isfinite(ToValue(n, Scale::Log, 0.0)));
        CHECK(ToValue(n, Scale::Log, 0.0) == doctest::Approx(DBL_EPSILON));
        CHECK(ToValue(n, Scale::Log, 0.5) == doctest::Approx(std::sqrt(DBL_EPSILON * 20000.0)));
    }
    SUBCASE("exp is linear in the exponential") {
        const UiNode n = Slider(0, 4);
        CHECK(ToValue(n, Scale::Exp, 0.0) == doctest::Approx(0));
        CHECK(ToValue(n, Scale::Exp, 1.0) == doctest::Approx(4));
        CHECK(ToValue(n, Scale::Exp, 0.5) == doctest::Approx(std::log((std::exp(0.0) + std::exp(4.0)) / 2)));
    }
    SUBCASE("every curve round-trips, and the position stays a position") {
        for (const Scale s : {Scale::Linear, Scale::Log, Scale::Exp}) {
            const UiNode n = Slider(1, 100);
            for (const double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                CHECK(ToPosition(n, s, ToValue(n, s, t)) == doctest::Approx(t));
                // A logarithm round trip misses by an ulp. `Quantize` is exact.
                CHECK(ToValue(n, s, t) >= doctest::Approx(1.0));
                CHECK(ToValue(n, s, t) <= doctest::Approx(100.0));
                CHECK(Quantize(n, ToValue(n, s, t)) >= 1.0);
                CHECK(Quantize(n, ToValue(n, s, t)) <= 100.0);
            }
            CHECK(ToPosition(n, s, -1000) == doctest::Approx(0));
            CHECK(ToPosition(n, s, 1000) == doctest::Approx(1));
        }
    }
}

TEST_CASE("`min`, `max` and `step` are a contract the host enforces, not one the DSP re-checks") {
    const UiNode n = Slider(0, 10, 0.25);
    CHECK(Quantize(n, 3.3) == doctest::Approx(3.25));
    CHECK(Quantize(n, 3.4) == doctest::Approx(3.5));
    // The DSP does not re-check the range, so this is the only place it holds.
    CHECK(Quantize(n, -5) == doctest::Approx(0));
    CHECK(Quantize(n, 500) == doctest::Approx(10));
    CHECK(Quantize(Slider(0, 1), 0.123456) == doctest::Approx(0.123456));
}

TEST_CASE("the displayed value takes its precision from the step") {
    Style s;
    CHECK(Format(Slider(0, 1000, 1), s, 440) == "440");
    CHECK(Format(Slider(0, 1, 0.01), s, 0.5) == "0.50");
    CHECK(Format(Slider(0, 1, 0.001), s, 0.5) == "0.500");
    // No step declared: three places.
    CHECK(Format(Slider(0, 1), s, 0.5) == "0.500");
    s.Unit = "Hz";
    CHECK(Format(Slider(0, 1000, 1), s, 440) == "440 Hz");
}
