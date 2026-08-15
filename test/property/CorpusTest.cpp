// The four retentive-lens obligations over the corpus: tokens tile every byte, PutGet,
// Retentiveness, idempotent normalization.
#include "property/Corpus.h"
#include "property/Retentive.h"
#include "syntax/Edit.h"
#include "syntax/Parser.h"
#include "syntax/Printer.h"
#include "syntax/Splice.h"
#include "unit/Syntax.h"

#include "doctest.h"

#include <format>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using namespace faustlens;
using namespace faustlens::test;

namespace {

bool HoleFree(const Terms &t, ValueId root) {
    std::set<ValueId> seen;
    std::vector<ValueId> stack{root};
    while (!stack.empty()) {
        const ValueId v = stack.back();
        stack.pop_back();
        if (t.KindOf(v) == Kind::Hole) return false;
        if (!seen.insert(v).second) continue;
        for (const ValueId c : t.Children(v)) stack.push_back(c);
    }
    return true;
}

ValueId ReplaceAt(Terms &t, ValueId root, std::span<const uint32_t> path, size_t depth, ValueId replacement) {
    if (depth == path.size()) return replacement;
    const TermValue n = t.Get(root); // by value: the recursion below grows `Values`
    const Kind kind = t.KindOf(root);
    std::vector<ValueId> kids(t.Children(root).begin(), t.Children(root).end());
    kids[path[depth]] = ReplaceAt(t, kids[path[depth]], path, depth + 1, replacement);
    return t.Make(kind, n.Form, n.Variants, n.Payload, kids);
}

struct LiteralFinder {
    const Terms &T;
    std::unordered_map<ValueId, bool> Memo;

    bool Path(ValueId v, std::vector<uint32_t> &path) {
        if (!Has(v)) return false;
        while (T.KindOf(v) != Kind::Int && T.KindOf(v) != Kind::Real) {
            const auto kids = T.Children(v);
            for (uint32_t i = 0; i < kids.size(); ++i) {
                if (!Has(kids[i])) continue;
                path.push_back(i);
                v = kids[i];
                break;
            }
        }
        return true;
    }

    bool Has(ValueId v) {
        if (const auto it = Memo.find(v); it != Memo.end()) return it->second;
        Memo.emplace(v, false);
        bool has = T.KindOf(v) == Kind::Int || T.KindOf(v) == Kind::Real;
        for (const ValueId c : T.Children(v)) has = Has(c) || has;
        Memo[v] = has;
        return has;
    }
};

} // namespace

TEST_CASE("token coverage over the whole corpus") {
    size_t files = 0;
    for (const CorpusFile &f : WholeCorpus()) {
        INFO(f.Relative);
        CheckTiling(f.Text);
        ++files;
    }
    MESSAGE("token coverage over ", files, " files");
    CHECK(files == 693); // 341 tests + 296 examples + 56 libraries
}

TEST_CASE("the corpus parses, and the pinned rejections are rejected") {
    int accepted = 0, rejected = 0;
    std::vector<std::string> unexpected_rejections, unexpected_accepts;
    for (const CorpusFile &f : WholeCorpus()) {
        Terms terms;
        const ParseResult r = Parse(terms, f.Text);
        const bool ok = r.Diags.empty();
        if (IsPinnedRejection(f.Relative)) {
            if (ok) unexpected_accepts.push_back(f.Relative);
            ++rejected;
            continue;
        }
        if (!ok) {
            unexpected_rejections.push_back(std::format("{}: {} at {}", f.Relative, CodeName(r.Diags[0].Code), r.Diags[0].Begin));
        } else {
            ++accepted;
        }
    }
    for (const std::string &s : unexpected_rejections) MESSAGE("rejected: ", s);
    for (const std::string &s : unexpected_accepts) MESSAGE("wrongly accepted: ", s);
    CHECK(unexpected_rejections.empty());
    CHECK(unexpected_accepts.empty());
    MESSAGE("accepted ", accepted, ", pinned rejections ", rejected);
    CHECK(accepted == 671); // 319 tests + 296 examples + 56 libraries
    CHECK(rejected == 22);
}

