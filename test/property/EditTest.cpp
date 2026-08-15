// Every structural edit at every corpus ref: a disjoint script inside the target's span
// that keeps unrebuilt bytes and parses.
#include "syntax/Edit.h"
#include "property/Corpus.h"
#include "property/Retentive.h"
#include "syntax/Parser.h"
#include "syntax/Splice.h"

#include "doctest.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <span>
#include <string>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

struct Check {
    const CorpusFile &F;
    Loaded &L;
    Sweep &Sw;
    size_t Budget = ReparseBudget;

    // `retain` answers the first ref inside the target whose bytes the script dropped.
    template<class Retain> bool Run(const char *what, RefId at, const Edit &e, Retain retain) {
        const TermRef &t = L.R.Refs.Refs[e.Target];
        const EditScript script = L.Ctx->Splice(e.Target, e.Value);
        ++Sw.A;
        if (const char *why = BadScript(script, t)) return Fail(what, at, why);
        if (const RefId lost = retain(script); lost != NoRef)
            return Fail(what, at, std::format("ref {} ({}) lost its bytes", lost, KindName(L.Terms.KindOf(L.R.Refs.Refs[lost].ValueId))));
        if (!CommentsSalvaged(L, F, script, Sw.B)) return Fail(what, at, "comment lost");
        if (Budget == 0) return true;
        --Budget;
        Terms fresh;
        const ParseResult r = Parse(fresh, ApplyScript(F.Text, script));
        if (!r.Diags.empty()) return Fail(what, at, std::format("spliced text does not parse ({})", CodeName(r.Diags[0].Code)));
        return true;
    }

    bool Run(const char *what, RefId at, const Edit &e, const std::vector<uint32_t> &path = {}, RefId dropped = NoRef) {
        return Run(what, at, e, [&](const EditScript &script) { return LostBytes(L, F.Text, e.Target, path, dropped, script); });
    }

    bool Fail(const char *what, RefId at, const std::string &why) {
        Sw.Failures.push_back(std::format("{} {} at ref {}: {}", F.Relative, what, at, why));
        return false;
    }
};

void EntryRefs(const Loaded &l, RefId r, std::vector<RefId> &out) {
    if (l.Terms.KindOf(l.R.Refs.Refs[r].ValueId) != Kind::Par) {
        out.push_back(r);
        return;
    }
    for (const RefId c : l.R.Refs.Children(r)) EntryRefs(l, c, out);
}

// A `route` entry is draggable only where it is a plain decimal `Int`.
bool Decimal(const Loaded &l, RefId r, uint32_t *out) {
    const ValueId v = l.R.Refs.Refs[r].ValueId;
    if (l.Terms.KindOf(v) != Kind::Int) return false;
    const std::string_view s = l.Terms.Lexeme(v);
    return std::from_chars(s.data(), s.data() + s.size(), *out).ec == std::errc();
}

// Appending a pair rebuilds every `Par`, so what owes bytes is the counts and kept entries.
RefId LostEntry(const Loaded &l, std::string_view src, RefId route, std::span<const RefId> gone, const EditScript &script) {
    const TermRef &t0 = l.R.Refs.Refs[route];
    std::vector<RefId> stack{route};
    while (!stack.empty()) {
        const RefId r = stack.back();
        stack.pop_back();
        if (std::ranges::contains(gone, r)) continue;
        if (r != route && l.Terms.KindOf(l.R.Refs.Refs[r].ValueId) != Kind::Par && !Survives(l, src, l.R.Refs.Refs[r], t0, script)) return r;
        for (const RefId c : l.R.Refs.Children(r)) stack.push_back(c);
    }
    return NoRef;
}

struct Connective {
    Kind Kind;
    uint8_t Form;
};
constexpr Connective Connectives[] = {
    {Kind::Seq, 0},     {Kind::Par, 0}, {Kind::Split, 0}, {Kind::Merge, uint8_t(MergeSpelling::Colon)}, {Kind::Merge, uint8_t(MergeSpelling::Plus)},
    {Kind::RecComp, 0},
};

// `at` returns false to stop that file, so one failure is reported per file, not per ref.
template<class AtRef> Merged SweepRefs(AtRef at) {
    return SweepFiles([&at](const CorpusFile &f, Loaded &l, Sweep &sw) {
        EditContext ed(l.Terms, l.R.Refs);
        Check ck{f, l, sw};
        for (RefId i = 0; i < l.R.Refs.Refs.size(); ++i)
            if (!at(ck, ed, i)) break;
    });
}

} // namespace

TEST_CASE("wrap and insert: a new stage leaves every other node's bytes alone") {
    const Merged m = SweepRefs([](Check &ck, EditContext &ed, RefId i) {
        const ValueId probe = ck.L.Terms.MakeLeaf(Kind::Ident, ck.L.Terms.InternStr("fl_probe"));
        // One connective and side per ref, cycled, rather than twelve sweeps.
        const Connective &c = Connectives[i % std::size(Connectives)];
        const Side side = (i / std::size(Connectives)) % 2 ? Side::Before : Side::After;
        const Edit e = ed.Compose(i, c.Kind, c.Form, side, probe);
        return !e || ck.Run("compose", i, e);
    });
    Report(m, "compositions");
    CHECK(m.A == 403259);
}

