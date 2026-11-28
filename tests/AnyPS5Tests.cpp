#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "Fixtures.hpp"
#include "Framework.hpp"
#include "UnwrapGolden.hpp"

#include "core/AnyPS5.hpp"
#include "core/Install.hpp"
#include "core/Launch.hpp"
#include "core/Encryption.hpp"
#include "core/Links.hpp"
#include "core/Session.hpp"
#include "core/Modules.hpp"
#include "core/Profiles.hpp"
#include "core/Unwrap.hpp"
#include "core/Utf8.hpp"
#include "core/ZipStore.hpp"

using namespace launcher;

namespace {

std::vector<std::uint8_t> FromHex(std::string_view hex) {
    std::vector<std::uint8_t> bytes(hex.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) bytes[index] = static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(index * 2, 2)), nullptr, 16));
    return bytes;
}

std::string ReadText(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool Same(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code error;
    return std::filesystem::equivalent(left, right, error);
}

AnyPS5Runtime FakeRuntime(const std::filesystem::path& root) {
    fixture::WriteFolder(root, {{"libs/libkernel.prx", fixture::Bytes("kernel")}, {"libs/libc.prx", fixture::Bytes("libc")},
                                {"compiler/libextra.dll", fixture::Bytes("extra")}});
    AnyPS5Runtime runtime;
    runtime.root = root;
    runtime.layout = "build";
    runtime.relinker = root / "relinker";
    runtime.libsFolder = root / "libs";
    runtime.libraries = {root / "libs" / "libc.prx", root / "libs" / "libkernel.prx", root / "compiler" / "libextra.dll"};
    return runtime;
}

// Stands in for the relinker: reads the modules the way it does and writes each
// <module>.guest.prx under <output folder>/app0/<module folder>.
struct FakeRelinker {
    int exitCode = 0;
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
    std::vector<std::string> modules;

    ProcessRunner Runner() {
        return [this](const std::vector<std::string>& args, const std::filesystem::path& cwd, const std::filesystem::path& output, JobControl&) {
            arguments = args;
            workingDirectory = cwd;
            const auto input = FromUtf8(args[args.size() - 2]);
            const auto executable = FromUtf8(args.back());
            std::vector<std::string> folders{"sce_module", "sce_modules", "prx"};
            for (std::size_t index = 0; index + 1 < args.size(); ++index) {
                if (args[index] == "--module-dir") folders.push_back(args[index + 1]);
            }
            modules.clear();
            std::vector<std::pair<std::string, std::string>> guests;
            for (const auto& folder : folders) {
                const auto directory = input.parent_path() / folder;
                if (!std::filesystem::is_directory(directory)) continue;
                for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                    const auto name = ToUtf8(entry.path().filename());
                    if (!entry.is_regular_file() || name.ends_with(".guest.prx")) continue;
                    const auto head = ReadText(entry.path()).substr(0, 4);
                    if (head == "\x7f" "ELF") {
                        modules.push_back(folder + "/" + name);
                        guests.emplace_back(folder, name);
                    }
                }
            }
            std::sort(modules.begin(), modules.end());
            std::ofstream(output) << "relinking\n" << (exitCode == 0 ? "Conversion complete\n" : "FAIL: missing symbol sceFooBar\nInput: x\n");
            if (exitCode == 0) {
                fixture::Tree tree{{ToUtf8(executable.filename()), fixture::Bytes("converted")}, {"relink.registry.json", fixture::Bytes("{}")}};
                for (const auto& [folder, name] : guests) tree["app0/" + folder + "/" + name + ".guest.prx"] = fixture::Elf64();
                fixture::WriteFolder(executable.parent_path(), tree);
            } else {
                std::ofstream(executable) << "half written";
            }
            return exitCode;
        };
    }
};

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

fixture::Tree SelfGame(const std::string& titleId) {
    auto tree = fixture::Game(titleId, "Installable");
    tree["eboot.bin"] = FromHex(golden::cases[0].selfHex);
    tree["sce_module/libGame.prx"] = fixture::Elf64();
    tree["sce_module/libSelf.sprx"] = FromHex(golden::cases[1].selfHex);
    tree["sce_module/readme.txt"] = fixture::Bytes("not a module");
    tree["Media/Modules/plugin.prx"] = FromHex(golden::cases[2].selfHex);
    return tree;
}

struct InstallFixture {
    fixture::TempDir temp;
    Layout layout;
    AnyPS5Runtime runtime;
    TitleEntry title;

    InstallFixture() {
        layout.userdata = temp.Path() / "userdata";
        layout.library = temp.Path() / "library";
        layout.profile = "Player";
        runtime = FakeRuntime(temp.Path() / "anyps5");
        title.relink = {"--module-dir", "Media/Modules"};
    }

    GameEntry Scan(const std::string& name = "games") {
        const auto result = launcher::Scan({.roots = {temp.Path() / name}});
        if (result.games.size() != 1) throw std::runtime_error("expected one game, found " + std::to_string(result.games.size()));
        return result.games.front();
    }

    // A SelfGame after Convert SELF to ELF.
    std::filesystem::path WriteConvertedGame(const std::string& titleId) {
        const auto folder = temp.Path() / "games" / "Game";
        fixture::WriteFolder(folder, SelfGame(titleId));
        JobControl control;
        const auto result = ConvertGameSelfFiles(folder, folder.parent_path() / "Game.originals.zip", ModuleFolders(ModuleDirectories(title)), {}, control);
        if (!result.refused.empty()) throw std::runtime_error(result.refused.front());
        return folder;
    }

    InstallManifest Install(const GameEntry& game, FakeRelinker& relinker) {
        JobControl control;
        return InstallGame({game, runtime, layout, &title, "abc123", true, false}, relinker.Runner(), control);
    }
};

}

TEST(ProfileNamesAndLayout) {
    CHECK(IsValidProfileName("Player"));
    CHECK(IsValidProfileName("Νίκος"));
    CHECK(IsValidProfileName("Player 2"));
    CHECK(!IsValidProfileName(""));
    CHECK(!IsValidProfileName("a/b"));
    CHECK(!IsValidProfileName("con"));
    CHECK(!IsValidProfileName("trailing."));
    CHECK(!IsValidProfileName(" lead"));
    Layout layout{"U", "L", "Player"};
    CHECK(layout.Saves("PPSA00001") == std::filesystem::path("U") / "profiles" / "Player" / "saves" / "PPSA00001");
    CHECK(layout.Trophies("PPSA00001") == std::filesystem::path("U") / "profiles" / "Player" / "_trophy" / "PPSA00001");
    layout.profile = "../x";
    CHECK_THROWS(layout.Profile());
}

