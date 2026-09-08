#pragma once

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace faustlens {

std::expected<std::string, std::string> ReadFile(const std::filesystem::path &);

enum class Origin { Buffer, ImportingDirectory, SearchPath, EmbeddedStdlib };

struct Resolved {
    // Canonicalize disk paths; preserve registered buffer paths and embedded-library specs.
    std::string Key;
    Origin Origin = Origin::EmbeddedStdlib;
    std::string_view Text;
};

// Resolve through buffers, importer directory, disk search paths, then embedded libraries.
struct Vfs {
    std::vector<std::filesystem::path> SearchPaths;
    std::map<std::string, std::string> Buffers;
    mutable std::map<std::string, std::string> Disk;
    std::map<std::string_view, std::string_view> Embedded;

    Vfs();

    void SetBuffer(std::string path, std::string text);
    void ClearBuffer(const std::string &path);

    void AddSearchPath(std::filesystem::path);

    // Retry failed reads on subsequent calls.
    std::optional<Resolved> Resolve(std::string_view spec, std::string_view importing_file) const;

    std::optional<std::string_view> Read(const std::string &key) const;

    // Invalidate the cached disk read and all returned text views.
    void Forget(const std::string &key);

    // Copy an embedded library into workspace as an overlay.
    std::expected<void, std::string> Eject(std::string_view spec, const std::filesystem::path &workspace) const;

    std::optional<Resolved> TryDisk(const std::filesystem::path &) const;
};

} // namespace faustlens
