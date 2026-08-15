// The vocabulary is pinned so an unknown operation cannot pass as a call node.
#include "conformance/SigParse.h"
#include "conformance/Sweep.h"

#include "doctest.h"

#include <filesystem>
#include <format>
#include <set>
#include <string>
#include <vector>

using namespace faustlens::test;

namespace {

namespace fs = std::filesystem;

// Every operation this corpus reaches: the reference printer's own, plus the `ffunction`s
// the corpus declares.
const std::set<std::string> &KnownOps() {
    static const std::set<std::string> k = {
        "+",         "-",      "*",         "/",         "%",          "<<",     ">>",        ">>>",     "<",      "<=",     ">",       ">=",       "==",
        "!=",        "&",      "^",         "|",         "@",          "'",      "letrec",    "select2", "prefix", "attach", "control", "enable",   "int",
        "float",     "bit",    "sigRDTbl",  "WRTbl2p",   "sigWRTbl4p", "sigGen", "soundfile", "buffer",  "length", "rate",   "button",  "checkbox", "vslider",
        "hslider",   "nentry", "vbargraph", "hbargraph", "abs",        "acos",   "acosh",     "asin",    "asinh",  "atan",   "atan2",   "atanh",    "ceil",
        "copysign",  "cos",    "cosh",      "exp",       "floor",      "fmod",   "isinf",     "isnan",   "log",    "log10",  "max",     "min",      "pow",
        "remainder", "rint",   "round",     "sin",       "sinh",       "sqrt",   "tan",       "tanh",
    };
    return k;
}

bool IsProj(const std::string &s) { return s.starts_with("proj") && s.size() > 4 && s.find_first_not_of("0123456789", 4) == std::string::npos; }

void Walk(const SigTerm &t, size_t defs, std::set<std::string> &ops, std::set<std::string> &names, std::string &bad) {
    switch (t.Kind) {
        case SigTerm::Kind::Id:
            if (!bad.empty()) return;
            if (t.I < 0 || size_t(t.I) >= defs) bad = std::format("ID_{} is out of range", t.I);
            return;
        case SigTerm::Kind::Name: names.insert(t.Text); return;
        case SigTerm::Kind::Op:
            if (!IsProj(t.Text)) ops.insert(t.Text);
            break;
        default: break;
    }
    for (const SigTerm &a : t.Args) Walk(a, defs, ops, names, bad);
}

} // namespace

TEST_CASE("the `.sig` reader is total over the reference corpus") {
    REQUIRE_MESSAGE(fs::is_directory(OracleDir()), "run test/conformance/regenerate_oracle.sh first");

    std::vector<std::string> failures;
    std::set<std::string> ops, names;
    int files = 0, nodes = 0;

    for (const fs::path &p : PathsIn(OracleDir(), ".sig")) {
        const std::string name = p.filename().string();
        const auto f = ParseSig(ReadText(p));
        if (!f) {
            failures.push_back(name + ": " + f.error());
            continue;
        }
        // The reference's own node count, an independent check nothing was skipped.
        if (f->Size >= 0 && size_t(f->Size) != f->Defs.size())
            failures.push_back(std::format("{}: header says {} nodes, read {}", name, f->Size, f->Defs.size()));
        if (f->Outputs.Args.empty()) failures.push_back(name + ": no outputs");

        std::string bad;
        for (const SigTerm &d : f->Defs) Walk(d, f->Defs.size(), ops, names, bad);
        Walk(f->Outputs, f->Defs.size(), ops, names, bad);
        if (!bad.empty()) failures.push_back(name + ": " + bad);

        nodes += int(f->Defs.size());
        ++files;
    }

    for (const std::string &s : failures) MESSAGE(s);
    CHECK(failures.empty());
    CHECK(files == 188); // 94 programs, normalized and unnormalized

    std::vector<std::string> unknown;
    for (const std::string &o : ops)
        if (!KnownOps().contains(o)) unknown.push_back(o);
    for (const std::string &u : unknown) MESSAGE("unpinned operation: ", u);
    CHECK(unknown.empty());

    MESSAGE("`.sig` read over ", files, " files, ", nodes, " nodes, ", ops.size(), " distinct operations");
    for (const std::string &n : names) MESSAGE("foreign name: ", n);
}