TEST_CASE("delete: removing a stage leaves the other one's bytes alone") {
    const Merged m = SweepRefs([](Check &ck, EditContext &ed, RefId i) {
        const Edit e = ed.Delete(i);
        // The deleted stage is the one subtree nobody owes bytes to.
        return !e || ck.Run("delete", i, e, {}, i);
    });
    Report(m, "deletions");
    CHECK(m.A == 235198);
}

TEST_CASE("retext: a changed literal or label leaves its siblings' bytes alone") {
    const Merged m = SweepRefs([](Check &ck, EditContext &ed, RefId i) {
        const Kind k = ck.L.Terms.KindOf(ck.L.R.Refs.Refs[i].ValueId);
        const bool number = k == Kind::Int || k == Kind::Real;
        const Edit e = ed.Retext(i, number ? "987654" : "\"fl_probe\"");
        return !e || ck.Run("retext", i, e);
    });
    Report(m, "retexts");
    CHECK(m.A == 115367);
}

TEST_CASE("rewire: a route's other entries keep their bytes") {
    const Merged m = SweepRefs([](Check &ck, EditContext &ed, RefId i) {
        const Loaded &l = ck.L;
        if (l.Terms.KindOf(l.R.Refs.Refs[i].ValueId) != Kind::Route) return true;
        const auto kids = l.R.Refs.Children(i);
        std::vector<RefId> entries;
        if (kids.size() > 2) EntryRefs(l, kids[2], entries);
        const Edit add = ed.Connect(i, 1, 1);
        if (add && !ck.Run("connect", i, add, [&](const EditScript &s) { return LostEntry(l, ck.F.Text, i, {}, s); })) return false;
        uint32_t in = 0, out = 0;
        if (entries.size() < 2 || !Decimal(l, entries[0], &in) || !Decimal(l, entries[1], &out)) return true;
        const Edit drop = ed.Disconnect(i, in, out);
        const std::vector<RefId> gone{entries[0], entries[1]};
        return !drop || ck.Run("disconnect", i, drop, [&](const EditScript &s) { return LostEntry(l, ck.F.Text, i, gone, s); });
    });
    for (const std::string &s : m.Failures) MESSAGE(s);
    MESSAGE("rewires at ", m.A, " routes, skipping ", m.Skipped, " generated");
    CHECK(m.Failures.empty());
    CHECK(m.A == 46);
}

TEST_CASE("Hippocraticness: an identity edit from the catalogue is byte-identical") {
    const Merged m = SweepFiles([](const CorpusFile &f, Loaded &l, Sweep &sw) {
        EditContext ed(l.Terms, l.R.Refs);
        for (RefId i = 0; i < l.R.Refs.Refs.size(); ++i) {
            const ValueId v = l.R.Refs.Refs[i].ValueId;
            if (!HasLexeme(l.Terms.KindOf(v))) continue;
            const Edit e = ed.Retext(i, l.Terms.Lexeme(v));
            if (!e) continue;
            ++sw.A;
            if (e.Value != v) {
                sw.Failures.push_back(std::format("{} ref {}: retext to its own text built a different value", f.Relative, i));
                break;
            }
            if (!l.Ctx->Splice(e.Target, e.Value).empty()) {
                sw.Failures.push_back(std::format("{} ref {}: retext to its own text changed bytes", f.Relative, i));
                break;
            }
        }
        for (RefId i = 0; i < l.R.Refs.Refs.size(); ++i) {
            if (l.Terms.KindOf(l.R.Refs.Refs[i].ValueId) != Kind::Route) continue;
            const std::vector<ValueId> entries = ed.Entries(i);
            if (entries.size() < 2) continue;
            uint32_t in = 0, out = 0;
            if (l.Terms.KindOf(entries[0]) != Kind::Int || l.Terms.KindOf(entries[1]) != Kind::Int) continue;
            in = uint32_t(std::stoul(std::string(l.Terms.Lexeme(entries[0]))));
            out = uint32_t(std::stoul(std::string(l.Terms.Lexeme(entries[1]))));
            const Edit e = ed.Connect(i, in, out);
            if (!e) continue;
            ++sw.B;
            if (!l.Ctx->Splice(e.Target, e.Value).empty()) {
                sw.Failures.push_back(std::format("{} ref {}: connecting a wired pair changed bytes", f.Relative, i));
                break;
            }
        }
    });
    for (const std::string &s : m.Failures) MESSAGE(s);
    MESSAGE("identity retexts at ", m.A, " refs, identity connects at ", m.B, " routes");
    CHECK(m.Failures.empty());
    CHECK(m.A == 113714);
    CHECK(m.B == 21);
}