TEST(ProfilesAreCreatedListedAndKeepTheirLanguage) {
    fixture::TempDir temp;
    Layout layout{temp.Path(), temp.Path() / "library", "Player"};
    CreateProfile(layout);
    layout.profile = "Δεύτερος";
    CreateProfile(layout);
    SaveProfile(layout, {"25"});
    CHECK_EQ(LoadProfile(layout).language, std::string("25"));
    CHECK_THROWS(SaveProfile(layout, {"31"}));
    CHECK_THROWS(SaveProfile(layout, {"english"}));
    SaveProfile(layout, {"AUTO"});
    const auto names = ListProfiles(temp.Path());
    CHECK_EQ(names.size(), std::size_t{2});
    CHECK_EQ(names[0], std::string("Player"));
    CHECK_EQ(ConsoleLanguages().size(), std::size_t{31});
    CHECK_EQ(std::string(ConsoleLanguages()[25].name), std::string("Greek"));
}

TEST(ProfilesAreRenamedWithTheirData) {
    fixture::TempDir temp;
    Layout layout{temp.Path(), temp.Path() / "library", "Player"};
    CreateProfile(layout);
    SaveProfile(layout, {"8"});
    std::filesystem::create_directories(layout.Saves("PPSA00001") / "_sd");
    layout.profile = "Other";
    CreateProfile(layout);
    layout.profile = "Player";
    CHECK_THROWS(RenameProfile(layout, "Other"));
    CHECK_THROWS(RenameProfile(layout, "bad/name"));
    RenameProfile(layout, "player");  // case only
    layout.profile = "player";
    CHECK(std::filesystem::is_directory(layout.Saves("PPSA00001") / "_sd"));
    RenameProfile(layout, "Leska");
    layout.profile = "Leska";
    CHECK(std::filesystem::is_directory(layout.Saves("PPSA00001") / "_sd"));
    CHECK_EQ(LoadProfile(layout).language, std::string("8"));
    const auto names = ListProfiles(temp.Path());
    CHECK_EQ(names.size(), std::size_t{2});
    CHECK_EQ(names[0], std::string("Leska"));
}

TEST(FindsAnAnyPS5BuildOrRelease) {
    fixture::TempDir temp;
    CHECK(!ProbeAnyPS5("").runtime);
    const auto clone = temp.Path() / "clone";
    const char* relinker = TargetIsWindows ? "build/core/relinker/relinker.exe" : "build/core/relinker/relinker";
    fixture::WriteFolder(clone, {{"CMakeLists.txt", {}}, {relinker, {}}});
    auto probe = ProbeAnyPS5(ToUtf8(clone));
    CHECK(!probe.runtime);
    CHECK(probe.problem.find("only the relinker") != std::string::npos);
    fixture::WriteFolder(clone, {{"build/core/libs/libs/libkernel.prx", {}}, {"build/core/libs/libs/libc.prx", {}}, {"tools/titles.json", fixture::Bytes("{}")}});
    if (!TargetIsWindows) {
        probe = ProbeAnyPS5(ToUtf8(clone));
        CHECK(probe.runtime.has_value());
        CHECK_EQ(probe.runtime->layout, std::string("build"));
        CHECK_EQ(probe.runtime->libraries.size(), std::size_t{2});
        CHECK(probe.runtime->titleCatalog.has_value());
    }
    const auto release = temp.Path() / "release";
    fixture::WriteFolder(release, {{TargetIsWindows ? "relinker-v0.1.6.exe" : "relinker-v0.1.6", {}}, {"libs/libkernel.prx", {}}});
    if (TargetIsWindows) fixture::WriteFolder(release, {{"libs/libgcc_s_seh-1.dll", {}}, {"libs/libstdc++-6.dll", {}}, {"libs/libwinpthread-1.dll", {}}});
    probe = ProbeAnyPS5(ToUtf8(release));
    CHECK(probe.runtime.has_value());
    CHECK_EQ(probe.runtime->layout, std::string("release"));
    CHECK_EQ(ToUtf8(probe.runtime->relinker.filename()), std::string(TargetIsWindows ? "relinker-v0.1.6.exe" : "relinker-v0.1.6"));
}

