// Revision-stamped query engine with early cutoff, over the path-addressed layers only:
// below Term everything is content-addressed.
#pragma once

#include "box/Box.h"
#include "eval/Env.h"
#include "eval/Eval.h"
#include "files/Vfs.h"
#include "syntax/Parser.h"

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace faustlens {

enum class QueryKind : uint8_t { FileText, VfsRevision, Resolve, Terms, FileEnv };

struct QueryKey {
    QueryKind Kind = QueryKind::VfsRevision;
    std::string A, B;
    bool operator<(const QueryKey &o) const {
        if (Kind != o.Kind) return Kind < o.Kind;
        if (A != o.A) return A < o.A;
        return B < o.B;
    }
    bool operator==(const QueryKey &o) const { return Kind == o.Kind && A == o.A && B == o.B; }
};

struct TermsResult {
    ValueId Root = NoTerm;
    RefTree Refs;
    TokenVector Tokens;
    std::vector<Diagnostic> Diags;
    std::string Text; // the bytes the spans address
};

struct FileEnvResult {
    EnvId Env = NilEnv;
    std::vector<Binding> Bindings; // flat merge of this file's and every import's
    StrId File = 0;
    MetaSet Meta;
    std::vector<Diagnostic> Diags;
    bool Cycle = false; // set on the re-entrant call that closes an import cycle
};

// Owns everything a compile reads: terms, boxes, environments, evaluator, VFS.
struct Session {
    // One moment of a query's bookkeeping: when its inputs last moved, and what it read.
    struct Entry {
        uint64_t ChangedAt = 0, VerifiedAt = 0;
        std::vector<QueryKey> Deps;
        bool InFlight = false;
        bool Uncached = false; // on an import cycle
        uint32_t Recomputes = 0;
    };

    Vfs Vfs;
    Terms Terms;
    Boxes Boxes;
    Envs Envs;
    Evaluator Eval;
    uint64_t Revision = 1;
    std::string Root;
    // The last `Process`'s file-level `declare`s from the import closure, plus
    // the definition-level ones evaluation reached.
    MetaSet Metadata;
    std::vector<Diagnostic> EvalDiags;

    std::map<QueryKey, Entry> Entries;
    std::vector<QueryKey> Stack; // in-flight, for the cycle detector
    std::map<std::string, TermsResult> TermsResults;
    std::map<std::string, FileEnvResult> EnvResults;
    std::map<QueryKey, std::optional<std::string>> ResolveResults;

    Session();

    // Input writes, each bumping the revision.
    void SetBuffer(const std::string &path, std::string text);
    void ClearBuffer(const std::string &path);
    // The file changed on disk, with no buffer involved to move `ChangedAt`.
    void Touch(const std::string &path);
    void AddSearchPath(std::filesystem::path);

    const TermsResult &TermsOf(const std::string &path);
    const FileEnvResult &FileEnv(const std::string &path);
    std::optional<std::string> Resolve(const std::string &spec, const std::string &importer);

    // `process` of `path` in propagation normal form, or `Boxes::error()`.
    BoxId Process(const std::string &path);
    // Deterministically ordered.
    std::vector<Diagnostic> Diagnostics() const;

    uint32_t Recomputes(QueryKind, const std::string &path) const;

    // Every file ever parsed, deliberately wider than the current program's set: a stale
    // watch costs a recompile, a missing one an unheard edit.
    std::vector<std::string> Parsed() const;

    // Stamp over every `FileEnv`'s `ChangedAt`, so a caller can see if any moved.
    uint64_t FileEnvGeneration() const;
    Entry &EntryFor(const QueryKey &);
    void Record(const QueryKey &dep);
    bool NeedsRecompute(const QueryKey &, Entry &);
    void ComputeTerms(const std::string &path);
    void ComputeFileEnv(const std::string &path);
};

} // namespace faustlens