TEST_CASE("PutGet over the whole corpus") {
    std::vector<std::string> failures;
    size_t checked = 0;
    for (const CorpusFile &f : WholeCorpus()) {
        Terms terms;
        const ParseResult a = Parse(terms, f.Text);
        // Hole-free only: a hole's bytes reparsed in isolation need not recover the same shape.
        if (!a.Diags.empty() || !HoleFree(terms, a.Root)) continue;
        ++checked;
        const std::string printed = PrintTerm(terms, a.Root);
        const ParseResult b = Parse(terms, printed);
        if (!b.Diags.empty()) {
            failures.push_back(std::format("{}: reprint does not parse ({})", f.Relative, CodeName(b.Diags[0].Code)));
        } else if (a.Root != b.Root) {
            failures.push_back(f.Relative + ": reprint parses to a different value");
        }
    }
    for (const std::string &s : failures) MESSAGE(s);
    MESSAGE("PutGet over ", checked, " files");
    CHECK(failures.empty());
}

TEST_CASE("Hippocraticness: an identity edit at every ref is byte-identical") {
    std::vector<std::string> failures;
    size_t refs = 0;
    for (const CorpusFile &f : WholeCorpus()) {
        Loaded l(f);
        if (!l.R.Diags.empty()) continue;
        for (RefId i = 0; i < l.R.Refs.Refs.size(); ++i) {
            ++refs;
            if (l.Ctx->Splice(i, l.R.Refs.Refs[i].ValueId).empty()) continue;
            failures.push_back(std::format("{} ref {} ({})", f.Relative, i, KindName(l.Terms.KindOf(l.R.Refs.Refs[i].ValueId))));
            break;
        }
    }
    for (const std::string &s : failures) MESSAGE(s);
    MESSAGE("identity edits at ", refs, " refs");
    CHECK(failures.empty());
}

TEST_CASE("Retentiveness: a changed literal leaves every other node's bytes alone") {
    const Merged m = SweepFiles([](const CorpusFile &f, Loaded &l, Sweep &sw) {
        LiteralFinder literals{l.Terms};
        const ValueId replacement = l.Terms.MakeLeaf(Kind::Int, l.Terms.InternStr("987654"));
        struct Rewrite {
            ValueId Value;
            std::vector<uint32_t> Path;
        };
        std::unordered_map<ValueId, Rewrite> rewrites;
        size_t budget = ReparseBudget;
        for (RefId i = 0; i < l.R.Refs.Refs.size(); ++i) {
            const TermRef &t = l.R.Refs.Refs[i];
            auto it = rewrites.find(t.ValueId);
            if (it == rewrites.end()) {
                Rewrite made{t.ValueId, {}};
                if (literals.Path(t.ValueId, made.Path)) made.Value = ReplaceAt(l.Terms, t.ValueId, made.Path, 0, replacement);
                it = rewrites.emplace(t.ValueId, std::move(made)).first;
            }
            const Rewrite &rw = it->second;
            if (rw.Value == t.ValueId) continue; // no literal under it, or already 987654
            const EditScript script = l.Ctx->Splice(i, rw.Value);
            ++sw.A;
            if (const char *why = BadScript(script, t)) {
                sw.Failures.push_back(std::format("{} ref {}: {}", f.Relative, i, why));
                break;
            }
            if (const RefId lost = LostBytes(l, f.Text, i, rw.Path, NoRef, script); lost != NoRef) {
                sw.Failures.push_back(
                    std::format("{} ref {}: ref {} ({}) lost its bytes", f.Relative, i, lost, KindName(l.Terms.KindOf(l.R.Refs.Refs[lost].ValueId)))
                );
                break;
            }
            if (budget == 0) continue;
            --budget;
            ++sw.B;
            Terms fresh;
            if (!Parse(fresh, ApplyScript(f.Text, script)).Diags.empty()) {
                sw.Failures.push_back(std::format("{} ref {}: spliced text does not parse", f.Relative, i));
                break;
            }
        }
    });
    for (const std::string &s : m.Failures) MESSAGE(s);
    MESSAGE("literal rewrites at ", m.A, " refs, ", m.B, " reparsed", ", skipping ", m.Skipped, " generated");
    CHECK(m.Failures.empty());
}

