// The extracted UI tree against the `.fir`'s `User Interface` section, both sides flattened.
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

// Bounds compare as numbers, not text: the reference prints `60` as `6e+01`.
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
    // A soundfile's `url` is the instruction's own field, not metadata.
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
            // The section is one `BlockInst` wrapping the whole listing.
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

std::string FirstDifference(std::span<const std::string> ours, std::span<const std::string> theirs) {
    for (size_t i = 0; i < std::max(ours.size(), theirs.size()); ++i) {
        const std::string a = i < ours.size() ? ours[i] : "<end>";
        const std::string b = i < theirs.size() ? theirs[i] : "<end>";
        if (a != b) return "ours `" + a + "`, theirs `" + b + "`";
    }
    return "";
}

} // namespace

TEST_CASE("UI completeness: the extracted tree matches the `.fir` User Interface section") {
    int agreed = 0, differed = 0, widgets = 0;
    Census census;

    ForEachDump<FirFile>(".fir", ParseFir, census, differed, [&](const std::string &name, const FirFile &fir, Program &prog) {
        const auto theirs = FlattenTheirs(fir);
        if (!theirs) {
            ++differed;
            census.Add(theirs.error());
            return;
        }
        widgets += int(prog.Prop.Ui.size());

        // Both trees are built downstream of simplification, so a folded-away widget is in neither.
        std::vector<std::string> ours;
        FlattenOurs(prog.Ui(RootLabel(prog.Session.Metadata)), ours);

        const std::string diff = FirstDifference(ours, *theirs);
        if (diff.empty()) {
            ++agreed;
            return;
        }
        ++differed;
        const size_t sp = diff.find(' ');
        const size_t sp2 = diff.find("`, theirs `");
        const std::string key = diff.substr(0, sp2 == std::string::npos ? sp : sp2);
        census.Add(key.substr(0, key.find(' ', 6)), name + " -- " + diff);
    });

    MESSAGE("UI tree: ", agreed, " of 94 agree, ", differed, " differ; ", widgets, " widgets propagated");
    census.Report();

    CHECK(agreed + differed == 94);
    // Reference rules: the root takes `declare name` over the filename, an unlabelled
    // bargraph is counter-named and anything else `0x00`.
    CHECK(agreed == 94);
}

TEST_CASE("a label path names exactly one control") {
    // Reference faust throws on a duplicate path and every corpus program compiles there.
    int programs = 0, warned = 0;
    std::vector<std::string> errors;
    std::map<std::string, int> warnings;

    for (const fs::path &p : DspPaths()) {
        const std::string name = p.stem().string();
        Program const prog(p);
        if (!prog.Ok) continue;
        ++programs;

        bool any = false;
        for (const Diagnostic &d : CheckPaths(prog.Ui(RootLabel(prog.Session.Metadata)))) {
            if (d.Severity == Severity::Error) errors.push_back(name + ": " + d.Payload);
            else {
                ++warnings[name];
                any = true;
            }
        }
        warned += any;
    }

    MESSAGE("checked ", programs, " programs: ", errors.size(), " with a duplicate control path, ", warned, " with a repeated bargraph path");
    for (const std::string &e : errors) MESSAGE("  ", e);
    CHECK(errors.empty());

    // No corpus program reaches the bargraph case: `vumeter`'s empty label counter-names as theirs.
    CHECK(warnings.empty());
}

TEST_CASE("the three duplicate-path rules, stated apart") {
    // Reference faust refuses to compile these, so none is in the corpus.
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
        // Regresses a demo bug: the two labels clean to one path.
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
