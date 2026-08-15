// The `.fir` reader: every file parses, blocks close, and the vocabularies are pinned.
#include "conformance/FirParse.h"
#include "conformance/Sweep.h"

#include "doctest.h"

#include <filesystem>
#include <functional>
#include <map>
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

// Drops the subcontainer label so the twenty-five `Sub container "mydspSIG<n>"` markers pin as one.
std::string SectionKey(const std::string &n) {
    const size_t q = n.find('"');
    return q == std::string::npos ? n : n.substr(0, q) + "…";
}

std::string NoteShape(const std::string &line) {
    if (!line.empty() && line[0] == '=') return "=…=";
    const size_t k = line.find_first_of("=:");
    std::string head = k == std::string::npos ? line : line.substr(0, k);
    while (!head.empty() && head.back() == ' ') head.pop_back();
    return head;
}

} // namespace

TEST_CASE("the `.fir` reader is total over the reference corpus") {
    REQUIRE_MESSAGE(fs::is_directory(OracleDir()), "run test/conformance/regenerate_oracle.sh first");

    int files = 0;
    size_t stmts = 0, notes = 0;
    std::vector<std::string> failures;
    std::set<std::string> instructions, sections, callables, note_shapes;

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
            notes += s.Notes.size();
            // `COMPILER STATISTICS` is instrumentation, not FIR, so its lines go unpinned.
            if (s.Name != "COMPILER STATISTICS")
                for (const std::string &t : s.Notes) note_shapes.insert(NoteShape(t));
            WalkStmts(s.Stmts, [&](const FirStmt &st) {
                ++stmts;
                instructions.insert(st.Term.Name);
                // Wider than the statement heads: `Address`, `Int32` and the rest only nest.
                WalkTerm(st.Term, [&](const FirTerm &t) {
                    if (t.Kind == FirTerm::Kind::Call && !t.Name.empty()) callables.insert(t.Name);
                });
            });
        }
    }

    for (const std::string &f : failures) MESSAGE(f);
    MESSAGE("`.fir` read over ", files, " files, ", stmts, " statements, ", notes, " lines outside the statement grammar");
    MESSAGE("  callables: ", Join(callables));
    MESSAGE("  sections: ", Join(sections, " | "));

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
        // `Real(*)` is a binop class name in the `Instructions complexity` note, not a callable.
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
    PinVocabulary("callable", callables, known_callables);

    // Not a subset of the callables: the three bracketing keywords and `CloseboxInst` print bare.
    const std::set<std::string> known_statements = {
        // A `ForLoopInst`'s or `IfInst`'s condition is a statement in this tree.
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
    PinVocabulary("statement", instructions, known_statements);

    // Only `Init` is the init band: `ResetUI`/`Clear` are lifecycle, `Flatten FIR` repeats
    // the whole program.
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
    PinVocabulary("section", sections, known_sections);

    // Secondary oracles on state layout, pinned even though nothing reads them.
    const std::set<std::string> known_notes = {
        "=…=", // `========== Declaration part ==========`, inside `Flatten FIR`
        "Field", "Heap size int", "Heap size int*", "Heap size real", "Instructions complexity", "Stack size in compute", "Total heap size",
    };
    PinVocabulary("note", note_shapes, known_notes);
}

TEST_CASE("the `.fir` DSP struct reads as fields with shapes") {
    int files = 0, with_tables = 0;
    size_t fields = 0, arrays = 0;
    std::vector<std::string> failures;
    std::set<std::string> types;
    std::map<std::string, int> prefixes;

    for (const fs::path &p : PathsIn(OracleDir(), ".fir")) {
        const std::string name = p.stem().string();
        const auto f = ParseFir(ReadText(p));
        if (!f) continue;
        ++files;

        bool found = false, static_struct = false;
        for (const FirSection &s : f->Sections) {
            // A `StaticStruct` declaration is a read-only table kept outside the DSP struct.
            if (s.Name == "Global declarations")
                WalkStmts(s.Stmts, [&](const FirStmt &st) {
                    for (const FirTerm &a : st.Term.Args)
                        if (a.Kind == FirTerm::Kind::Name && a.Name.starts_with("StaticStruct")) static_struct = true;
                });
            if (s.Name != "DSP struct") continue;
            for (const FirStmt &st : s.Stmts) {
                if (st.Term.Name != "DeclareStructTypeInst" || st.Term.Args.empty()) continue;
                const FirTerm &ty = st.Term.Args[0];
                if (ty.Name != "StructType" || ty.Args.empty()) continue;
                found = true;
                // First argument is the struct's name, the rest `("type", field)` pairs.
                for (size_t i = 1; i < ty.Args.size(); ++i) {
                    const FirTerm &pair = ty.Args[i];
                    if (pair.Kind != FirTerm::Kind::Call || pair.Args.size() != 2) {
                        failures.push_back(name + ": a struct field is not a (type, name) pair");
                        continue;
                    }
                    ++fields;
                    // The extent rides on the type: `("double[1024]", ftbl0)`.
                    const std::string &decl = pair.Args[0].Name;
                    const size_t br = decl.find('[');
                    types.insert(decl.substr(0, br));
                    if (br != std::string::npos) ++arrays;
                    const FirTerm &fld = pair.Args[1];
                    size_t k = fld.Name.size();
                    while (k > 0 && std::isdigit(static_cast<unsigned char>(fld.Name[k - 1]))) --k;
                    ++prefixes[fld.Name.substr(0, k)];
                    if (fld.Name.empty()) failures.push_back(name + ": a struct field has no name");
                }
            }
        }
        if (!found) failures.push_back(name + ": no DSP struct");
        if (static_struct) ++with_tables;
    }

    for (const std::string &f : failures) MESSAGE(f);
    MESSAGE(
        "`.fir` DSP struct: ", fields, " fields over ", files, " files, ", arrays, " of them arrays; ", with_tables,
        " files declare read-only table storage outside it"
    );
    MESSAGE("  field types: ", Join(types));
    MESSAGE("  field names: ", JoinCounts(prefixes));

    CHECK(failures.empty());
    CHECK(files == 94);
    // State migration pairs fields on shape, so this is the whole vocabulary a state allocator
    // produces.
    CHECK(types == std::set<std::string>{"FAUSTFLOAT", "Soundfile*", "double", "int"});
}
