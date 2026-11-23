#include <fstream>
#include <iterator>

#include "Fixtures.hpp"
#include "Framework.hpp"

#include "core/Archive.hpp"
#include "core/Scanner.hpp"

using launcher::ExtractZarToFolder;
using launcher::IsSafeArchiveName;
using launcher::JobControl;
using launcher::PackFolderToZar;

namespace {

std::vector<std::uint8_t> Pattern(std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    std::uint32_t state = 12345;
    for (auto& byte : bytes) {
        state = state * 1103515245u + 12345u;
        byte = static_cast<std::uint8_t>(state >> 16);
    }
    return bytes;
}

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

fixture::Tree BigGame() {
    auto tree = fixture::Game("PPSA00020", "Round Trip");
    tree["Media/big.bin"] = Pattern((9u << 20) + 7);
    tree["Media/deep/er/small.txt"] = fixture::Bytes("x");
    tree["empty.dat"] = {};
    return tree;
}

void CheckTree(const std::filesystem::path& root, const fixture::Tree& tree) {
    for (const auto& [path, bytes] : tree) {
        CHECK(std::filesystem::is_regular_file(root / path));
        CHECK(ReadBytes(root / path) == bytes);
    }
}

}

TEST(PacksFolderAndExtractsItBackIdentically) {
    fixture::TempDir temp;
    const auto tree = BigGame();
    fixture::WriteFolder(temp.Path() / "games" / "Round Trip", tree);
    std::filesystem::create_directories(temp.Path() / "games" / "Round Trip" / "EmptyFolder");

    JobControl pack;
    const auto zar = temp.Path() / "games" / "Round Trip.zar";
    PackFolderToZar(temp.Path() / "games" / "Round Trip", zar, pack);
    CHECK(std::filesystem::is_regular_file(zar));
    CHECK(!std::filesystem::exists(temp.Path() / "games" / "Round Trip.zar.partial"));
    CHECK_EQ(pack.Done(), pack.Total());

    const auto scan = launcher::Scan({.roots = {temp.Path() / "games"}});
    CHECK_EQ(scan.games.size(), std::size_t{2});
    CHECK(launcher::GroupByTitleId(scan.games).at("PPSA00020").size() == 2);

    JobControl extract;
    const auto out = temp.Path() / "out" / "Round Trip";
    std::filesystem::create_directories(out.parent_path());
    ExtractZarToFolder(zar, "", out, extract);
    CheckTree(out, tree);
    CHECK(std::filesystem::is_directory(out / "EmptyFolder"));
    CHECK_EQ(extract.Done(), extract.Total());
}

TEST(ExtractsNestedArchiveWithoutTheTopFolder) {
    fixture::TempDir temp;
    const auto tree = fixture::Game("PPSA00021", "Nested");
    fixture::WriteZar(temp.Path() / "Nested.zar", fixture::Prefixed(tree, "Inner Name/"));
    const auto scan = launcher::Scan({.roots = {temp.Path()}});
    CHECK_EQ(scan.games.size(), std::size_t{1});
    const auto& game = scan.games.front();
    CHECK(launcher::FolderOutputFor(game) == temp.Path() / "Nested");
    JobControl control;
    ExtractZarToFolder(game.location, game.innerPrefix, launcher::FolderOutputFor(game), control);
    CheckTree(temp.Path() / "Nested", tree);
    CHECK(!std::filesystem::exists(temp.Path() / "Nested" / "Inner Name"));
}

TEST(DefaultOutputsSitBesideTheSource) {
    launcher::GameEntry folder;
    folder.location = std::filesystem::path("games") / "My Game";
    CHECK(launcher::ZarOutputFor(folder) == std::filesystem::path("games") / "My Game.zar");
    launcher::GameEntry zar;
    zar.location = std::filesystem::path("games") / "My Game.zar";
    CHECK(launcher::FolderOutputFor(zar) == std::filesystem::path("games") / "My Game");
}

TEST(RefusesExistingOutputsAndLeftovers) {
    fixture::TempDir temp;
    fixture::WriteFolder(temp.Path() / "A", fixture::Game("PPSA00022", "A"));
    fixture::WriteFolder(temp.Path(), {{"A.zar", fixture::Bytes("existing")}});
    JobControl control;
    CHECK_THROWS(PackFolderToZar(temp.Path() / "A", temp.Path() / "A.zar", control));
    CHECK(ReadBytes(temp.Path() / "A.zar") == fixture::Bytes("existing"));

    fixture::WriteFolder(temp.Path(), {{"B.zar.partial", fixture::Bytes("left over")}});
    CHECK_THROWS(PackFolderToZar(temp.Path() / "A", temp.Path() / "B.zar", control));
    CHECK(!std::filesystem::exists(temp.Path() / "B.zar"));

    fixture::WriteZar(temp.Path() / "C.zar", fixture::Game("PPSA00023", "C"));
    std::filesystem::create_directories(temp.Path() / "C");
    CHECK_THROWS(ExtractZarToFolder(temp.Path() / "C.zar", "", temp.Path() / "C", control));
}

