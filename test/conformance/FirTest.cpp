#include "conformance/FirParse.h"
#include "conformance/Sweep.h"

#include "doctest.h"

#include <filesystem>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace faustlens::test;

namespace {

namespace fs = std::filesystem;

void WalkTerm(const FirTerm &t, const std::function<void(const FirTerm &)> &f) {
    f(t);
    for (const FirTerm &a : t.Args) WalkTerm(a, f);
    for (const FirTerm &i : t.Index) WalkTerm(i, f);
}

void WalkStmts(std::span<const FirStmt> v, const std::function<void(const FirStmt &)> &f) {
    for (const FirStmt &s : v) {
        f(s);
        WalkStmts(s.Body, f);
    }
}

// Normalize numbered subcontainer labels for vocabulary checks.
std::string SectionKey(const std::string &n) {
    const size_t q = n.find('"');
    return q == std::string::npos ? n : n.substr(0, q) + "…";
}

} // namespace

TEST_CASE("the `.fir` reader is total over the reference corpus") {
    REQUIRE_MESSAGE(fs::is_directory(OracleDir()), "run test/conformance/regenerate_oracle.sh first");

    int files = 0;
    std::vector<std::string> failures;
    std::set<std::string> instructions, sections, callables;

    for (const fs::path &p : PathsIn(OracleDir(), ".fir")) {
        const auto f = ParseFir(ReadText(p));
        if (!f) {
            failures.push_back(p.stem().string() + ": " + f.error());
            continue;
        }
        ++files;
        if (f->Container.empty()) failures.push_back(p.stem().string() + ": no container name");

        for (const FirSection &s : f->Sections) {
            sections.insert(SectionKey(s.Name));
            WalkStmts(s.Stmts, [&](const FirStmt &st) {
                instructions.insert(st.Term.Name);
                // Include nested expression kinds in the vocabulary.
                WalkTerm(st.Term, [&](const FirTerm &t) {
                    if (t.Kind == FirTerm::Kind::Call && !t.Name.empty()) callables.insert(t.Name);
                });
            });
        }
    }

    for (const std::string &f : failures) MESSAGE(f);
    CHECK(failures.empty());
    CHECK(files == 94);

    const std::set<std::string> known_callables = {
        "AddButtonInst",
        "AddCheckButtonInst",
        "AddHorizontalBargraph",
        "AddHorizontalSlider",
        "AddMetaDeclareInst",
        "AddNumEntry",
        "AddSoundfile",
        "AddVerticalBargraph",
        "AddVerticalSlider",
        "DeclareFunInst",
        "DeclareStructTypeInst",
        "DeclareVarInst",
        "DropInst",
        "OpenHorizontalBox",
        "OpenTabBox",
        "OpenVerticalBox",
        "RetInst",
        "StoreVarInst",
        // Exclude the Real(*) statistics label from callable names.
        "&",
        "->",
        "Address",
        "BinopInst",
        "CastInst",
        "Double",
        "DoubleArrayNumInst",
        "FunCallInst",
        "Int32",
        "Int32ArrayNumInst",
        "Int64",
        "LoadVarInst",
        "MethodFunCallInst",
        "NegInst",
        "Select2Inst",
        "StructType",
    };
    CheckVocabulary("callable", callables, known_callables);

    // Include bare block keywords and CloseboxInst.
    const std::set<std::string> known_statements = {
        "BinopInst",
        "LoadVarInst",
        "AddButtonInst",
        "AddCheckButtonInst",
        "AddHorizontalBargraph",
        "AddHorizontalSlider",
        "AddMetaDeclareInst",
        "AddNumEntry",
        "AddSoundfile",
        "AddVerticalBargraph",
        "AddVerticalSlider",
        "BlockInst",
        "CloseboxInst",
        "DeclareFunInst",
        "DeclareStructTypeInst",
        "DeclareVarInst",
        "DropInst",
        "ForLoopInst",
        "IfInst",
        "OpenHorizontalBox",
        "OpenTabBox",
        "OpenVerticalBox",
        "RetInst",
        "StoreVarInst",
    };
    CheckVocabulary("statement", instructions, known_statements);

    // Compare Init separately from lifecycle sections and repeated Flatten FIR output.
    const std::set<std::string> known_sections = {
        "Allocate",
        "Clear",
        "COMPILER STATISTICS",
        "Compute control",
        "Compute DSP",
        "Container …",
        "DSP struct",
        "External types declaration",
        "Flatten FIR",
        "Global declarations",
        "Global external declarations",
        "Init",
        "Object memory footprint",
        "Post compute DSP",
        "ResetUI",
        "Static Init",
        "Sub container",
        "Sub container …",
        "User Interface",
        "Variable access in Control",
        "Variable access in compute control",
        "Variable access in compute DSP",
    };
    CheckVocabulary("section", sections, known_sections);
}

TEST_CASE("the `.fir` DSP struct reads as fields with shapes") {
    int files = 0;
    std::vector<std::string> failures;
    std::set<std::string> types;

    for (const fs::path &p : PathsIn(OracleDir(), ".fir")) {
        const std::string name = p.stem().string();
        const auto f = ParseFir(ReadText(p));
        if (!f) continue;
        ++files;

        bool found = false;
        for (const FirSection &s : f->Sections) {
            if (s.Name != "DSP struct") continue;
            for (const FirStmt &st : s.Stmts) {
                if (st.Term.Name != "DeclareStructTypeInst" || st.Term.Args.empty()) continue;
                const FirTerm &ty = st.Term.Args[0];
                if (ty.Name != "StructType" || ty.Args.empty()) continue;
                found = true;
                for (size_t i = 1; i < ty.Args.size(); ++i) {
                    const FirTerm &pair = ty.Args[i];
                    if (pair.Kind != FirTerm::Kind::Call || pair.Args.size() != 2) {
                        failures.push_back(name + ": a struct field is not a (type, name) pair");
                        continue;
                    }
                    // Read array extent from the field type.
                    const std::string &decl = pair.Args[0].Name;
                    const size_t br = decl.find('[');
                    types.insert(decl.substr(0, br));
                    const FirTerm &fld = pair.Args[1];
                    if (fld.Name.empty()) failures.push_back(name + ": a struct field has no name");
                }
            }
        }
        if (!found) failures.push_back(name + ": no DSP struct");
    }

    for (const std::string &f : failures) MESSAGE(f);
    CHECK(failures.empty());
    CHECK(files == 94);
    CHECK(types == std::set<std::string>{"FAUSTFLOAT", "Soundfile*", "double", "int"});
}
