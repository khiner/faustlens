#include "query/Query.h"

#include <algorithm>

namespace faustlens {
namespace {

QueryKey TextKey(const std::string &p) { return {QueryKind::FileText, p, {}}; }
QueryKey TermsKey(const std::string &p) { return {QueryKind::Terms, p, {}}; }
QueryKey EnvKey(const std::string &p) { return {QueryKind::FileEnv, p, {}}; }
QueryKey VfsKey() { return {QueryKind::VfsRevision, {}, {}}; }
QueryKey ResolveKey(const std::string &spec, const std::string &importer) { return {QueryKind::Resolve, spec, importer}; }

// A `declare`'s namespace key, so `math.lib`'s `author` is `math.lib/author`.
std::string_view Basename(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

Session::Session() : Eval(Terms, Boxes, Envs) {
    Entries[VfsKey()].ChangedAt = Revision;
    // A resolver map baked into the environment would be cyclic.
    Eval.Resolve = [this](std::string_view importer, std::string_view spec) {
        const auto target = Resolve(std::string(spec), std::string(importer));
        if (!target) return NilEnv;
        const FileEnvResult &r = FileEnv(*target);
        // The target contributes header metadata as an `import` does.
        for (const auto &[k, v] : r.Meta.Entries) Metadata.Add(k, v);
        return r.Env;
    };
}

Session::Entry &Session::EntryFor(const QueryKey &k) { return Entries[k]; }

void Session::Record(const QueryKey &dep) {
    if (Stack.empty()) return;
    Entry &e = Entries[Stack.back()];
    if (!std::ranges::contains(e.Deps, dep)) e.Deps.push_back(dep);
}

void Session::SetBuffer(const std::string &path, std::string text) {
    ++Revision;
    Vfs.SetBuffer(path, std::move(text));
    Entries[TextKey(path)].ChangedAt = Revision;
    // A buffer opening is also a change to what `Resolve` can see.
    Entries[VfsKey()].ChangedAt = Revision;
}

void Session::Touch(const std::string &path) {
    ++Revision;
    // Stamping alone would recompute from the cached read and repeat the answer.
    Vfs.Forget(path);
    Entries[TextKey(path)].ChangedAt = Revision;
}

void Session::ClearBuffer(const std::string &path) {
    ++Revision;
    Vfs.ClearBuffer(path);
    Entries[TextKey(path)].ChangedAt = Revision;
    Entries[VfsKey()].ChangedAt = Revision;
}

void Session::AddSearchPath(std::filesystem::path p) {
    ++Revision;
    Vfs.AddSearchPath(std::move(p));
    Entries[VfsKey()].ChangedAt = Revision;
}

bool Session::NeedsRecompute(const QueryKey &key, Entry &e) {
    if (e.VerifiedAt == Revision) return false;
    if (e.VerifiedAt == 0) return true; // never computed
    const std::vector<QueryKey> deps = e.Deps;
    bool stale = false;
    for (const QueryKey &d : deps) {
        switch (d.Kind) {
            case QueryKind::Terms: TermsOf(d.A); break;
            case QueryKind::FileEnv: FileEnv(d.A); break;
            case QueryKind::Resolve: Resolve(d.A, d.B); break;
            default: break; // inputs carry their own `ChangedAt`
        }
        if (Entries[d].ChangedAt > e.VerifiedAt) stale = true;
    }
    if (!stale) Entries[key].VerifiedAt = Revision;
    return stale;
}

std::optional<std::string> Session::Resolve(const std::string &spec, const std::string &importer) {
    const QueryKey key = ResolveKey(spec, importer);
    Record(key);
    // Depending on the VFS revision retries a failed resolution exactly when
    // something could have changed, without negative caching.
    Entry &probe = EntryFor(key);
    if (!NeedsRecompute(key, probe)) return ResolveResults[key];

    Stack.push_back(key);
    Entries[key].Deps.clear();
    Record(VfsKey());
    const auto found = Vfs.Resolve(spec, importer);
    Stack.pop_back();

    std::optional<std::string> now;
    if (found) now = found->Key;
    Entry &e = Entries[key];
    if (ResolveResults[key] != now) {
        ResolveResults[key] = now;
        e.ChangedAt = Revision;
    }
    e.VerifiedAt = Revision;
    ++e.Recomputes;
    return now;
}

void Session::ComputeTerms(const std::string &path) {
    const QueryKey key = TermsKey(path);
    Stack.push_back(key);
    Entries[key].Deps.clear();
    Record(TextKey(path));
    Stack.pop_back();

    TermsResult next;
    const auto text = Vfs.Read(path);
    if (!text) {
        next.Diags.push_back({.Code = Code::ResReadError, .Payload = path});
    } else {
        next.Text = std::string(*text);
        ParseResult r = Parse(Terms, next.Text);
        next.Root = r.Root;
        next.Refs = std::move(r.Refs);
        next.Tokens = std::move(r.Tokens);
        next.Diags = std::move(r.Diags);
    }

    Entry &e = Entries[key];
    // The one hand-written equality rule: spans shift on every whitespace edit, so
    // comparing whole results would defeat the cutoff.
    const auto it = TermsResults.find(path);
    const bool same = it != TermsResults.end() && it->second.Root == next.Root && it->second.Diags.size() == next.Diags.size();
    if (!same) e.ChangedAt = Revision;
    TermsResults[path] = std::move(next);
    e.VerifiedAt = Revision;
    ++e.Recomputes;
}

const TermsResult &Session::TermsOf(const std::string &path) {
    const QueryKey key = TermsKey(path);
    Record(key);
    Entry &probe = EntryFor(key);
    if (NeedsRecompute(key, probe)) ComputeTerms(path);
    return TermsResults[path];
}

void Session::ComputeFileEnv(const std::string &path) {
    const QueryKey key = EnvKey(path);
    Stack.push_back(key);
    Entries[key].Deps.clear();

    const TermsResult &t = TermsOf(path);
    FileEnvResult next;
    next.Diags = t.Diags;

    // Only `import` resolves here, its bindings merging into this layer. `component` and
    // `library` need two interned layers referencing each other, so the resolver takes them.
    if (t.Root != NoTerm) {
        const auto stmts = Terms.Children(t.Root);
        // Copied out: every call below can grow the pool the span points into.
        const std::vector<ValueId> statements(stmts.begin(), stmts.end());
        std::vector<Binding> imported;
        for (const ValueId s : statements) {
            if (Terms.KindOf(s) != Kind::Import) continue;
            const std::string spec(StripQuotes(Terms.Lexeme(s)));
            const auto target = Resolve(spec, path);
            if (!target) {
                next.Diags.push_back({.Code = Code::ResFileNotFound, .Subject = s, .Payload = spec});
                continue;
            }
            const FileEnvResult &sub = FileEnv(*target);
            if (sub.Cycle) {
                next.Diags.push_back({.Code = Code::ResImportCycle, .Subject = s, .Payload = spec});
                continue;
            }
            // Imports flatten into one layer, so two files defining a name collide.
            for (const Binding &b : sub.Bindings) {
                const auto seen = std::ranges::find_if(imported, [&](const Binding &x) { return x.Name == b.Name; });
                if (seen == imported.end()) imported.push_back(b);
                else *seen = b;
            }
            for (const auto &[k2, v2] : sub.Meta.Entries) next.Meta.Add(k2, v2);
        }

        next.File = Terms.InternStr(path);
        const FileLayer layer = Eval.BuildLayer(statements, NilEnv, next.File, Basename(path), path == Root, imported);
        next.Env = layer.Env;
        next.Bindings = layer.Bindings;
        for (const auto &[k2, v2] : layer.Meta.Entries) next.Meta.Add(k2, v2);
    }
    Stack.pop_back();

    Entry &e = Entries[key];
    const auto it = EnvResults.find(path);
    // Interned, so an edit that re-derives the same environment cuts off here.
    if (it == EnvResults.end() || it->second.Env != next.Env) e.ChangedAt = Revision;
    EnvResults[path] = std::move(next);
    e.VerifiedAt = Revision;
    ++e.Recomputes;
}

const FileEnvResult &Session::FileEnv(const std::string &path) {
    const QueryKey key = EnvKey(path);
    Record(key);
    // An import cycle is a query cycle. Nothing on the cycle is cached, since its
    // result depends on which file was queried first.
    if (Entries[key].InFlight) {
        for (const QueryKey &k : Stack) Entries[k].Uncached = true;
        static const FileEnvResult Cycle = [] {
            FileEnvResult r;
            r.Cycle = true;
            return r;
        }();
        return Cycle;
    }
    Entry &probe = EntryFor(key);
    if (probe.Uncached || NeedsRecompute(key, probe)) {
        Entries[key].InFlight = true;
        ComputeFileEnv(path);
        Entries[key].InFlight = false;
        if (Entries[key].Uncached) Entries[key].VerifiedAt = 0; // recomputed each revision
    }
    return EnvResults[path];
}

BoxId Session::Process(const std::string &path) {
    if (Root != path) {
        ++Revision; // the `declare` namespacing rule reads which file is root
        Root = path;
    }
    Eval.Diags.clear();
    Eval.Meta.Entries.clear();
    const uint64_t before = FileEnvGeneration();
    const FileEnvResult &env = FileEnv(path);
    // A `component` target is in no importing layer, so a change to it moves no
    // layer id and ordinary invalidation cannot see it.
    if (FileEnvGeneration() != before) Eval.ClearMemo();
    Metadata = env.Meta;
    // Set before evaluation, so a failing file still has a name. Appended, not replaced,
    // so a literal `filename` prints first.
    const std::string_view base = Basename(path);
    const auto &entries = Metadata.Entries;
    const bool named = std::ranges::any_of(entries, [](const auto &e) { return e.first == "name"; });
    if (!named) {
        std::string_view stem = base;
        if (stem.size() > 4 && stem.substr(stem.size() - 4) == ".dsp") stem.remove_suffix(4);
        Metadata.Add("name", std::string(stem));
    }
    Metadata.Add("filename", std::string(base));
    if (env.Env == NilEnv) return Boxes.Error;
    const BoxId b = Eval.EvalEntry(env.Env, 0);
    for (const auto &[k, v] : Eval.Meta.Entries) Metadata.Add(k, v);
    EvalDiags = Eval.Diags;
    return b;
}

std::vector<Diagnostic> Session::Diagnostics() const {
    std::vector<Diagnostic> all;
    for (const auto &[path, r] : EnvResults) all.insert(all.end(), r.Diags.begin(), r.Diags.end());
    all.insert(all.end(), EvalDiags.begin(), EvalDiags.end());
    SortAndDedupe(all);
    return all;
}

uint64_t Session::FileEnvGeneration() const {
    uint64_t g = 0;
    for (const auto &[k, e] : Entries)
        if (k.Kind == QueryKind::FileEnv) g += e.ChangedAt;
    return g;
}

uint32_t Session::Recomputes(QueryKind kind, const std::string &path) const {
    const auto it = Entries.find(QueryKey{kind, path, {}});
    return it == Entries.end() ? 0 : it->second.Recomputes;
}

std::vector<std::string> Session::Parsed() const {
    std::vector<std::string> out;
    // `Entries` is ordered, so this is too.
    for (const auto &[key, entry] : Entries)
        if (key.Kind == QueryKind::Terms) out.push_back(key.A);
    return out;
}

} // namespace faustlens
