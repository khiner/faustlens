// Compare flattened UI trees with the reference FIR User Interface section.
#include "signal/Ui.h"
#include "conformance/FirParse.h"
#include "conformance/Sweep.h"
#include "query/Query.h"

#include "doctest.h"

#include <algorithm>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <span>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

namespace fs = std::filesystem;

// Compare numeric bounds independently of reference text formatting.
std::string Num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

const char *KindName(UiKind k) {
    switch (k) {
        case UiKind::Button: return "button";
        case UiKind::Checkbox: return "check";
        case UiKind::VSlider: return "vslider";
        case UiKind::HSlider: return "hslider";
        case UiKind::NumEntry: return "nentry";
        case UiKind::VBargraph: return "vbargraph";
        case UiKind::HBargraph: return "hbargraph";
        case UiKind::Soundfile: return "soundfile";
    }
    return "?";
}

void FlattenOurs(const UiNode &n, std::vector<std::string> &out) {
    // Read soundfile URLs from the instruction field.
    if (n.Kind == UiKind::Soundfile && !n.IsGroup) {
        const auto url = n.Meta.find("url");
        out.push_back(std::format("soundfile {} {}", n.Label, url == n.Meta.end() || url->second.empty() ? "" : *url->second.begin()));
        return;
    }
    for (const auto &[key, values] : n.Meta)
        for (const std::string &v : values) out.push_back(std::format("meta {}={}", key, v));
    if (n.IsGroup) {
        out.push_back(std::format("open {} {}", "vht"[n.Orient], n.Label));
        for (const UiNode &c : n.Children) FlattenOurs(c, out);
        out.emplace_back("close");
        return;
    }
    std::string s = std::format("{} {}", KindName(n.Kind), n.Label);
    switch (n.Kind) {
        case UiKind::Button:
        case UiKind::Checkbox:
        case UiKind::Soundfile: break;
        case UiKind::VBargraph:
        case UiKind::HBargraph: s += std::format(" {} {}", Num(n.Min), Num(n.Max)); break;
        default: s += std::format(" {} {} {} {}", Num(n.Init), Num(n.Min), Num(n.Max), Num(n.Step)); break;
    }
    out.push_back(s);
}

std::expected<std::vector<std::string>, std::string> FlattenTheirs(const FirFile &f) {
    std::vector<std::string> out;
    const auto num = [&](const FirTerm &t) { return Num(t.Num); };
    for (const FirSection &s : f.Sections) {
        if (s.Name != "User Interface") continue;
        for (const FirStmt &top : s.Stmts) {
            const std::vector<FirStmt> &body = top.Term.Name == "BlockInst" ? top.Body : s.Stmts;
            for (const FirStmt &st : body) {
                const FirTerm &t = st.Term;
                const std::vector<FirTerm> &a = t.Args;
                const auto arg = [&](size_t i) { return i < a.size() ? a[i].Name : std::string(); };
                if (t.Name == "OpenVerticalBox") out.push_back(std::format("open v {}", arg(0)));
                else if (t.Name == "OpenHorizontalBox") out.push_back(std::format("open h {}", arg(0)));
                else if (t.Name == "OpenTabBox") out.push_back(std::format("open t {}", arg(0)));
                else if (t.Name == "CloseboxInst") out.emplace_back("close");
                else if (t.Name == "AddMetaDeclareInst") out.push_back(std::format("meta {}={}", arg(1), arg(2)));
                else if (t.Name == "AddButtonInst") out.push_back(std::format("button {}", arg(0)));
                else if (t.Name == "AddCheckButtonInst") out.push_back(std::format("check {}", arg(0)));
                else if (t.Name == "AddVerticalBargraph") out.push_back(std::format("vbargraph {} {} {}", arg(0), num(a[2]), num(a[3])));
                else if (t.Name == "AddHorizontalBargraph") out.push_back(std::format("hbargraph {} {} {}", arg(0), num(a[2]), num(a[3])));
                else if (t.Name == "AddVerticalSlider" || t.Name == "AddHorizontalSlider" || t.Name == "AddNumEntry") {
                    const char *k = t.Name == "AddNumEntry" ? "nentry" : t.Name == "AddVerticalSlider" ? "vslider" : "hslider";
                    out.push_back(std::format("{} {} {} {} {} {}", k, arg(0), num(a[2]), num(a[3]), num(a[4]), num(a[5])));
                } else if (t.Name == "AddSoundfile") {
                    out.push_back(std::format("soundfile {} {}", arg(0), arg(1)));
                } else {
                    return std::unexpected(std::format("unread UI instruction `{}`", t.Name));
                }
            }
            if (top.Term.Name == "BlockInst") break;
        }
        return out;
    }
    return std::unexpected("no `User Interface` section");
}

} // namespace

