#include <algorithm>

#include "Fixtures.hpp"
#include "Framework.hpp"

#include "core/Scanner.hpp"

using launcher::ExecutableKind;
using launcher::PackageFormat;
using launcher::Scan;
using launcher::ScanOptions;
using launcher::ZarLayout;

namespace {

const launcher::GameEntry* Find(const launcher::ScanResult& result, const std::string& titleId) {
    const auto found = std::find_if(result.games.begin(), result.games.end(), [&](const auto& game) { return game.metadata.titleId == titleId; });
    return found == result.games.end() ? nullptr : &*found;
}

bool HasProblemFor(const launcher::ScanResult& result, const std::filesystem::path& path) {
    return std::any_of(result.problems.begin(), result.problems.end(), [&](const auto& problem) { return problem.location == path; });
}

}

TEST(FindsFolderGamesAtAnyDepth) {
    fixture::TempDir temp;
    fixture::WriteFolder(temp.Path() / "PPSA00001", fixture::Game("PPSA00001", "First"));
    fixture::WriteFolder(temp.Path() / "nested" / "deeper" / "Second", fixture::Game("PPSA00002", "Second"));
    const auto result = Scan({.roots = {temp.Path()}});
    CHECK_EQ(result.games.size(), std::size_t{2});
    CHECK(result.problems.empty());
    const auto* first = Find(result, "PPSA00001");
    CHECK(first != nullptr);
    CHECK(first->format == PackageFormat::Folder);
    CHECK(first->executable.kind == ExecutableKind::Elf);
    CHECK_EQ(first->metadata.name, std::string("First"));
    CHECK_EQ(std::string(first->icon.begin(), first->icon.end()), std::string("not really a png"));
}

TEST(RootFolderItselfCanBeAGame) {
    fixture::TempDir temp;
    fixture::WriteFolder(temp.Path(), fixture::Game("PPSA00001", "First"));
    const auto result = Scan({.roots = {temp.Path()}});
    CHECK_EQ(result.games.size(), std::size_t{1});
}

TEST(FolderWithoutEbootIsNotAGame) {
    fixture::TempDir temp;
    auto tree = fixture::Game("PPSA00001", "First");
    tree.erase("eboot.bin");
    fixture::WriteFolder(temp.Path() / "Broken", tree);
    const auto result = Scan({.roots = {temp.Path()}});
    CHECK(result.games.empty());
    CHECK(result.problems.empty());
}

TEST(DoesNotDescendIntoAGame) {
    fixture::TempDir temp;
    fixture::WriteFolder(temp.Path() / "Outer", fixture::Game("PPSA00001", "Outer"));
    fixture::WriteFolder(temp.Path() / "Outer" / "Media" / "Inner", fixture::Game("PPSA00002", "Inner"));
    const auto result = Scan({.roots = {temp.Path()}});
    CHECK_EQ(result.games.size(), std::size_t{1});
    CHECK(Find(result, "PPSA00001") != nullptr);
}

TEST(SkipsExcludedFolders) {
    fixture::TempDir temp;
    fixture::WriteFolder(temp.Path() / "games" / "A", fixture::Game("PPSA00001", "A"));
    fixture::WriteFolder(temp.Path() / "library" / "PPSA00001" / "app0", fixture::Game("PPSA00001", "A"));
    const auto result = Scan({.roots = {temp.Path()}, .excluded = {temp.Path() / "library"}});
    CHECK_EQ(result.games.size(), std::size_t{1});
    CHECK(result.games.front().location == temp.Path() / "games" / "A");
}

TEST(ReportsBadMetadataAndKeepsScanning) {
    fixture::TempDir temp;
    auto broken = fixture::Game("PPSA00001", "A");
    broken["sce_sys/param.json"] = fixture::Bytes("{ not json");
    fixture::WriteFolder(temp.Path() / "Broken", broken);
    fixture::WriteFolder(temp.Path() / "Good", fixture::Game("PPSA00002", "B"));
    const auto result = Scan({.roots = {temp.Path()}});
    CHECK_EQ(result.games.size(), std::size_t{1});
    CHECK(HasProblemFor(result, temp.Path() / "Broken"));
}

TEST(ReportsMissingRoot) {
    fixture::TempDir temp;
    const auto missing = temp.Path() / "nope";
    const auto result = Scan({.roots = {missing}});
    CHECK(result.games.empty());
    CHECK(HasProblemFor(result, missing));
}

