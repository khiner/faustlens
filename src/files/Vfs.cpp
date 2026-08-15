#include "files/Vfs.h"

#include "files/Stdlib.h"

#include <format>
#include <fstream>
#include <iterator>

namespace faustlens {

std::expected<std::string, std::string> ReadFile(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::unexpected(std::format("could not open {}", p.string()));
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Vfs::Vfs() {
    for (const StdlibEntry &e : EmbeddedStdlib()) Embedded.emplace(e.Path, e.Text);
}

void Vfs::SetBuffer(std::string path, std::string text) { Buffers[std::move(path)] = std::move(text); }

void Vfs::ClearBuffer(const std::string &path) { Buffers.erase(path); }

void Vfs::AddSearchPath(std::filesystem::path p) { SearchPaths.push_back(std::move(p)); }

std::optional<Resolved> Vfs::TryDisk(const std::filesystem::path &p) const {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return std::nullopt;
    const std::string key = std::filesystem::weakly_canonical(p, ec).string();
    if (const auto it = Disk.find(key); it != Disk.end()) return Resolved{key, Origin::SearchPath, it->second};
    auto text = ReadFile(p);
    if (!text) return std::nullopt;
    const auto [it, _] = Disk.emplace(key, *std::move(text));
    return Resolved{key, Origin::SearchPath, it->second};
}

std::optional<Resolved> Vfs::Resolve(std::string_view spec, std::string_view importing_file) const {
    const std::string s(spec);
    const auto buffered = [this](std::string key) -> std::optional<Resolved> {
        const auto it = Buffers.find(key);
        if (it == Buffers.end()) return std::nullopt;
        return Resolved{std::move(key), Origin::Buffer, it->second};
    };

    if (auto r = buffered(s)) return r;

    // The importing file's own directory, per import and not accumulated. The reference
    // accumulates dirnames globally, which is order-dependent. Same result on the corpus.
    if (!importing_file.empty()) {
        const auto dir = std::filesystem::path(importing_file).parent_path();
        if (!dir.empty()) {
            const auto candidate = dir / s;
            if (auto r = buffered(candidate.string())) return r;
            if (auto r = TryDisk(candidate)) {
                r->Origin = Origin::ImportingDirectory;
                return r;
            }
        }
    }

    for (const auto &root : SearchPaths) {
        const auto candidate = root / s;
        if (auto r = buffered(candidate.string())) return r;
        if (auto r = TryDisk(candidate)) return r;
    }

    if (const auto it = Embedded.find(std::string_view(s)); it != Embedded.end()) return Resolved{s, Origin::EmbeddedStdlib, it->second};

    return std::nullopt;
}

void Vfs::Forget(const std::string &key) { Disk.erase(key); }

std::optional<std::string_view> Vfs::Read(const std::string &key) const {
    if (const auto it = Buffers.find(key); it != Buffers.end()) return it->second;
    if (const auto it = Disk.find(key); it != Disk.end()) return it->second;
    if (const auto it = Embedded.find(std::string_view(key)); it != Embedded.end()) return it->second;
    if (auto r = TryDisk(key)) return r->Text;
    return std::nullopt;
}

std::expected<void, std::string> Vfs::Eject(std::string_view spec, const std::filesystem::path &workspace) const {
    const auto it = Embedded.find(spec);
    if (it == Embedded.end()) return std::unexpected(std::format("no embedded library named {}", spec));
    const auto out = workspace / std::filesystem::path(std::string(spec));
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    std::ofstream f(out, std::ios::binary);
    if (!f) return std::unexpected(std::format("could not open {} for writing", out.string()));
    f.write(it->second.data(), std::streamsize(it->second.size()));
    if (!f.good()) return std::unexpected(std::format("could not write {}", out.string()));
    return {};
}

} // namespace faustlens
