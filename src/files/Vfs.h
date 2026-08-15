#pragma once

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace faustlens {

// Outside the VFS, for callers reading files that are no layer of the overlay.
std::expected<std::string, std::string> ReadFile(const std::filesystem::path &);

enum class Origin { Buffer, ImportingDirectory, SearchPath, EmbeddedStdlib };

struct Resolved {
    // What `Read` takes back. Canonicalized only for a file found on disk: a buffer keeps
    // its registered path, the stdlib its spec.
    std::string Key;
    Origin Origin = Origin::EmbeddedStdlib;
    std::string_view Text;
};

// Resolution is an overlay: open editor buffers, the importing file's own directory,
// the search path on disk, then the embedded stdlib.
struct Vfs {
    std::vector<std::filesystem::path> SearchPaths;
    std::map<std::string, std::string> Buffers;
    mutable std::map<std::string, std::string> Disk; // read-through cache
    std::map<std::string_view, std::string_view> Embedded;

    Vfs();

    // An unsaved buffer, keyed by the path it would be saved to.
    void SetBuffer(std::string path, std::string text);
    void ClearBuffer(const std::string &path);

    void AddSearchPath(std::filesystem::path);

    // A failure is never cached, so a missing file is retried when it appears.
    std::optional<Resolved> Resolve(std::string_view spec, std::string_view importing_file) const;

    std::optional<std::string_view> Read(const std::string &key) const;

    // Drops a cached disk read, for a file edited outside the app. Invalidates every
    // view `Read` handed out.
    void Forget(const std::string &key);

    // Copies an embedded library into `workspace`, shadowing the embedded copy.
    std::expected<void, std::string> Eject(std::string_view spec, const std::filesystem::path &workspace) const;

    std::optional<Resolved> TryDisk(const std::filesystem::path &) const;
};

} // namespace faustlens