TEST(TitleCatalogGivesTitleRunsRelinkArguments) {
    fixture::TempDir temp;
    fixture::WriteFolder(temp.Path(), {{"titles.json", fixture::Bytes(R"({
        "PPSA28319": {"relink": ["--module-dir", "Media/Modules", "--module-dir", "Media/Plugins"]},
        "PPSA68091": {"relink": ["--registry"], "fixed_base": "0x400000", "executables": ["other.bin"], "env": {"APS5_X": "1"}, "seconds": 30}
    })")}});
    const auto catalog = LoadTitleCatalog(temp.Path() / "titles.json");
    CHECK_EQ(catalog.size(), std::size_t{2});
    const auto& unity = catalog.at("PPSA28319");
    CHECK(ModuleDirectories(unity) == (std::vector<std::string>{"Media/Modules", "Media/Plugins"}));
    CHECK(RelinkArguments(unity, true, false) == (std::vector<std::string>{"--windows", "--module-dir", "Media/Modules", "--module-dir", "Media/Plugins"}));
    CHECK(RelinkArguments(unity, true, true).back() == "--to-intel");
    const auto& ruffle = catalog.at("PPSA68091");
    CHECK(RelinkArguments(ruffle, true, false) ==
          (std::vector<std::string>{"--windows", "--registry", "--windows-fixed-base", "0x400000", "--executable", "other.bin"}));
    CHECK_EQ(ruffle.env.at("APS5_X"), std::string("1"));
    TitleEntry avx;
    avx.relink = {"--to-avx"};
    CHECK(RelinkArguments(avx, false, true) == (std::vector<std::string>{"--to-avx"}));
    fixture::WriteFolder(temp.Path(), {{"bad.json", fixture::Bytes(R"({"PPSA00001": {"relink": "--x"}})")}});
    CHECK_THROWS(LoadTitleCatalog(temp.Path() / "bad.json"));
}

TEST(DirectoryLinksAreReplacedButFilesNeverDeleted) {
    fixture::TempDir temp;
    const auto a = temp.Path() / "a";
    const auto b = temp.Path() / "b";
    const auto link = temp.Path() / "run" / "_sd";
    fixture::WriteFolder(a, {{"save.dat", fixture::Bytes("precious")}});
    EnsureDirectoryLink(link, a);
    CHECK(IsDirectoryLink(link));
    CHECK(Same(link, a));
    EnsureDirectoryLink(link, a);
    EnsureDirectoryLink(link, b);
    CHECK(Same(link, b));
    CHECK_EQ(ReadText(a / "save.dat"), std::string("precious"));
    RemoveDirectoryLink(link);
    std::filesystem::create_directories(link);
    EnsureDirectoryLink(link, a);
    CHECK(IsDirectoryLink(link));
    RemoveDirectoryLink(link);
    fixture::WriteFolder(link, {{"stray.dat", fixture::Bytes("x")}});
    CHECK_THROWS(EnsureDirectoryLink(link, a));
    CHECK_THROWS(RemoveDirectoryLink(link));
    CHECK_EQ(ReadText(link / "stray.dat"), std::string("x"));

    const auto tree = temp.Path() / "own";
    fixture::WriteFolder(tree, {{"x/y.txt", fixture::Bytes("y")}});
    EnsureDirectoryLink(tree / "x" / "link", a);
    RemoveOwnTree(tree);
    CHECK(!std::filesystem::exists(tree));
    CHECK_EQ(ReadText(a / "save.dat"), std::string("precious"));
}

// An x86-64 ELF with one load segment holding payload.
std::vector<std::uint8_t> ElfWithSegment(const std::vector<std::uint8_t>& payload) {
    auto file = fixture::Elf64();
    file.resize(64 + 56, 0);
    const auto put = [&](std::size_t offset, std::uint64_t value, int bytes) {
        for (int index = 0; index < bytes; ++index) file[offset + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(value >> (8 * index));
    };
    put(0x10, 0xFE10, 2);
    put(0x20, 64, 8);
    put(0x36, 56, 2);
    put(0x38, 1, 2);
    put(64, 1, 4);
    put(64 + 0x08, 64 + 56, 8);
    put(64 + 0x20, payload.size(), 8);
    file.insert(file.end(), payload.begin(), payload.end());
    return file;
}

std::vector<std::uint8_t> RandomBytes(std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    for (auto& byte : bytes) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return bytes;
}

bool HasAnyLink(const std::filesystem::path& root) {
    for (std::filesystem::recursive_directory_iterator it(root), end; it != end; ++it) {
        if (it->is_symlink()) return true;
        if (it->is_regular_file() && std::filesystem::hard_link_count(it->path()) > 1) return true;
    }
    return false;
}

bool Has(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

TEST(FolderInstallMovesTheGameIntoApp0AndRelinks) {
    InstallFixture fixture;
    const auto home = fixture.WriteConvertedGame("PPSA00100");
    const auto game = fixture.Scan();
    CHECK(game.home.empty());
    FakeRelinker relinker;
    const auto manifest = fixture.Install(game, relinker);
    const auto app0 = home / "app0";

    CHECK(std::filesystem::is_regular_file(app0 / "eboot.bin"));
    CHECK(std::filesystem::is_regular_file(app0 / "sce_sys" / "param.json"));
    CHECK(std::filesystem::is_regular_file(app0 / "Media" / "data.bin"));
    CHECK(!std::filesystem::exists(home / "eboot.bin"));
    CHECK(FromUtf8(relinker.arguments[relinker.arguments.size() - 2]) == app0 / "eboot.bin");
    CHECK(FromUtf8(relinker.arguments.back()) == ExecutableIn(home, "PPSA00100"));
    CHECK(relinker.workingDirectory == home);
    CHECK_EQ(relinker.arguments[1], std::string("--windows"));
    const auto rpath = std::find(relinker.arguments.begin(), relinker.arguments.end(), "--rpath");
    CHECK(rpath != relinker.arguments.end() && FromUtf8(*std::next(rpath)) == std::filesystem::absolute(fixture.runtime.libsFolder).lexically_normal());
    CHECK(relinker.modules == (std::vector<std::string>{"Media/Modules/plugin.prx", "sce_module/libGame.prx", "sce_module/libSelf.sprx"}));
    CHECK_EQ(manifest.modules, 3);
    CHECK(manifest.complete);
    CHECK(manifest.addedLibraries == std::vector<std::string>{"libextra.dll"});
    CHECK(std::filesystem::is_regular_file(fixture.runtime.libsFolder / "libextra.dll"));
    CHECK(Has(manifest.outputs, ToUtf8(ExecutableIn(home, "PPSA00100").filename())) && Has(manifest.outputs, "relink.registry.json"));
    CHECK(std::filesystem::is_regular_file(app0 / "sce_module" / "libGame.prx.guest.prx"));
    CHECK(std::filesystem::is_regular_file(app0 / "Media" / "Modules" / "plugin.prx.guest.prx"));
    CHECK(!HasAnyLink(fixture.temp.Path()));

    const auto installed = fixture.Scan();
    CHECK(installed.home == home);
    CHECK(installed.location == app0);
    CHECK(installed.format == PackageFormat::Folder);
    CHECK_EQ(installed.modules.elf, 3);  // sce_module and the Media/Modules the scan found; .guest.prx and .self aren't counted
    CHECK(installed.moduleDirs == (std::vector<std::string>{"Media/Modules"}));
    CHECK_EQ(installed.modules.self, 0);
    CHECK(ReadInstallManifest(home).has_value());
    CHECK(ReinstallReason(installed, *ReadInstallManifest(home), "abc123").empty());
    CHECK(!ReinstallReason(installed, *ReadInstallManifest(home), "def456").empty());

    const auto again = fixture.Install(installed, relinker);
    CHECK(again.complete);
    CHECK(std::filesystem::is_regular_file(app0 / "eboot.bin"));
    CHECK(!std::filesystem::exists(app0 / "app0"));
}

TEST(AnApp0FolderAloneInItsParentIsUsedWhereItIs) {
    InstallFixture fixture;
    const auto parent = fixture.temp.Path() / "games" / "PPSA00130-dump";
    const auto app0 = parent / "app0";
    auto tree = fixture::Game("PPSA00130", "In app0");
    tree["sce_module/libGame.prx"] = fixture::Elf64();
    fixture::WriteFolder(app0, tree);
    fixture::WriteFolder(parent, {{"desktop.ini", fixture::Bytes("[.ShellClassInfo]")}});
    const auto game = fixture.Scan();
    CHECK(game.location == app0);
    CHECK(game.app0Parent == parent);
    CHECK(HomeFor(game) == parent);
    CHECK(OriginalsZipFor(game, fixture.temp.Path() / "library") == fixture.temp.Path() / "library" / "originals" / "PPSA00130-01.000.000.originals.zip");

    FakeRelinker relinker;
    const auto manifest = fixture.Install(game, relinker);
    CHECK(manifest.keptApp0);
    CHECK(FromUtf8(relinker.arguments.back()) == ExecutableIn(parent, "PPSA00130"));
    CHECK(std::filesystem::is_regular_file(app0 / "eboot.bin"));
    CHECK(!std::filesystem::exists(app0 / "app0"));
    const auto installed = fixture.Scan();
    CHECK(installed.home == parent);
    CHECK(installed.location == app0);

    const auto removed = RemoveInstall(installed);
    CHECK(removed.restored == app0);
    CHECK(std::filesystem::is_regular_file(app0 / "eboot.bin"));
    CHECK(!std::filesystem::exists(ExecutableIn(parent, "PPSA00130")));
    CHECK(!std::filesystem::exists(parent / InstallManifestName));
    CHECK(!std::filesystem::exists(app0 / "sce_module" / "libGame.prx.guest.prx"));
    CHECK(fixture.Scan().app0Parent == parent);

    // With anything else beside app0, the parent is left alone and app0 is the game's folder.
    fixture::WriteFolder(parent, {{"notes.txt", fixture::Bytes("mine")}});
    const auto crowded = fixture.Scan();
    CHECK(crowded.app0Parent.empty());
    CHECK(HomeFor(crowded) == app0);

    // An app0 directly in the games folder never makes the games folder a game's folder.
    InstallFixture top;
    fixture::WriteFolder(top.temp.Path() / "games" / "app0", fixture::Game("PPSA00131", "Top"));
    CHECK(top.Scan().app0Parent.empty());
}

TEST(FolderInstallRefusesSelfFilesWithoutMovingAnything) {
    InstallFixture fixture;
    const auto home = fixture.temp.Path() / "games" / "Game";
    fixture::WriteFolder(home, SelfGame("PPSA00102"));
    FakeRelinker relinker;
    std::string message;
    try {
        fixture.Install(fixture.Scan(), relinker);
    } catch (const std::exception& failure) {
        message = failure.what();
    }
    CHECK(message.find("eboot.bin, sce_module/libSelf.sprx, Media/Modules/plugin.prx") != std::string::npos);
    CHECK(message.find("Convert SELF to ELF") != std::string::npos);
    CHECK(relinker.arguments.empty());
    CHECK(std::filesystem::is_regular_file(home / "eboot.bin"));
    CHECK(!std::filesystem::exists(home / "app0"));
    CHECK(!std::filesystem::exists(home / InstallManifestName));
}

TEST(ZarInstallMovesTheArchiveAndExtractsOnlyWhatIsNeeded) {
    InstallFixture fixture;
    const auto archive = fixture.temp.Path() / "games" / "Game.zar";
    fixture::WriteZar(archive, SelfGame("PPSA00103"));
    const auto original = ReadBytes(archive);
    const auto game = fixture.Scan();
    FakeRelinker relinker;
    const auto manifest = fixture.Install(game, relinker);
    const auto home = fixture.temp.Path() / "games" / "Game";
    const auto app0 = home / "app0";

    CHECK(!std::filesystem::exists(archive));
    CHECK(ReadBytes(home / "app0.zar") == original);
    CHECK(launcher::ModuleKindOf(ReadBytes(app0 / "eboot.elf")) == launcher::ModuleKind::Elf);
    CHECK(!std::filesystem::exists(app0 / "eboot.bin"));
    CHECK(std::filesystem::is_regular_file(app0 / "sce_sys" / "param.json"));
    CHECK(std::filesystem::is_regular_file(app0 / "sce_sys" / "icon0.png"));
    CHECK(launcher::ModuleKindOf(ReadBytes(app0 / "sce_module" / "libSelf.sprx")) == launcher::ModuleKind::Elf);
    CHECK(launcher::ModuleKindOf(ReadBytes(app0 / "Media" / "Modules" / "plugin.prx")) == launcher::ModuleKind::Elf);
    CHECK(!std::filesystem::exists(app0 / "sce_module" / "readme.txt"));
    CHECK(!std::filesystem::exists(app0 / "Media" / "data.bin"));
    CHECK_EQ(manifest.modules, 3);
    CHECK_EQ(manifest.unwrapped, 3);
    CHECK_EQ(manifest.originalName, std::string("Game.zar"));
    CHECK(FromUtf8(relinker.arguments[relinker.arguments.size() - 2]) == app0 / "eboot.elf");
    CHECK(std::filesystem::is_regular_file(app0 / "sce_module" / "libSelf.sprx.guest.prx"));
    CHECK(!HasAnyLink(fixture.temp.Path()));

    const auto installed = fixture.Scan();
    CHECK(installed.format == PackageFormat::Zar);
    CHECK(installed.home == home);
    CHECK(installed.location == home / "app0.zar");
    CHECK(ReadInstallManifest(home).has_value());
    CHECK(fixture.Install(installed, relinker).complete);
    CHECK(ReadBytes(home / "app0.zar") == original);

    fixture::WriteZar(fixture.temp.Path() / "nested" / "Nested.zar", fixture::Prefixed(SelfGame("PPSA00104"), "Inner/"));
    CHECK_THROWS(fixture.Install(fixture.Scan("nested"), relinker));
    CHECK(std::filesystem::exists(fixture.temp.Path() / "nested" / "Nested.zar"));
}

TEST(ZarInstallFailureLeavesTheArchiveWhereItWas) {
    InstallFixture fixture;
    auto tree = SelfGame("PPSA00106");
    tree["sce_module/libLocked.sprx"] = FromHex(golden::encrypted_load_hex);
    const auto archive = fixture.temp.Path() / "games" / "Locked.zar";
    fixture::WriteZar(archive, tree);
    FakeRelinker relinker;
    std::string message;
    try {
        fixture.Install(fixture.Scan(), relinker);
    } catch (const std::exception& failure) {
        message = failure.what();
    }
    CHECK(message.find("sce_module/libLocked.sprx") != std::string::npos);
    CHECK(message.find("looks encrypted") != std::string::npos);
    CHECK(std::filesystem::is_regular_file(archive));
    CHECK(!std::filesystem::exists(fixture.temp.Path() / "games" / "Locked"));
}

TEST(AFailedFirstInstallPutsTheFolderBack) {
    InstallFixture fixture;
    const auto home = fixture.WriteConvertedGame("PPSA00101");
    FakeRelinker relinker;
    relinker.exitCode = 2;
    std::string message;
    try {
        fixture.Install(fixture.Scan(), relinker);
    } catch (const std::exception& failure) {
        message = failure.what();
    }
    CHECK(message.find("missing symbol sceFooBar") != std::string::npos);
    // As before the install: the files out of app0, nothing of the launcher's left.
    CHECK(std::filesystem::is_regular_file(home / "eboot.bin"));
    CHECK(!std::filesystem::exists(home / "app0"));
    CHECK(!std::filesystem::exists(home / InstallManifestName));
    CHECK(!std::filesystem::exists(ExecutableIn(home, "PPSA00101")));
    const auto game = fixture.Scan();
    CHECK(game.home.empty());
    relinker.exitCode = 0;
    CHECK(fixture.Install(game, relinker).complete);
}

TEST(AFailedFirstZarInstallPutsTheArchiveBack) {
    InstallFixture fixture;
    const auto archive = fixture.temp.Path() / "games" / "Game.zar";
    fixture::WriteZar(archive, SelfGame("PPSA00108"));
    const auto original = ReadBytes(archive);
    FakeRelinker relinker;
    relinker.exitCode = 2;
    CHECK_THROWS(fixture.Install(fixture.Scan(), relinker));
    CHECK(ReadBytes(archive) == original);
    CHECK(!std::filesystem::exists(fixture.temp.Path() / "games" / "Game"));
    relinker.exitCode = 0;
    CHECK(fixture.Install(fixture.Scan(), relinker).complete);
}

TEST(AFailedReinstallStaysSetUpAndCanBeRemoved) {
    InstallFixture fixture;
    const auto archive = fixture.temp.Path() / "games" / "Game.zar";
    fixture::WriteZar(archive, SelfGame("PPSA00109"));
    const auto original = ReadBytes(archive);
    const auto home = fixture.temp.Path() / "games" / "Game";
    FakeRelinker relinker;
    fixture.Install(fixture.Scan(), relinker);
    relinker.exitCode = 2;
    CHECK_THROWS(fixture.Install(fixture.Scan(), relinker));
    CHECK(!ReadInstallManifest(home).has_value());
    CHECK(ReadHomeManifest(home).has_value());
    const auto game = fixture.Scan();
    CHECK(game.home == home);
    RemoveInstall(game);
    CHECK(ReadBytes(archive) == original);
    CHECK(!std::filesystem::exists(home));
}

TEST(RunFilesDoNotBlockRemovingAnInstall) {
    // A run's anyps5-recipe.json beside the exe, and one an older set-up moved into app0.
    InstallFixture fixture;
    const auto home = fixture.WriteConvertedGame("PPSA00107");
    FakeRelinker relinker;
    fixture.Install(fixture.Scan(), relinker);
    fixture::WriteFolder(home, {{"anyps5-recipe.json", fixture::Bytes("{}")}, {"anyps5-report.json", fixture::Bytes("{}")}});
    fixture::WriteFolder(home / "app0", {{"anyps5-recipe.json", fixture::Bytes("{}")}});
    const auto removed = RemoveInstall(fixture.Scan());
    CHECK(removed.restored == home);
    CHECK(removed.leftovers.empty());
    CHECK(!std::filesystem::exists(home / "anyps5-recipe.json"));
    CHECK(!std::filesystem::exists(home / "anyps5-report.json"));
    CHECK(!std::filesystem::exists(home / "app0"));

    // Set up again with a run file still beside the game: it isn't moved into app0.
    fixture::WriteFolder(home, {{"anyps5-recipe.json", fixture::Bytes("{}")}});
    fixture.Install(fixture.Scan(), relinker);
    CHECK(!std::filesystem::exists(home / "app0" / "anyps5-recipe.json"));
    CHECK(!std::filesystem::exists(home / "anyps5-recipe.json"));
}

TEST(ARunsTemp0DoesNotBlockRemovingAnInstall) {
    // The game's files came with a temp0 (from running them before they were set up), and
    // a run of the installed game made another beside app0: that one gives way.
    InstallFixture fixture;
    const auto home = fixture.WriteConvertedGame("PPSA00111");
    fixture::WriteFolder(home, {{"temp0/old.bin", fixture::Bytes("before")}});
    FakeRelinker relinker;
    fixture.Install(fixture.Scan(), relinker);
    CHECK(std::filesystem::exists(home / "app0" / "temp0" / "old.bin"));
    fixture::WriteFolder(home, {{"temp0/new.bin", fixture::Bytes("run")}});
    const auto removed = RemoveInstall(fixture.Scan());
    CHECK(removed.leftovers.empty());
    CHECK(std::filesystem::exists(home / "temp0" / "old.bin"));
    CHECK(!std::filesystem::exists(home / "temp0" / "new.bin"));
    CHECK(!std::filesystem::exists(home / "app0"));
}

TEST(OtherFilesInTheWayStopRemovingAnInstall) {
    InstallFixture fixture;
    const auto home = fixture.WriteConvertedGame("PPSA00112");
    FakeRelinker relinker;
    fixture.Install(fixture.Scan(), relinker);
    fixture::WriteFolder(home, {{"sce_sys/extra.bin", fixture::Bytes("mine")}});
    CHECK_THROWS(RemoveInstall(fixture.Scan()));
    CHECK(std::filesystem::exists(home / "app0" / "eboot.bin"));  // nothing moved
}

TEST(RemoveInstallPutsEverythingBack) {
    InstallFixture fixture;
    const auto home = fixture.WriteConvertedGame("PPSA00107");
    FakeRelinker relinker;
    fixture.Install(fixture.Scan(), relinker);
    fixture::WriteFolder(home, {{"notes.txt", fixture::Bytes("mine")}});
    const auto removed = RemoveInstall(fixture.Scan());
    CHECK(removed.restored == home);
    CHECK(removed.leftovers == std::vector<std::string>{"notes.txt"});
    CHECK(launcher::ModuleKindOf(ReadBytes(home / "eboot.bin")) == launcher::ModuleKind::Elf);
    CHECK(std::filesystem::is_regular_file(home.parent_path() / "Game.originals.zip"));
    CHECK(!std::filesystem::exists(home / "sce_module" / "libGame.prx.guest.prx"));
    CHECK(!std::filesystem::exists(home / "app0"));
    CHECK(!std::filesystem::exists(ExecutableIn(home, "PPSA00107")));
    CHECK(!std::filesystem::exists(home / "relink.registry.json"));
    CHECK(!std::filesystem::exists(home / InstallManifestName));
    CHECK(fixture.Scan().home.empty());

    InstallFixture zar;
    const auto archive = zar.temp.Path() / "games" / "Game.zar";
    fixture::WriteZar(archive, SelfGame("PPSA00108"));
    const auto original = ReadBytes(archive);
    zar.Install(zar.Scan(), relinker);
    const auto restored = RemoveInstall(zar.Scan());
    CHECK(restored.restored == archive);
    CHECK(ReadBytes(archive) == original);
    CHECK(!std::filesystem::exists(zar.temp.Path() / "games" / "Game"));

    // The game made devlog and temp0 beside the exe: removal leaves the folder for them,
    // and installing again uses that folder instead of stopping on it.
    zar.Install(zar.Scan(), relinker);
    const auto zarHome = zar.temp.Path() / "games" / "Game";
    fixture::WriteFolder(zarHome, {{"devlog/run.txt", fixture::Bytes("log")}, {"temp0/cache.bin", fixture::Bytes("tmp")}});
    const auto kept = RemoveInstall(zar.Scan());
    CHECK(kept.leftovers == (std::vector<std::string>{"devlog", "temp0"}));
    CHECK(ReadBytes(archive) == original);
    const auto again = zar.Scan();
    CHECK(launcher::LeftoverHome(again));
    CHECK(zar.Install(again, relinker).complete);
    CHECK(ReadBytes(zarHome / "app0.zar") == original);
    CHECK_EQ(ReadText(zarHome / "devlog" / "run.txt"), std::string("log"));
    CHECK(zar.Scan().home == zarHome);

    // A folder holding something an install makes is still in the way.
    const auto removedAgain = RemoveInstall(zar.Scan());
    CHECK(removedAgain.restored == archive);
    fixture::WriteFolder(zarHome, {{"app0/stray.txt", fixture::Bytes("x")}});
    CHECK(!launcher::LeftoverHome(zar.Scan()));
    CHECK_THROWS(zar.Install(zar.Scan(), relinker));
    CHECK(std::filesystem::is_regular_file(archive));
}

TEST(PlayMovesTheProfilesFoldersInAndBack) {
    InstallFixture fixture;
    const auto home = fixture.WriteConvertedGame("PPSA00105");
    FakeRelinker relinker;
    fixture.Install(fixture.Scan(), relinker);
    const auto game = fixture.Scan();
    fixture.title.env = {{"APS5_TITLE_SETTING", "7"}};
    const auto layout = fixture.layout;
    std::filesystem::create_directories(layout.Saves("PPSA00105") / "_sd");
    std::ofstream(layout.Saves("PPSA00105") / "_sd" / "slot.dat") << "save";

    const auto plan = PrepareRun(game, layout, {"25"}, &fixture.title);
    CHECK(plan.workingDirectory == home);
    CHECK(plan.executable == ExecutableIn(home, "PPSA00105"));
    CHECK_EQ(ReadText(home / "_sd" / "slot.dat"), std::string("save"));
    CHECK(!std::filesystem::exists(layout.Saves("PPSA00105") / "_sd"));
    for (const char* name : {"_sd_mem", "_trophy", "data", "download0"}) CHECK(std::filesystem::is_directory(home / name));
    CHECK(HasSession(home));
    CHECK(!HasAnyLink(fixture.temp.Path()));
    const auto env = [&](const std::string& name) {
        for (const auto& [key, value] : plan.environment) {
            if (key == name) return value;
        }
        return std::string("(unset)");
    };
    CHECK_EQ(env("ANYPS5_INPUT_CONFIG"), ToUtf8(layout.InputConfig()));
    CHECK_EQ(env("ANYPS5_LANGUAGE"), std::string("25"));
    CHECK_EQ(env("ANYPS5_ENTITLEMENTS"), std::string("(unset)"));
    CHECK_EQ(env("APS5_TITLE_SETTING"), std::string("7"));
    CHECK(std::filesystem::is_regular_file(layout.InputConfig()));
    CHECK_THROWS(PrepareRun(game, layout, {}, nullptr));  // already running

    std::ofstream(home / "_sd" / "new.dat") << "progress";
    std::ofstream(home / "_trophy" / "268435456_0.txt", std::ios::binary) << "1 2\n";
    FinishRun(plan);
    CHECK(!HasSession(home));
    for (const char* name : {"_sd", "_sd_mem", "_trophy", "data", "download0"}) CHECK(!std::filesystem::exists(home / name));
    CHECK_EQ(ReadText(layout.Saves("PPSA00105") / "_sd" / "new.dat"), std::string("progress"));
    CHECK_EQ(ReadText(layout.Trophies("PPSA00105") / "268435456_0.txt"), std::string("1 2\n"));

    // Another profile gets its own folders.
    fixture.layout.profile = "Second";
    const auto second = PrepareRun(game, fixture.layout, {}, nullptr);
    CHECK(!std::filesystem::exists(home / "_sd" / "slot.dat"));
    // The launcher stopped during the game: the next start moves them back.
    CHECK(MoveOutSession(home).size() == 5);
    CHECK(std::filesystem::is_directory(fixture.layout.Saves("PPSA00105") / "_sd"));
    CHECK_EQ(ReadText(layout.Saves("PPSA00105") / "_sd" / "slot.dat"), std::string("save"));

    // A move back to another drive copied a folder but couldn't delete it beside the
    // exe (the crashed game held a file): the next try only deletes that copy.
    const auto again = PrepareRun(game, layout, {}, nullptr);
    std::ofstream(home / "_sd_mem" / "mem.dat") << "memory";
    std::filesystem::copy(home / "_sd_mem", layout.Saves("PPSA00105") / "_sd_mem", std::filesystem::copy_options::recursive);
    CHECK(MoveOutSession(home).size() == 5);
    CHECK(!HasSession(home));
    CHECK(!std::filesystem::exists(home / "_sd_mem"));
    CHECK_EQ(ReadText(layout.Saves("PPSA00105") / "_sd_mem" / "mem.dat"), std::string("memory"));
    // A different folder there still stops it, and nothing is lost.
    PrepareRun(game, layout, {}, nullptr);
    std::ofstream(home / "_sd_mem" / "mem.dat") << "newer";
    std::filesystem::create_directories(layout.Saves("PPSA00105") / "_sd_mem");
    std::ofstream(layout.Saves("PPSA00105") / "_sd_mem" / "mem.dat") << "older";
    CHECK_THROWS(MoveOutSession(home));
    CHECK(HasSession(home));
    CHECK_EQ(ReadText(home / "_sd_mem" / "mem.dat"), std::string("newer"));
    std::filesystem::remove_all(layout.Saves("PPSA00105") / "_sd_mem");
    CHECK(MoveOutSession(home).size() == 1);
    CHECK_EQ(ReadText(layout.Saves("PPSA00105") / "_sd_mem" / "mem.dat"), std::string("newer"));
    (void)again;

    // A folder with files beside the exe that no profile put there is never touched.
    fixture::WriteFolder(home / "_trophy", {{"stray.txt", fixture::Bytes("x")}});
    CHECK_THROWS(PrepareRun(game, layout, {}, nullptr));
    CHECK(!HasSession(home));
    CHECK(std::filesystem::is_directory(layout.Saves("PPSA00105") / "_sd"));
    CHECK_EQ(ReadText(home / "_trophy" / "stray.txt"), std::string("x"));
}

TEST(EncryptedElfIsSpottedInTheListAndRefusedByInstall) {
    const auto encrypted = ElfWithSegment(RandomBytes(0x30000));
    const auto plain = ElfWithSegment(std::vector<std::uint8_t>(0x30000, 0x90));
    CHECK(ElfEncryption(encrypted).has_value());
    CHECK(ElfEncryption(encrypted)->starts_with("every load segment looks encrypted: segment 0 at 0x78"));
    CHECK(!ElfEncryption(plain).has_value());
    CHECK(!ElfEncryption(fixture::Elf64()).has_value());

    InstallFixture fixture;
    const auto home = fixture.temp.Path() / "games" / "Game";
    auto tree = fixture::Game("PPSA00120", "Encrypted");
    tree["eboot.bin"] = encrypted;
    fixture::WriteFolder(home, tree);
    const auto game = fixture.Scan();
    CHECK(game.executable.kind == ExecutableKind::ElfEncrypted);
    CHECK(game.executable.detail.find("looks encrypted") != std::string::npos);
    FakeRelinker relinker;
    std::string message;
    try {
        fixture.Install(game, relinker);
    } catch (const std::exception& failure) {
        message = failure.what();
    }
    CHECK(message.find("still encrypted: eboot.bin (every load segment looks encrypted") != std::string::npos);
    CHECK(relinker.arguments.empty());
    CHECK(std::filesystem::is_regular_file(home / "eboot.bin"));
    CHECK(!std::filesystem::exists(home / "app0"));

    // A module that is still encrypted stops a folder install and a .zar install.
    tree["eboot.bin"] = plain;
    tree["sce_module/libLocked.prx"] = encrypted;
    fixture::WriteFolder(home, tree);
    CHECK(fixture.Scan().executable.kind == ExecutableKind::Elf);
    message.clear();
    try {
        fixture.Install(fixture.Scan(), relinker);
    } catch (const std::exception& failure) {
        message = failure.what();
    }
    CHECK(message.find("sce_module/libLocked.prx") != std::string::npos);
    CHECK(!std::filesystem::exists(home / "app0"));

    InstallFixture zar;
    const auto archive = zar.temp.Path() / "games" / "Locked.zar";
    fixture::WriteZar(archive, tree);
    message.clear();
    try {
        zar.Install(zar.Scan(), relinker);
    } catch (const std::exception& failure) {
        message = failure.what();
    }
    CHECK(message.find("still encrypted: sce_module/libLocked.prx") != std::string::npos);
    CHECK(std::filesystem::is_regular_file(archive));
    CHECK(!std::filesystem::exists(zar.temp.Path() / "games" / "Locked"));
}

TEST(RunReportsAreMovedBesideTheLogAndSummarised) {
    fixture::TempDir temp;
    RunPlan plan;
    plan.reportSource = temp.Path() / "install" / "anyps5-report.json";
    plan.reportTarget = temp.Path() / "logs" / "2026-11-23_20-00-00.report.json";
    std::filesystem::create_directories(temp.Path() / "logs");
    fixture::WriteFolder(temp.Path() / "install", {{"anyps5-report.json", fixture::Bytes(R"({"run": {"exit": "crash", "stage": "first-draw", "seconds": 12.4},
        "stop": {"kind": "missing-function", "message": "sceFooBar is not implemented"}})")}});
    fixture::WriteFolder(temp.Path() / "install", {{"anyps5-recipe.json", fixture::Bytes("{}")}});
    const auto report = CollectRunReport(plan);
    CHECK(!std::filesystem::exists(temp.Path() / "install" / "anyps5-recipe.json"));
    CHECK(std::filesystem::exists(temp.Path() / "logs" / "2026-11-23_20-00-00.recipe.json"));
    CHECK(report.has_value());
    CHECK_EQ(report->summary, std::string("crash after 12 s, reached first-draw (missing-function: sceFooBar is not implemented)"));
    CHECK(!std::filesystem::exists(plan.reportSource));
    CHECK(std::filesystem::exists(plan.reportTarget));
    Layout layout{temp.Path(), temp.Path() / "library", "Player"};
    std::filesystem::create_directories(layout.Logs("PPSA00107"));
    std::filesystem::copy_file(plan.reportTarget, layout.Logs("PPSA00107") / "2026-11-23_20-00-00.report.json");
    fixture::WriteFolder(layout.Logs("PPSA00107"), {{"2026-11-24_08-00-00.report.json", fixture::Bytes(R"({"run": {"exit": "clean", "stage": "presents", "seconds": 600}, "stop": {"kind": "none"}})")}});
    const auto latest = LatestRunReport(layout, "PPSA00107");
    CHECK(latest.has_value());
    CHECK_EQ(latest->summary, std::string("clean after 600 s"));
    CHECK(!LatestRunReport(layout, "PPSA00108").has_value());
}

std::string Md5Hex(const std::vector<std::uint8_t>& bytes) {
    Md5 md5;
    md5.Update(bytes);
    return md5.HexDigest();
}

TEST(Md5MatchesTheReferenceValues) {
    CHECK_EQ(Md5Hex({}), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(Md5Hex(fixture::Bytes("abc")), std::string("900150983cd24fb0d6963f7d28e17f72"));
    CHECK_EQ(Md5Hex(fixture::Bytes("The quick brown fox jumps over the lazy dog")), std::string("9e107d9d372bb6826bd81d3542a419d6"));
    // Fed in uneven pieces, the result is the same.
    const auto data = RandomBytes(100000);
    Md5 pieces;
    for (std::size_t at = 0; at < data.size(); at += 777) pieces.Update(std::span(data).subspan(at, std::min<std::size_t>(777, data.size() - at)));
    CHECK_EQ(pieces.HexDigest(), Md5Hex(data));
}

TEST(ModuleFoldersAreFoundAndRelinkedWithoutATitleEntry) {
    InstallFixture fixture;
    const auto folder = fixture.WriteConvertedGame("PPSA00120");
    fixture.title = {};  // no tools/titles.json entry for this game
    fixture::WriteFolder(folder, {{"Media/Plugins/unity.sprx", fixture::Elf64()},
                                  {"Media/Plugins/readme.prx", fixture::Bytes("not a module")},
                                  {"Media/Data/level.bin", fixture::Bytes("data")},
                                  {"Engine/Bin/engine.prx", fixture::Elf64()},     // found when installing, not by the quick scan
                                  {"a/b/c/d/e/deep.prx", fixture::Elf64()}});  // deeper than anything looks
    const auto game = fixture.Scan();
    CHECK(game.moduleDirs == (std::vector<std::string>{"Media/Modules", "Media/Plugins"}));
    {
        FolderPackage package(game.location);
        CHECK(FindModuleDirectories(package) == (std::vector<std::string>{"Engine/Bin", "Media/Modules", "Media/Plugins"}));
    }

    FakeRelinker relinker;
    const auto manifest = fixture.Install(game, relinker);
    const auto& args = relinker.arguments;
    const auto has = [&](const std::string& folderName) {
        for (std::size_t index = 0; index + 1 < args.size(); ++index) {
            if (args[index] == "--module-dir" && args[index + 1] == folderName) return true;
        }
        return false;
    };
    CHECK(has("Media/Modules"));
    CHECK(has("Media/Plugins"));
    CHECK(has("Engine/Bin"));
    CHECK(!has("a/b/c/d/e"));
    CHECK(std::find(args.begin(), args.end(), "--skip-sce-module") == args.end());
    CHECK(std::filesystem::is_regular_file(folder / "app0" / "Media" / "Plugins" / "unity.sprx.guest.prx"));
    const auto installed = fixture.Scan();
    CHECK(ReinstallReason(installed, manifest, "abc123").empty());

    // An install from before the scan found these folders asks to be done again.
    auto older = manifest;
    older.arguments = {"--windows", "--rpath", "x"};
    CHECK_EQ(ReinstallReason(installed, older, "abc123"), std::string("the modules in Media/Modules weren't converted"));
}

TEST(ConvertSelfZipsTheOriginalsThenConvertsInPlace) {
    InstallFixture fixture;
    const auto folder = fixture.temp.Path() / "games" / "Game";
    const auto zip = fixture.temp.Path() / "games" / "Game.originals.zip";
    auto tree = SelfGame("PPSA00110");
    tree["sce_module/libLocked.sprx"] = FromHex(golden::encrypted_load_hex);
    fixture::WriteFolder(folder, tree);
    fixture::WriteFolder(folder, {{"eboot.elf", fixture::Bytes("left by the old converter")}});

    const auto before = fixture.Scan();
    CHECK_EQ(before.modules.elf, 1);
    CHECK_EQ(before.modules.self, 3);  // eboot aside: libSelf, libLocked and Media/Modules/plugin
    {
        // Only the library's originals folder counts; a zip beside the game is ignored.
        const auto library = fixture.temp.Path() / "library";
        fixture::WriteFolder(fixture.temp.Path() / "games", {{"PPSA00110-01.000.000.originals.zip", fixture::Bytes("old")}});
        CHECK(OriginalsZipFor(before, library) == library / "originals" / "PPSA00110-01.000.000.originals.zip");
        std::filesystem::remove(fixture.temp.Path() / "games" / "PPSA00110-01.000.000.originals.zip");
    }

    JobControl control;
    const auto result = ConvertGameSelfFiles(folder, zip, ModuleFolders(ModuleDirectories(fixture.title)), {}, control);
    CHECK(result.eboot);
    CHECK_EQ(result.modules, 2);
    CHECK_EQ(result.refused.size(), std::size_t{1});
    CHECK(result.refused[0].starts_with("sce_module/libLocked.sprx: "));
    CHECK_EQ(control.Done(), control.Total());

    for (const auto* name : {"eboot.bin", "sce_module/libSelf.sprx", "Media/Modules/plugin.prx"}) {
        CHECK(launcher::ModuleKindOf(ReadBytes(folder / name)) == launcher::ModuleKind::Elf);
    }
    CHECK(ReadBytes(folder / "sce_module" / "libLocked.sprx") == FromHex(golden::encrypted_load_hex));
    CHECK(!std::filesystem::exists(folder / "eboot.elf"));
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
        const auto name = ToUtf8(entry.path().filename());
        CHECK(!name.ends_with(".self") && !name.ends_with(".tmp"));
    }

    {
        // Closed before the next conversion: Windows can't replace an open file.
        ZipReader reader(zip);
        std::vector<std::string> names;
        for (const auto& entry : reader.Entries()) names.push_back(entry.name);
        CHECK(names == (std::vector<std::string>{"eboot.bin", "sce_module/libSelf.sprx", "Media/Modules/plugin.prx", "MD5SUMS"}));
        CHECK(reader.ReadAll(*reader.Find("eboot.bin")) == FromHex(golden::cases[0].selfHex));
        const auto sums = reader.ReadAll(*reader.Find("MD5SUMS"));
        const std::string list(sums.begin(), sums.end());
        CHECK(list.find(Md5Hex(FromHex(golden::cases[1].selfHex)) + "  sce_module/libSelf.sprx\n") != std::string::npos);
        CHECK(list.find(Md5Hex(FromHex(golden::cases[0].selfHex)) + "  eboot.bin\n") != std::string::npos);
    }

    const auto after = fixture.Scan();
    CHECK(after.executable.kind == ExecutableKind::Elf);
    CHECK_EQ(after.modules.elf, 3);
    CHECK_EQ(after.modules.self, 1);

    // Converting again adds only what is new; the locked module is still refused.
    fixture::WriteFolder(folder, {{"sce_module/libLate.sprx", FromHex(golden::cases[1].selfHex)}});
    JobControl again;
    const auto second = ConvertGameSelfFiles(folder, zip, ModuleFolders(ModuleDirectories(fixture.title)), {}, again);
    CHECK(!second.eboot);
    CHECK_EQ(second.modules, 1);
    {
        ZipReader grown(zip);
        CHECK_EQ(grown.Entries().size(), std::size_t{5});
        CHECK(grown.ReadAll(*grown.Find("sce_module/libLate.sprx")) == FromHex(golden::cases[1].selfHex));
        CHECK(grown.ReadAll(*grown.Find("eboot.bin")) == FromHex(golden::cases[0].selfHex));
    }

    // Install refuses while one module is still SELF, then runs once it's gone.
    FakeRelinker relinker;
    CHECK_THROWS(fixture.Install(fixture.Scan(), relinker));
    std::filesystem::remove(folder / "sce_module" / "libLocked.sprx");
    const auto manifest = fixture.Install(fixture.Scan(), relinker);
    CHECK_EQ(manifest.modules, 4);
}

TEST(RestorePutsTheOriginalsBackOnlyWhenTheirMd5Matches) {
    fixture::TempDir temp;
    const auto folder = temp.Path() / "Game";
    const auto zip = temp.Path() / "Game.originals.zip";
    fixture::WriteFolder(folder, SelfGame("PPSA00111"));
    JobControl control;
    ConvertGameSelfFiles(folder, zip, ModuleFolders({"Media/Modules"}), {}, control);
    CHECK(launcher::ModuleKindOf(ReadBytes(folder / "eboot.bin")) == launcher::ModuleKind::Elf);

    // A damaged zip restores nothing.
    auto damaged = ReadBytes(zip);
    const auto copy = temp.Path() / "copy.zip";
    std::filesystem::copy_file(zip, copy);
    damaged[200] ^= 0xFF;
    std::ofstream(zip, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(damaged.data()), static_cast<std::streamsize>(damaged.size()));
    JobControl broken;
    CHECK_THROWS(RestoreOriginals(folder, zip, broken));
    CHECK(launcher::ModuleKindOf(ReadBytes(folder / "eboot.bin")) == launcher::ModuleKind::Elf);
    CHECK(std::filesystem::exists(zip));

    std::filesystem::remove(zip);
    std::filesystem::rename(copy, zip);
    JobControl restore;
    CHECK_EQ(RestoreOriginals(folder, zip, restore), 3);
    CHECK(ReadBytes(folder / "eboot.bin") == FromHex(golden::cases[0].selfHex));
    CHECK(ReadBytes(folder / "sce_module" / "libSelf.sprx") == FromHex(golden::cases[1].selfHex));
    CHECK(ReadBytes(folder / "Media" / "Modules" / "plugin.prx") == FromHex(golden::cases[2].selfHex));
    CHECK(!std::filesystem::exists(zip));
    CHECK(!std::filesystem::exists(folder / "eboot.bin.restore.tmp"));
}

TEST(OlderSelfBackupsMoveIntoTheZip) {
    fixture::TempDir temp;
    const auto folder = temp.Path() / "Game";
    const auto zip = temp.Path() / "Game.originals.zip";
    auto tree = SelfGame("PPSA00112");
    tree["eboot.bin"] = fixture::Elf64();
    tree["Media/Modules/plugin.prx"] = fixture::Elf64();
    tree["sce_module/libSelf.sprx"] = fixture::Elf64();
    tree["sce_module/libSelf.sprx.self"] = FromHex(golden::cases[1].selfHex);
    tree["sce_module/orphan.prx.self"] = fixture::Bytes("no module beside it");
    fixture::WriteFolder(folder, tree);
    JobControl control;
    const auto result = ConvertGameSelfFiles(folder, zip, ModuleFolders(), {}, control);
    CHECK_EQ(result.migrated, 1);
    CHECK_EQ(result.modules, 0);
    CHECK(!std::filesystem::exists(folder / "sce_module" / "libSelf.sprx.self"));
    CHECK(std::filesystem::exists(folder / "sce_module" / "orphan.prx.self"));
    {
        ZipReader reader(zip);
        CHECK(reader.ReadAll(*reader.Find("sce_module/libSelf.sprx")) == FromHex(golden::cases[1].selfHex));
    }
    JobControl restore;
    CHECK_EQ(RestoreOriginals(folder, zip, restore), 1);
    CHECK(ReadBytes(folder / "sce_module" / "libSelf.sprx") == FromHex(golden::cases[1].selfHex));

    // Nothing to do: no zip is made.
    fixture::TempDir clean;
    fixture::WriteFolder(clean.Path() / "Game", fixture::Game("PPSA00113", "Plain"));
    JobControl nothing;
    const auto none = ConvertGameSelfFiles(clean.Path() / "Game", clean.Path() / "Game.originals.zip", ModuleFolders(), {}, nothing);
    CHECK(!none.eboot && none.modules == 0 && none.migrated == 0);
    CHECK(!std::filesystem::exists(clean.Path() / "Game.originals.zip"));
}
