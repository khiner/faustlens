#include "files/Vfs.h"
#include "files/Stdlib.h"

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace faustlens;

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path Path;
    explicit TempDir(const char *name) : Path(fs::temp_directory_path() / name) {
        fs::remove_all(Path);
        fs::create_directories(Path);
    }
    ~TempDir() { fs::remove_all(Path); }

    fs::path Write(const std::string &rel, std::string_view text) const {
        const fs::path p = Path / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }
};

} // namespace

TEST_CASE("the embedded standard library is path-keyed and recursive") {
    const auto entries = EmbeddedStdlib();
    // 43 top-level files plus the 13 in subdirectories, which import by subdirectory path.
    CHECK(entries.size() == 56);

    Vfs const vfs;
    for (const char *spec : {"stdfaust.lib", "maths.lib", "dx7/operator.lib", "old/music.lib"}) {
        CAPTURE(spec);
        const auto r = vfs.Resolve(spec, "");
        REQUIRE(r.has_value());
        CHECK(r->Origin == Origin::EmbeddedStdlib);
        CHECK(!r->Text.empty());
    }
    CHECK(!vfs.Resolve("no/such.lib", "").has_value());

    CHECK(std::string_view(EmbeddedStdlibSha()).size() == 40);
}

TEST_CASE("an open buffer shadows everything below it") {
    // Without this, `.lib` edits take effect only on save.
    Vfs vfs;
    const auto before = vfs.Resolve("maths.lib", "");
    REQUIRE(before.has_value());
    CHECK(before->Origin == Origin::EmbeddedStdlib);

    vfs.SetBuffer("maths.lib", "PI = 3;\n");
    const auto after = vfs.Resolve("maths.lib", "");
    REQUIRE(after.has_value());
    CHECK(after->Origin == Origin::Buffer);
    CHECK(after->Text == "PI = 3;\n");

    vfs.ClearBuffer("maths.lib");
    CHECK(vfs.Resolve("maths.lib", "")->Origin == Origin::EmbeddedStdlib);
}

TEST_CASE("the importing file's own directory comes before the search path") {
    // Taken per import rather than accumulated, so resolution stays deterministic.
    const TempDir dir("faustlens_vfs_layer2");
    dir.Write("dsp/music.lib", "local = 1;\n");
    dir.Write("elsewhere/music.lib", "shared = 2;\n");
    const auto importer = dir.Write("dsp/echo.dsp", "import(\"music.lib\");\n");

    Vfs vfs;
    vfs.AddSearchPath(dir.Path / "elsewhere");

    const auto from_dsp = vfs.Resolve("music.lib", importer.string());
    REQUIRE(from_dsp.has_value());
    CHECK(from_dsp->Origin == Origin::ImportingDirectory);
    CHECK(from_dsp->Text == "local = 1;\n");

    const auto other = dir.Write("other/thing.dsp", "");
    const auto from_search = vfs.Resolve("music.lib", other.string());
    REQUIRE(from_search.has_value());
    CHECK(from_search->Origin == Origin::SearchPath);
    CHECK(from_search->Text == "shared = 2;\n");
}

TEST_CASE("a failed resolution is retried when the file appears") {
    const TempDir dir("faustlens_vfs_retry");
    Vfs vfs;
    vfs.AddSearchPath(dir.Path);
    CHECK(!vfs.Resolve("later.lib", "").has_value());
    dir.Write("later.lib", "x = 1;\n");
    const auto found = vfs.Resolve("later.lib", "");
    REQUIRE(found.has_value());
    CHECK(found->Text == "x = 1;\n");
}

TEST_CASE("ejecting writes an embedded library into the workspace") {
    // Eject is the only path to a modified library, so the embedded copy is never changed.
    const TempDir workspace("faustlens_vfs_eject");
    Vfs vfs;
    REQUIRE(vfs.Eject("dx7/operator.lib", workspace.Path));
    CHECK(fs::is_regular_file(workspace.Path / "dx7" / "operator.lib"));
    CHECK(!vfs.Eject("no/such.lib", workspace.Path));

    vfs.AddSearchPath(workspace.Path);
    const auto r = vfs.Resolve("dx7/operator.lib", "");
    REQUIRE(r.has_value());
    CHECK(r->Origin == Origin::SearchPath);
}