TEST_CASE("UI trees match the reference corpus") {
    ForEachDump<FirFile>(".fir", ParseFir, [](const FirFile &reference, Program &program) {
        const auto theirs = FlattenTheirs(reference);
        REQUIRE_MESSAGE(theirs, (theirs ? "" : theirs.error()));
        std::vector<std::string> ours;
        FlattenOurs(program.Ui(RootLabel(program.Session.Metadata)), ours);
        CHECK(ours == *theirs);
    });
}

TEST_CASE("control paths are unique across the reference corpus") {
    const auto paths = DspPaths();
    REQUIRE(paths.size() == 94);
    for (const auto &path : paths) {
        INFO(path.string());
        const Program program(path);
        REQUIRE(program.Ok);
        for (const auto &diagnostic : CheckPaths(program.Ui(RootLabel(program.Session.Metadata)))) FAIL_CHECK(diagnostic.Payload);
    }
}

TEST_CASE("the three duplicate-path rules, stated apart") {
    // Test programs rejected by the reference outside the accepted corpus.
    const auto check = [](const std::string &src) {
        Program const prog("/u.dsp", src);
        REQUIRE(prog.Ok);
        return CheckPaths(prog.Ui("p"));
    };

    SUBCASE("two inputs on one path is an error") {
        const std::vector<Diagnostic> d = check("process = hslider(\"g\", 0, 0, 1, 0.1) + hslider(\"g\", 1, 0, 2, 0.1);\n");
        REQUIRE(d.size() == 1);
        CHECK(d[0].Severity == Severity::Error);
        CHECK(d[0].Code == Code::PropDuplicatePath);
        CHECK(d[0].Payload.find("'/p/g'") != std::string::npos);
    }
    SUBCASE("an input and a bargraph on one path is an error") {
        // Detect paths that collide after label cleanup.
        const std::vector<Diagnostic> d = check("process = _ * hslider(\"L [unit:dB]\", 0, 0, 1, 0.1) : vbargraph(\"L\", 0, 1);\n");
        REQUIRE(d.size() == 1);
        CHECK(d[0].Severity == Severity::Error);
        CHECK(d[0].Payload.find("an input control and a bargraph") != std::string::npos);
    }
    SUBCASE("two bargraphs on one path is only a warning") {
        const std::vector<Diagnostic> d = check("process = _, _ : vbargraph(\"m\", 0, 1), vbargraph(\"m\", 0, 1);\n");
        REQUIRE(d.size() == 1);
        CHECK(d[0].Severity == Severity::Warning);
        CHECK(d[0].Code == Code::PropDuplicateBargraphPath);
    }
    SUBCASE("groups keep two same-named controls apart") {
        CHECK(check(
                  "process = vgroup(\"a\", hslider(\"g\", 0, 0, 1, 0.1)) + "
                  "vgroup(\"b\", hslider(\"g\", 0, 0, 1, 0.1));\n"
        )
                  .empty());
    }
    SUBCASE("a widget that folds away collides with nothing") {
        CHECK(check(
                  "unused = hslider(\"g\", 0, 0, 1, 0.1);\n"
                  "process = _ * hslider(\"g\", 1, 0, 2, 0.1);\n"
        )
                  .empty());
    }
}