TEST(RefusesFolderNamesAZarCantHold) {
    fixture::TempDir temp;
    auto collide = fixture::Game("PPSA00024", "A");
    collide["Media/Data.bin"] = fixture::Bytes("1");
    collide["Media/data.bin"] = fixture::Bytes("2");
    fixture::WriteFolder(temp.Path() / "Collide", collide);
    JobControl control;
    const auto media = std::distance(std::filesystem::directory_iterator(temp.Path() / "Collide" / "Media"), std::filesystem::directory_iterator{});
    if (media == 2) {
        CHECK_THROWS(PackFolderToZar(temp.Path() / "Collide", temp.Path() / "Collide.zar", control));
        CHECK(!std::filesystem::exists(temp.Path() / "Collide.zar"));
        CHECK(!std::filesystem::exists(temp.Path() / "Collide.zar.partial"));
    } else {
        std::cout << "  (case-collision part skipped: this file system ignores letter case)\n";
    }

    auto accented = fixture::Game("PPSA00025", "B");
    accented["Media/caf\xC3\xA9.bin"] = fixture::Bytes("1");
    fixture::WriteFolder(temp.Path() / "Accented", accented);
    CHECK_THROWS(PackFolderToZar(temp.Path() / "Accented", temp.Path() / "Accented.zar", control));

    fixture::WriteFolder(temp.Path() / "Linked", fixture::Game("PPSA00026", "C"));
    std::error_code linkError;
    std::filesystem::create_directory_symlink(temp.Path() / "Accented", temp.Path() / "Linked" / "link", linkError);
    if (!linkError) {
        CHECK_THROWS(PackFolderToZar(temp.Path() / "Linked", temp.Path() / "Linked.zar", control));
        CHECK(!std::filesystem::exists(temp.Path() / "Linked.zar.partial"));
    } else {
        std::cout << "  (link part skipped: creating symbolic links needs Developer Mode or admin rights on Windows)\n";
    }
}

TEST(RefusesArchiveNamesWindowsCantCreate) {
    fixture::TempDir temp;
    auto tree = fixture::Game("PPSA00027", "Bad");
    tree["Media/CON.txt"] = fixture::Bytes("x");
    fixture::WriteZar(temp.Path() / "Bad.zar", tree);
    JobControl control;
    CHECK_THROWS(ExtractZarToFolder(temp.Path() / "Bad.zar", "", temp.Path() / "Bad", control));
    CHECK(!std::filesystem::exists(temp.Path() / "Bad"));
    CHECK(!std::filesystem::exists(temp.Path() / "Bad.partial"));
}

TEST(CancelLeavesNothingBehind) {
    fixture::TempDir temp;
    fixture::WriteFolder(temp.Path() / "G", BigGame());
    JobControl control;
    control.Cancel();
    bool cancelled = false;
    try {
        PackFolderToZar(temp.Path() / "G", temp.Path() / "G.zar", control);
    } catch (const launcher::Cancelled&) {
        cancelled = true;
    }
    CHECK(cancelled);
    CHECK(!std::filesystem::exists(temp.Path() / "G.zar"));
    CHECK(!std::filesystem::exists(temp.Path() / "G.zar.partial"));

    fixture::WriteZar(temp.Path() / "H.zar", BigGame());
    cancelled = false;
    try {
        ExtractZarToFolder(temp.Path() / "H.zar", "", temp.Path() / "H", control);
    } catch (const launcher::Cancelled&) {
        cancelled = true;
    }
    CHECK(cancelled);
    CHECK(!std::filesystem::exists(temp.Path() / "H"));
    CHECK(!std::filesystem::exists(temp.Path() / "H.partial"));
}

TEST(SafeArchiveNames) {
    CHECK(IsSafeArchiveName("eboot.bin"));
    CHECK(IsSafeArchiveName("sce_sys"));
    CHECK(IsSafeArchiveName("console.txt"));
    CHECK(IsSafeArchiveName("COM10"));
    CHECK(!IsSafeArchiveName(""));
    CHECK(!IsSafeArchiveName(".."));
    CHECK(!IsSafeArchiveName("a:b"));
    CHECK(!IsSafeArchiveName("trailing."));
    CHECK(!IsSafeArchiveName("trailing "));
    CHECK(!IsSafeArchiveName("nul"));
    CHECK(!IsSafeArchiveName("Aux.dat"));
    CHECK(!IsSafeArchiveName("com1.txt"));
    CHECK(!IsSafeArchiveName("LPT9"));
    CHECK(!IsSafeArchiveName("caf\xC3\xA9"));
    CHECK(!IsSafeArchiveName("tab\there"));
}
