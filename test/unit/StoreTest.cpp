#include "controls/Store.h"
#include "Live.h"
#include "query/Query.h"
#include "runtime/Interp.h"
#include "signal/Ui.h"
#include "unit/Ui.h"

#include "doctest.h"

#include <memory>
#include <string>
#include <string_view>

using namespace faustlens;
using namespace faustlens::test;
using faustlens::app::Artifact;

namespace {

void Move(app::Live &live, controls::Values &store, std::string_view label, double v) {
    const Artifact *a = live.Current.get();
    REQUIRE(a != nullptr);
    const UiNode *w = Widget(a->Ui, label);
    REQUIRE(w != nullptr);
    a->Dsp->SetControl(w->WidgetLabel, v);
    controls::Record(store, a->Plan.Label(w->WidgetLabel), *w, v);
}

// What a control reads now, or a sentinel where the running program has no such widget.
double Read(const app::Live &live, std::string_view label) {
    const Artifact *a = live.Current.get();
    REQUIRE(a != nullptr);
    const UiNode *w = Widget(a->Ui, label);
    if (w == nullptr) return -12345;
    return a->Dsp->Control(w->WidgetLabel);
}

struct Fixture {
    Session Session;
    app::Live Live;
    controls::Values Controls;

    explicit Fixture(const std::string &src) { Edit(src); }

    void Edit(const std::string &src) {
        Session.SetBuffer("/s.dsp", src);
        Live.Reload(Session, "/s.dsp", Controls);
    }
};

} // namespace

TEST_CASE("a slider stays moved when the code around it is edited") {
    Fixture f(
        "gain = hslider(\"gain\", 0.1, 0, 1, 0.01);\n"
        "process = _ * gain;\n"
    );
    Move(f.Live, f.Controls, "gain", 0.75);

    f.Edit(
        "gain = hslider(\"gain\", 0.1, 0, 1, 0.01);\n"
        "process = _ * gain * 2.0;\n"
    );
    CHECK(Read(f.Live, "gain") == doctest::Approx(0.75));
}

TEST_CASE("and stays moved across a revision that did not declare it") {
    // The reported bug: move a slider, delete the stage it is in, undo.
    Fixture f(
        "gain = hslider(\"gain\", 0.1, 0, 1, 0.01);\n"
        "process = _ * gain;\n"
    );
    Move(f.Live, f.Controls, "gain", 0.75);

    f.Edit("process = _;\n"); // the definition is gone, and so is the widget
    REQUIRE(Read(f.Live, "gain") == -12345);

    f.Edit(
        "gain = hslider(\"gain\", 0.1, 0, 1, 0.01);\n"
        "process = _ * gain;\n"
    );
    CHECK(Read(f.Live, "gain") == doctest::Approx(0.75));
}

TEST_CASE("a control keys on its path, not on the id the arena gave it") {
    // Equal ids in two programs name different paths, so keying on id silenced the oscillator.
    Fixture f(
        "freq = hslider(\"freq\", 220, 20, 8000, 1);\n"
        "process = os_dummy(freq) : abs : max ~ *(0.999) : vbargraph(\"m\", 0, 1)\n"
        "with { os_dummy(f) = f; };\n"
    );
    Move(f.Live, f.Controls, "freq", 440);

    // One widget, taking the id the slider had in the program above.
    f.Edit("process = _ : abs : max ~ *(0.999) : vbargraph(\"m\", 0, 1);\n");
    CHECK(Read(f.Live, "m") == doctest::Approx(0));

    f.Edit(
        "freq = hslider(\"freq\", 220, 20, 8000, 1);\n"
        "process = os_dummy(freq) : abs : max ~ *(0.999) : vbargraph(\"m\", 0, 1)\n"
        "with { os_dummy(f) = f; };\n"
    );
    CHECK(Read(f.Live, "freq") == doctest::Approx(440));
}

TEST_CASE("an untouched control follows its declared init") {
    Fixture f("process = _ * hslider(\"gain\", 0.1, 0, 1, 0.01);\n");
    REQUIRE(Read(f.Live, "gain") == doctest::Approx(0.1));
    f.Edit("process = _ * hslider(\"gain\", 0.4, 0, 1, 0.01);\n");
    CHECK(Read(f.Live, "gain") == doctest::Approx(0.4));
}

TEST_CASE("a control with no entry goes back to what its program declares") {
    Fixture f("process = _ * hslider(\"gain\", 0.1, 0, 1, 0.01);\n");
    Move(f.Live, f.Controls, "gain", 0.75);
    REQUIRE(Read(f.Live, "gain") == doctest::Approx(0.75));

    f.Controls.clear(); // what an undo past that gesture leaves
    const Artifact *a = f.Live.Current.get();
    REQUIRE(a != nullptr);
    controls::Apply(f.Controls, a->Plan, a->Ui, *a->Dsp);
    CHECK(Read(f.Live, "gain") == doctest::Approx(0.1));
}

TEST_CASE("a restored value is re-quantized against the program it lands in") {
    // The host enforces `min`/`max`/`step`, so a value restored into a narrower range clamps.
    Fixture f("process = _ * hslider(\"gain\", 0, 0, 1, 0.01);\n");
    Move(f.Live, f.Controls, "gain", 0.9);
    f.Edit("process = _ * hslider(\"gain\", 0, 0, 0.5, 0.01);\n");
    CHECK(Read(f.Live, "gain") == doctest::Approx(0.5));
}

TEST_CASE("a stored value applies only where the widget is the same kind") {
    SUBCASE("a slider that became a checkbox does not take the slider's value") {
        Fixture f("process = _ * hslider(\"g\", 0, 0, 1, 0.01);\n");
        Move(f.Live, f.Controls, "g", 0.75);
        f.Edit("process = _ * checkbox(\"g\");\n");
        CHECK(Read(f.Live, "g") == doctest::Approx(0));
    }
    SUBCASE("but the three continuous kinds are one kind") {
        Fixture f("process = _ * hslider(\"g\", 0, 0, 1, 0.01);\n");
        Move(f.Live, f.Controls, "g", 0.75);
        f.Edit("process = _ * vslider(\"g\", 0, 0, 1, 0.01);\n");
        CHECK(Read(f.Live, "g") == doctest::Approx(0.75));
    }
    SUBCASE("a checkbox keeps its state") {
        Fixture f("process = _ * checkbox(\"on\");\n");
        Move(f.Live, f.Controls, "on", 1);
        f.Edit("process = _ * checkbox(\"on\") * 2;\n");
        CHECK(Read(f.Live, "on") == doctest::Approx(1));
    }
}

TEST_CASE("outputs and momentary controls are never stored") {
    // A bargraph is written by the DSP, and a stored `button` at 1 is stuck down.
    CHECK(controls::RestorableAs(UiKind::VBargraph) == controls::Restorable::No);
    CHECK(controls::RestorableAs(UiKind::HBargraph) == controls::Restorable::No);
    CHECK(controls::RestorableAs(UiKind::Button) == controls::Restorable::No);

    Fixture f("process = _ <: attach(_, vbargraph(\"m\", 0, 1));\n");
    const Artifact *a = f.Live.Current.get();
    REQUIRE(a != nullptr);
    const UiNode *w = Widget(a->Ui, "m");
    REQUIRE(w != nullptr);
    controls::Record(f.Controls, a->Plan.Label(w->WidgetLabel), *w, 0.5);
    CHECK(f.Controls.empty());
}