TEST_CASE("Retentiveness: an inserted stage keeps the comments under it") {
    const Merged m = SweepFiles([](const CorpusFile &f, Loaded &l, Sweep &sw) {
        const ValueId probe = l.Terms.MakeLeaf(Kind::Ident, l.Terms.InternStr("fl_probe"));
        size_t budget = ReparseBudget;
        for (RefId i = 0; i < l.R.Refs.Refs.size(); ++i) {
            const TermRef &t = l.R.Refs.Refs[i];
            if (!l.AcceptsExpression(i)) continue;
            ValueId rewritten;
            if (l.Terms.KindOf(t.ValueId) == Kind::Seq) {
                const ValueId inner = l.Terms.Make(Kind::Seq, {probe, l.Terms.Child(t.ValueId, 1)});
                rewritten = l.Terms.Make(Kind::Seq, {l.Terms.Child(t.ValueId, 0), inner});
            } else {
                rewritten = l.Terms.Make(Kind::Seq, {t.ValueId, probe});
            }
            const EditScript script = l.Ctx->Splice(i, rewritten);
            ++sw.A;
            if (const char *why = BadScript(script, t)) {
                sw.Failures.push_back(std::format("{} ref {}: {}", f.Relative, i, why));
                break;
            }
            if (!CommentsSalvaged(l, f, script, sw.B)) {
                sw.Failures.push_back(std::format("{} ref {}: comment lost", f.Relative, i));
                break;
            }
            if (budget == 0) continue;
            --budget;
            Terms fresh;
            if (!Parse(fresh, ApplyScript(f.Text, script)).Diags.empty()) {
                sw.Failures.push_back(std::format("{} ref {}: spliced text does not parse", f.Relative, i));
                break;
            }
        }
    });
    Report(m, "stage insertions");
}

TEST_CASE("Retentiveness: rewriting to a value from outside the target stays inside it") {
    const Merged m = SweepFiles([](const CorpusFile &f, Loaded &l, Sweep &sw) {
        if (l.R.Refs.Refs.size() < 4) return;
        // From the first statement, so it occurs only outside every target below.
        const auto top = l.R.Refs.Children(0);
        if (top.empty()) return;
        const uint32_t boundary = l.R.Refs.Refs[top.front()].OuterEnd;
        ValueId outside = NoTerm;
        for (RefId j = 1; j < l.R.Refs.Refs.size(); ++j) {
            const TermRef &o = l.R.Refs.Refs[j];
            if (o.OuterBegin >= boundary) break;
            if (o.OuterEnd <= boundary && IsExpression(l.Terms.KindOf(o.ValueId))) outside = o.ValueId;
        }
        if (outside == NoTerm) return;
        for (RefId i = 0; i < l.R.Refs.Refs.size(); ++i) {
            const TermRef &t = l.R.Refs.Refs[i];
            if (t.OuterBegin < boundary || !l.AcceptsExpression(i)) continue;
            if (const char *why = BadScript(l.Ctx->Splice(i, outside), t)) {
                sw.Failures.push_back(std::format("{} ref {}: {}", f.Relative, i, why));
                break;
            }
            ++sw.A;
        }
    });
    for (const std::string &s : m.Failures) MESSAGE(s);
    MESSAGE("out-of-target rewrites at ", m.A, " refs", ", skipping ", m.Skipped, " generated");
    CHECK(m.Failures.empty());
    CHECK(m.A > 1000);
}

TEST_CASE("normalization idempotence") {
    // Reprinted bytes come out canonically parenthesized, so a second splice changes nothing.
    std::vector<std::string> failures;
    size_t files = 0;
    for (const CorpusFile &f : WholeCorpus()) {
        Loaded l(f);
        if (!l.R.Diags.empty()) continue;
        LiteralFinder literals{l.Terms};
        const ValueId replacement = l.Terms.MakeLeaf(Kind::Int, l.Terms.InternStr("987654"));

        std::string once;
        for (RefId i = 0; i < l.R.Refs.Refs.size() && once.empty(); ++i) {
            std::vector<uint32_t> path;
            if (!literals.Path(l.R.Refs.Refs[i].ValueId, path)) continue;
            const ValueId rewritten = ReplaceAt(l.Terms, l.R.Refs.Refs[i].ValueId, path, 0, replacement);
            if (rewritten == l.R.Refs.Refs[i].ValueId) continue;
            const EditScript script = l.Ctx->Splice(i, rewritten);
            if (!script.empty()) once = ApplyScript(f.Text, script);
        }
        if (once.empty()) continue;
        ++files;

        Terms t2;
        const ParseResult r2 = Parse(t2, once);
        if (!r2.Diags.empty()) {
            failures.push_back(f.Relative + ": spliced text does not parse");
            continue;
        }
        const SpliceContext c2(t2, once, r2.Refs, r2.Tokens);
        for (RefId k = 0; k < r2.Refs.Refs.size(); ++k) {
            if (c2.Splice(k, r2.Refs.Refs[k].ValueId).empty()) continue;
            failures.push_back(std::format("{}: second splice at ref {} changed bytes", f.Relative, k));
            break;
        }
    }
    for (const std::string &s : failures) MESSAGE(s);
    MESSAGE("idempotence over ", files, " spliced files");
    CHECK(failures.empty());
}