TEST(ReadsZarWithAppZeroAtRoot) {
    fixture::TempDir temp;
    auto tree = fixture::Game("PPSA00003", "Packed");
    tree["eboot.bin"] = fixture::Self();
    fixture::WriteZar(temp.Path() / "Packed.zar", tree);
    const auto result = Scan({.roots = {temp.Path()}});
    CHECK(result.problems.empty());
    const auto* game = Find(result, "PPSA00003");
    CHECK(game != nullptr);
    CHECK(game->format == PackageFormat::Zar);
    CHECK(game->layout == ZarLayout::Root);
    CHECK(game->executable.kind == ExecutableKind::Self);
    CHECK_EQ(game->metadata.name, std::string("Packed"));
    CHECK_EQ(game->icon.size(), std::size_t{16});
}

TEST(ReadsZarWithOneTopLevelFolder) {
    fixture::TempDir temp;
    fixture::WriteZar(temp.Path() / "Nested.ZAR", fixture::Prefixed(fixture::Game("PPSA00004", "Nested"), "Nested Game/"));
    const auto result = Scan({.roots = {temp.Path()}});
    CHECK(result.problems.empty());
    const auto* game = Find(result, "PPSA00004");
    CHECK(game != nullptr);
    CHECK(game->layout == ZarLayout::Nested);
    CHECK_EQ(game->innerPrefix, std::string("Nested Game/"));
    CHECK(game->executable.kind == ExecutableKind::Elf);
}

TEST(ZarWithoutGameOrDamagedIsAProblem) {
    fixture::TempDir temp;
    fixture::WriteZar(temp.Path() / "Empty.zar", {{"readme.txt", fixture::Bytes("hello")}});
    fixture::WriteFolder(temp.Path(), {{"Garbage.zar", fixture::Bytes("this is not a zarchive")}});
    auto two = fixture::Prefixed(fixture::Game("PPSA00005", "A"), "A/");
    two.merge(fixture::Prefixed(fixture::Game("PPSA00006", "B"), "B/"));
    fixture::WriteZar(temp.Path() / "Two.zar", two);
    const auto result = Scan({.roots = {temp.Path()}});
    CHECK(result.games.empty());
    CHECK(HasProblemFor(result, temp.Path() / "Empty.zar"));
    CHECK(HasProblemFor(result, temp.Path() / "Garbage.zar"));
    CHECK(HasProblemFor(result, temp.Path() / "Two.zar"));
}

TEST(ZarMissingEbootIsListedAsMissing) {
    fixture::TempDir temp;
    auto tree = fixture::Game("PPSA00007", "No Eboot");
    tree.erase("eboot.bin");
    fixture::WriteZar(temp.Path() / "NoEboot.zar", tree);
    const auto result = Scan({.roots = {temp.Path()}});
    const auto* game = Find(result, "PPSA00007");
    CHECK(game != nullptr);
    CHECK(game->executable.kind == ExecutableKind::Missing);
}

TEST(GroupsDuplicatesByTitleId) {
    fixture::TempDir temp;
    fixture::WriteFolder(temp.Path() / "Folder", fixture::Game("PPSA00008", "Same"));
    fixture::WriteZar(temp.Path() / "Same.zar", fixture::Game("PPSA00008", "Same"));
    fixture::WriteFolder(temp.Path() / "Other", fixture::Game("PPSA00009", "Other"));
    const auto result = Scan({.roots = {temp.Path()}});
    const auto groups = launcher::GroupByTitleId(result.games);
    CHECK_EQ(groups.size(), std::size_t{2});
    CHECK_EQ(groups.at("PPSA00008").size(), std::size_t{2});
}

TEST(SkipsUnfinishedConversions) {
    fixture::TempDir temp;
    fixture::WriteFolder(temp.Path() / "Game.partial", fixture::Game("PPSA00011", "Half Extracted"));
    fixture::WriteFolder(temp.Path(), {{"Other.zar.partial", fixture::Bytes("half packed")}});
    const auto result = Scan({.roots = {temp.Path()}});
    CHECK(result.games.empty());
    CHECK(result.problems.empty());
}

TEST(NotesAnUnwrappedEboot) {
    fixture::TempDir temp;
    auto tree = fixture::Game("PPSA00012", "Unwrapped");
    tree["eboot.bin"] = fixture::Self();
    tree["eboot.elf"] = fixture::Elf64();
    fixture::WriteFolder(temp.Path() / "U", tree);
    const auto result = Scan({.roots = {temp.Path()}});
    CHECK_EQ(result.games.size(), std::size_t{1});
    CHECK(result.games.front().executable.kind == ExecutableKind::Self);
    CHECK(result.games.front().unwrapped.has_value());
    CHECK(result.games.front().unwrapped->kind == ExecutableKind::Elf);
}

TEST(IconLargerThanLimitIsSkipped) {
    fixture::TempDir temp;
    fixture::WriteFolder(temp.Path() / "A", fixture::Game("PPSA00010", "A"));
    const auto result = Scan({.roots = {temp.Path()}, .maxIconSize = 4});
    CHECK_EQ(result.games.size(), std::size_t{1});
    CHECK(result.games.front().icon.empty());
}
