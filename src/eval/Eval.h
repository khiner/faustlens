// Evaluate Term to Box with memo keys (value id, environment id).
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

// Environment closures cannot be applied.
inline constexpr uint8_t TermClosure = 0, EnvClosure = 1;

struct MetaSet {
    std::vector<std::pair<std::string, std::string>> Entries;

    void Add(std::string key, std::string value);
};

// File definitions merged with imported definitions.
struct FileLayer {
    EnvId Env = NilEnv;
    std::vector<Binding> Bindings;
    // Store declaration metadata on the interned layer for reuse after memo hits.
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
    // Normalize closures and pattern matchers to symbolic circuits.
    BoxId ToSymbolic(BoxId);
    // Lexical body context for symbolic lambda normalization.
    EnvId SymbolicEnvironment(ValueId lambda, EnvId);
    BoxId EvalEntry(EnvId, StrId name);

    FileLayer BuildLayer(std::span<const ValueId> stmts, EnvId parent, StrId file, std::string_view file_key, bool is_root, std::span<const Binding> imported);

    // Resolve (importer, spec) lazily to a file environment.
    using Resolver = std::function<EnvId(std::string_view importer, std::string_view spec)>;

    // Call after any file environment changes.
    void ClearMemo();

    std::unordered_map<uint64_t, BoxId> Memo;
    std::unordered_map<uint64_t, EnvId> SymbolicEnvs;
    std::unordered_map<uint64_t, BoxId> PmMemo; // on (rules, env)
    std::unordered_map<BoxId, BoxId> Symbolic;
    Resolver Resolve;
    std::vector<std::vector<std::pair<std::string, std::string>>> MetaGroups;
    uint32_t Depth = 0;
    ValueId SubjectNow = NoTerm; // source term for nested diagnostics

    StrId ProcessName = 0, LetrecBody = 0;

    // Copy children before Make or Push can reallocate their pools.
    std::vector<ValueId> TermKids(ValueId t) const {
        const auto k = Terms.Children(t);
        return {k.begin(), k.end()};
    }
    std::vector<BoxId> BoxKids(BoxId b) const {
        const auto k = Boxes.Children(b);
        return {k.begin(), k.end()};
    }

    BoxId RealEval(ValueId, EnvId, bool pattern);
    // Bind identifiers in pattern arguments and resolve application callees.
    BoxId EvalInPattern(ValueId, EnvId);
    BoxId EvalIdent(StrId, EnvId, ValueId subject);
    BoxId EvalBinding(const Binding &, EnvId layer);

    // Enable numeric-tuple folding only for source or desugared sequential composition.
    BoxId Compose(BoxKind, BoxId a, BoxId b, ValueId subject, bool fold = true);
    BoxId Apply(BoxId fun, std::span<const BoxId> args, ValueId subject);

    BoxId EvalCase(ValueId rules, EnvId);
    // Match one argument against all remaining rules.
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
    ValueId DefinitionTerm(std::span<const ValueId> clauses, ValueId subject);
    ValueId LetRecToWith(ValueId);
    ValueId NestLambda(std::span<const ValueId> params, ValueId body);

    void Raise(Code, ValueId subject, std::string payload = {}, Severity = Severity::Error);
    BoxId Fail(Code, ValueId subject, std::string payload = {});
};

} // namespace faustlens
