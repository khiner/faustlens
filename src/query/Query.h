// Revision-stamped invalidation for path-addressed queries; interned values use content-keyed memos.
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

struct Session {
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
    // Declarations from the last Process import closure and evaluated definitions.
    MetaSet Metadata;
    std::vector<Diagnostic> EvalDiags;

    std::map<QueryKey, Entry> Entries;
    std::vector<QueryKey> Stack;
    std::map<std::string, TermsResult> TermsResults;
    std::map<std::string, FileEnvResult> EnvResults;
    std::map<QueryKey, std::optional<std::string>> ResolveResults;

    Session();

    // Increment the revision on input writes.
    void SetBuffer(const std::string &path, std::string text);
    void ClearBuffer(const std::string &path);
    // Notify a disk change for a file without a buffer.
    void Touch(const std::string &path);
    void AddSearchPath(std::filesystem::path);

    const TermsResult &TermsOf(const std::string &path);
    const FileEnvResult &FileEnv(const std::string &path);
    std::optional<std::string> Resolve(const std::string &spec, const std::string &importer);

    // Return normalized process or an Error box.
    BoxId Process(const std::string &path);
    std::vector<Diagnostic> Diagnostics() const;

    uint32_t Recomputes(QueryKind, const std::string &path) const;

    // Return every previously parsed file for watching, including files outside the current import closure.
    std::vector<std::string> Parsed() const;

    // Return a stamp covering all FileEnv changes.
    uint64_t FileEnvGeneration() const;
    Entry &EntryFor(const QueryKey &);
    void Record(const QueryKey &dep);
    bool NeedsRecompute(const QueryKey &, Entry &);
    void ComputeTerms(const std::string &path);
    void ComputeFileEnv(const std::string &path);
};

} // namespace faustlens
