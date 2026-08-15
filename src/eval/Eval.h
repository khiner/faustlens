// Term to Box: a memoized traversal keyed on `(value id, env id)`, never touching the VFS.
#pragma once

#include "box/Box.h"
#include "eval/Env.h"
#include "eval/Fold.h"
#include "syntax/Diagnostic.h"
#include "syntax/Term.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace faustlens {

// `Closure`'s form. An environment closure cannot be applied.
inline constexpr uint8_t TermClosure = 0, EnvClosure = 1;

struct MetaSet {
    std::vector<std::pair<std::string, std::string>> Entries;

    // Ignores a repeat of a pair it already holds.
    void Add(std::string key, std::string value);
};

// A file's flat binding list: its own definitions merged with every imported one.
struct FileLayer {
    EnvId Env = NilEnv;
    std::vector<Binding> Bindings;
    // On the layer, not the evaluator's set, which memoization leaves empty on a recompile.
    MetaSet Meta;
};

struct Evaluator {
    Terms &Terms;
    Boxes &Boxes;
    Envs &Envs;
    std::vector<Diagnostic> Diags;
    MetaSet Meta;

    Evaluator(faustlens::Terms &, faustlens::Boxes &, faustlens::Envs &);

    BoxId Eval(ValueId, EnvId);
    // Closures and pattern matchers become symbolic boxes: propagation normal form.
    BoxId ToSymbolic(BoxId);
    // `process` is a name, not a keyword.
    BoxId EvalEntry(EnvId, StrId name);

    FileLayer BuildLayer(std::span<const ValueId> stmts, EnvId parent, StrId file, std::string_view file_key, bool is_root, std::span<const Binding> imported);

    // Resolves (importer, spec) to a file env. A hook, not a map: resolution must be lazy.
    using Resolver = std::function<EnvId(std::string_view importer, std::string_view spec)>;

    // Call whenever any file environment moved.
    void ClearMemo();

    // The environment a value was evaluated under, recorded on the memo miss. First writer wins.
    struct Evaluated {
        EnvId Env = NilEnv;
        uint32_t Envs = 0; // zero where the program never reached this value
        BoxId Box = NoBox; // what the first environment gave
        bool Ambiguous = false; // and some other environment gave something else
    };
    Evaluated EvaluatedIn(ValueId) const;

    std::unordered_map<uint64_t, BoxId> Memo;
    std::unordered_map<ValueId, Evaluated> EvaluatedEnvs;
    std::unordered_map<uint64_t, BoxId> PmMemo; // on (rules, env)
    std::unordered_map<BoxId, BoxId> Symbolic;
    Resolver Resolve;
    std::vector<std::vector<std::pair<std::string, std::string>>> MetaGroups;
    uint32_t Depth = 0;
    ValueId SubjectNow = NoTerm; // the term a diagnostic raised deeper down names

    StrId ProcessName = 0, LetrecBody = 0;

    // Copied out: `Make` and `Push` grow the pools, so a span held across a construction dangles.
    std::vector<ValueId> TermKids(ValueId t) const {
        const auto k = Terms.Children(t);
        return {k.begin(), k.end()};
    }
    std::vector<BoxId> BoxKids(BoxId b) const {
        const auto k = Boxes.Children(b);
        return {k.begin(), k.end()};
    }

    BoxId RealEval(ValueId, EnvId, bool pattern);
    // A bare identifier in a pattern is a binder, not a reference, but not everywhere:
    // an application's callee still resolves.
    BoxId EvalInPattern(ValueId, EnvId);
    BoxId EvalIdent(StrId, EnvId, ValueId subject);
    BoxId EvalBinding(const Binding &, EnvId layer);

    // `fold` (numeric-tuple simplification) belongs only to a `:` the source or a desugaring wrote.
    BoxId Compose(BoxKind, BoxId a, BoxId b, ValueId subject, bool fold = true);
    BoxId Apply(BoxId fun, std::span<const BoxId> args, ValueId subject);

    BoxId EvalCase(ValueId rules, EnvId);
    // One argument against every live rule, narrowed state carried as a partial application.
    BoxId MatchArgument(BoxId pm, BoxId arg, ValueId subject);
    bool MatchPattern(BoxId pattern, BoxId arg, EnvId &rule_env);

    BoxId SimplifyPattern(BoxId);
    BoxId Iterate(IterKind, StrId var, int32_t n, ValueId body, EnvId, ValueId subject, bool pattern);
    EnvId Target(EnvId env, std::string_view spec) const;
    BoxId Modulate(ValueId modulator, ValueId body, EnvId, ValueId subject);
    BoxId Implant(BoxId box, std::span<const std::string> path, BoxId slot, int32_t ins, BoxId circuit, bool &matched);

    std::optional<Num> EvalNumber(ValueId, EnvId, ValueId subject);
    int32_t Eval2Int(ValueId, EnvId, ValueId subject, bool &ok);
    double Eval2Double(ValueId, EnvId, ValueId subject, bool &ok);
    StrId EvalLabel(StrId raw, EnvId, ValueId subject);

    std::vector<Binding> BindingsFromDefs(std::span<const ValueId> defs, ValueId subject, BindKind, EnvId closure_env);
    // A definition's four shapes: bare body, abstraction, one-rule `case`, or `case` over clauses.
    ValueId DefinitionTerm(std::span<const ValueId> clauses, ValueId subject);
    ValueId LetRecToWith(ValueId);
    ValueId NestLambda(std::span<const ValueId> params, ValueId body);

    void Raise(Code, ValueId subject, std::string payload = {}, Severity = Severity::Error);
    BoxId Fail(Code, ValueId subject, std::string payload = {});
};

} // namespace faustlens
