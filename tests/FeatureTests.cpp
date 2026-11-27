#include <filesystem>
#include <fstream>
#include <string>

#include "Fixtures.hpp"
#include "Framework.hpp"

#include "core/Diagnosis.hpp"
#include "core/FileOps.hpp"
#include "core/GameSettings.hpp"
#include "core/Job.hpp"
#include "core/SaveBackup.hpp"

namespace {

void WriteText(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string ReadText(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string EnvValue(const std::vector<std::pair<std::string, std::string>>& env, const std::string& name) {
    for (const auto& [key, value] : env) {
        if (key == name) return value;
    }
    return "<unset>";
}

}

TEST(GameSettingsBecomeEnvironment) {
    launcher::GameSettings settings;
    CHECK(launcher::GameSettingsEnvironment(settings).empty());
    settings.screen = "borderless";
    settings.presentMode = "mailbox";
    settings.fpsLimit = "59.94";
    settings.upscale = "fsr";
    settings.fsrSharpness = "0.5";
    settings.overlay = "1";
    settings.overlayPosition = "br";
    settings.language = "25";
    settings.volume = "100";  // the default: not passed
    settings.rumble = "0";
    settings.muteOnFocusLoss = true;
    settings.shaderCache = false;
    const auto env = launcher::GameSettingsEnvironment(settings);
    CHECK_EQ(EnvValue(env, "ANYPS5_FULLSCREEN"), std::string("borderless"));
    CHECK_EQ(EnvValue(env, "ANYPS5_PRESENT_MODE"), std::string("mailbox"));
    CHECK_EQ(EnvValue(env, "ANYPS5_FPS_LIMIT"), std::string("59.94"));
    CHECK_EQ(EnvValue(env, "ANYPS5_FSR_SHARPNESS"), std::string("0.5"));
    CHECK_EQ(EnvValue(env, "ANYPS5_OVERLAY_POS"), std::string("br"));
    CHECK_EQ(EnvValue(env, "ANYPS5_LANGUAGE"), std::string("25"));
    CHECK_EQ(EnvValue(env, "ANYPS5_VOLUME"), std::string("<unset>"));
    CHECK_EQ(EnvValue(env, "ANYPS5_RUMBLE"), std::string("0"));
    CHECK_EQ(EnvValue(env, "ANYPS5_MUTE_ON_FOCUS_LOSS"), std::string("1"));
    CHECK_EQ(EnvValue(env, "ANYPS5_NO_SHADER_CACHE"), std::string("1"));

    fixture::TempDir temp;
    const auto file = temp.Path() / "settings" / "PPSA00001.json";
    launcher::SaveGameSettings(file, settings);
    CHECK(launcher::LoadGameSettings(file) == settings);
    // A value AnyPS5 would refuse is dropped, so the game still starts.
    WriteText(file, R"({"presentMode":"vsync","fpsLimit":"0.1","volume":"150","screen":"borderless"})");
    const auto loaded = launcher::LoadGameSettings(file);
    CHECK(loaded.presentMode.empty());
    CHECK(loaded.fpsLimit.empty());
    CHECK(loaded.volume.empty());
    CHECK_EQ(loaded.screen, std::string("borderless"));
    CHECK(launcher::ValidFpsLimit("30"));
    CHECK(!launcher::ValidFpsLimit("30.1234"));
    CHECK(!launcher::ValidSharpness("2.5"));
}

TEST(DiagnosesRuns) {
    using Level = launcher::RunDiagnosis::Level;
    launcher::RunReport clean;
    clean.exit = "clean";
    clean.summary = "clean after 60 s";
    CHECK(launcher::DiagnoseRun(clean, "a host type out of memory was given a fallback", 0, false).level == Level::Fine);
    CHECK(launcher::DiagnoseRun(std::nullopt, "", std::nullopt, true).level == Level::Stopped);

    launcher::RunReport missing;
    missing.exit = "abort";
    missing.stopKind = "missing-function";
    missing.missingFunction = "libScePad::scePadBondUpdateInit";
    const auto needs = launcher::DiagnoseRun(missing, "", 3, false);
    CHECK(needs.level == Level::Problem);
    CHECK_EQ(needs.detail, std::string("libScePad::scePadBondUpdateInit"));

    const auto crash = launcher::DiagnoseRun(std::nullopt, "loading\r\n\nFATAL: unhandled exception 0xc0000005 on thread 12 'main'\r\n  rip in nvgpucomp64.dll\n",
                                            -1073741819, false);
    CHECK(crash.headline.find("access violation") != std::string::npos);
    CHECK(crash.headline.find("0xC0000005") != std::string::npos);
    CHECK(crash.hint.find("NVIDIA") != std::string::npos);
    const auto module = launcher::DiagnoseRun(std::nullopt, "FAIL: dependency module not found: libSceFoo.prx\n", 1, false);
    CHECK(module.headline.find("module") != std::string::npos);
    CHECK_EQ(module.detail, std::string("FAIL: dependency module not found: libSceFoo.prx"));
    const auto lost = launcher::DiagnoseRun(std::nullopt, "vkQueueSubmit: VK_ERROR_DEVICE_LOST\n", 1, false);
    CHECK(lost.headline.find("GPU") != std::string::npos);
    const auto code = launcher::DiagnoseRun(std::nullopt, "", 7, false);
    CHECK(code.headline.find("exit code 7") != std::string::npos);
}

TEST(LogTailAndLatestLog) {
    fixture::TempDir temp;
    launcher::Layout layout{temp.Path() / "userdata", temp.Path() / "library", "Player"};
    WriteText(layout.Logs("PPSA00001") / "install.log", "install");
    WriteText(layout.Logs("PPSA00001") / "2026-11-20_10-00-00.log", "old");
    WriteText(layout.Logs("PPSA00001") / "2026-11-24_19-00-00.log", std::string(100, 'x') + "\nlast line\n");
    const auto latest = launcher::LatestRunLog(layout, "PPSA00001");
    CHECK(latest.has_value());
    CHECK_EQ(latest->filename().string(), std::string("2026-11-24_19-00-00.log"));
    CHECK_EQ(launcher::ReadLogTail(*latest, 20), std::string("last line\n"));
    CHECK(!launcher::LatestRunLog(layout, "PPSA00002").has_value());
}

TEST(SavesAreBackedUpAndRestored) {
    fixture::TempDir temp;
    launcher::Layout layout{temp.Path() / "userdata", temp.Path() / "library", "Player"};
    launcher::JobControl control;
    CHECK(!launcher::HasSaves(layout, "PPSA00001"));
    CHECK_THROWS(launcher::BackUpSaves(layout, "PPSA00001", control));

    WriteText(layout.Saves("PPSA00001") / "_sd" / "SAVE0" / "sdimg_SAVE0", "first save");
    WriteText(layout.Saves("PPSA00001") / "_sd_mem" / "mem.bin", "memory");
    WriteText(layout.Trophies("PPSA00001") / "1_0.txt", "3 63000000000000000\n");
    const auto first = launcher::BackUpSaves(layout, "PPSA00001", control);
    CHECK(std::filesystem::is_regular_file(first));
    CHECK(first.parent_path() == temp.Path() / "userdata" / "backups" / "Player" / "PPSA00001");

    // Play on: the save changes, a new one appears, the memory file goes.
    WriteText(layout.Saves("PPSA00001") / "_sd" / "SAVE0" / "sdimg_SAVE0", "second save");
    WriteText(layout.Saves("PPSA00001") / "_sd" / "SAVE1" / "sdimg_SAVE1", "another");
    std::filesystem::remove_all(layout.Saves("PPSA00001") / "_sd_mem");

    const auto kept = launcher::RestoreSaves(layout, "PPSA00001", first, control);
    CHECK(kept.has_value());
    CHECK_EQ(ReadText(layout.Saves("PPSA00001") / "_sd" / "SAVE0" / "sdimg_SAVE0"), std::string("first save"));
    CHECK(!std::filesystem::exists(layout.Saves("PPSA00001") / "_sd" / "SAVE1"));
    CHECK_EQ(ReadText(layout.Saves("PPSA00001") / "_sd_mem" / "mem.bin"), std::string("memory"));
    CHECK_EQ(ReadText(layout.Trophies("PPSA00001") / "1_0.txt"), std::string("3 63000000000000000\n"));
    CHECK(!std::filesystem::exists(layout.Saves("PPSA00001").parent_path() / "PPSA00001.restoring"));

    // The state before the restore was kept, and lists with the first.
    const auto backups = launcher::ListSaveBackups(layout, "PPSA00001");
    CHECK_EQ(backups.size(), std::size_t{2});
    CHECK(backups[0].file.filename() >= backups[1].file.filename());
    launcher::RestoreSaves(layout, "PPSA00001", *kept, control);
    CHECK_EQ(ReadText(layout.Saves("PPSA00001") / "_sd" / "SAVE1" / "sdimg_SAVE1"), std::string("another"));
    CHECK_EQ(backups[1].created.size(), std::size_t{19});
}

#include <chrono>
#include <thread>

#include "core/LaunchLinks.hpp"
#include "core/SaveSync.hpp"
#include "core/SelfUpdate.hpp"
#include "core/Playtime.hpp"
#include "core/Versions.hpp"
#include "core/ZipStore.hpp"

TEST(LinksAndArguments) {
    using Kind = launcher::LaunchRequest::Kind;
    const auto play = launcher::ParseLink("anyps5://play/ppsa01234/");
    CHECK(play.has_value());
    CHECK(play->kind == Kind::Play);
    CHECK_EQ(play->titleId, std::string("PPSA01234"));
    CHECK(launcher::ParseLink("ANYPS5://game/PPSA01234")->kind == Kind::Select);
    CHECK(!launcher::ParseLink("anyps5://play/../etc").has_value());
    CHECK(!launcher::ParseLink("anyps5://delete/PPSA01234").has_value());
    CHECK(!launcher::ParseLink("https://play/PPSA01234").has_value());
    const auto args = launcher::ParseArguments({"anyps5-launcher.exe", "--play", "PPSA00001"});
    CHECK(args.has_value());
    CHECK_EQ(launcher::LinkFor(*args), std::string("anyps5://play/PPSA00001"));
    CHECK(launcher::ParseRequest(launcher::FormatRequest(*args)) == args);
    CHECK(!launcher::ParseArguments({"anyps5-launcher.exe"}).has_value());
}

TEST(FindsLauncherUpdates) {
    std::vector<launcher::Release> releases(3);
    releases[0].tag = "v0.2.0";
    releases[0].assets.push_back({launcher::LauncherAssetName("v0.2.0"), "https://example/a", 10, ""});
    releases[1].tag = "v0.3.0";
    releases[1].prerelease = true;
    releases[1].assets.push_back({launcher::LauncherAssetName("v0.3.0"), "https://example/b", 10, ""});
    releases[2].tag = "v0.2.1";  // no Windows zip
    const auto update = launcher::FindLauncherUpdate(releases, "0.1.0");
    CHECK(update.has_value());
    CHECK_EQ(update->release.tag, std::string("v0.2.0"));
    CHECK(!launcher::FindLauncherUpdate(releases, "0.2.0").has_value());
    CHECK(!launcher::FindLauncherUpdate(releases, "v0.10.0").has_value());
}

TEST(SwapsTheLauncherExe) {
    fixture::TempDir temp;
    const auto current = temp.Path() / "anyps5-launcher.exe";
    const auto fresh = temp.Path() / "update" / "v0.2.0" / "files" / "anyps5-launcher.exe";
    WriteText(current, "old");
    WriteText(fresh, "new");
    launcher::SwapLauncherExe(current, fresh);
    CHECK_EQ(ReadText(current), std::string("new"));
    CHECK_EQ(ReadText(temp.Path() / "anyps5-launcher.exe.old"), std::string("old"));
    launcher::CleanUpLauncherUpdate(current, temp.Path());
    CHECK(!std::filesystem::exists(temp.Path() / "anyps5-launcher.exe.old"));
    CHECK(!std::filesystem::exists(temp.Path() / "update"));
}

TEST(PlaytimeAddsUpAndSurvivesBadFiles) {
    fixture::TempDir temp;
    const auto file = temp.Path() / "profile" / "playtime.json";
    CHECK(launcher::LoadPlaytimes(file).empty());
    auto record = launcher::AddPlaytime(file, "PPSA01001", 125, true, 1795000000);
    CHECK_EQ(record.seconds, std::uint64_t{125});
    CHECK_EQ(record.sessions, 1);
    record = launcher::AddPlaytime(file, "PPSA01001", 60, false, 1795000060);  // same session, a minute later
    CHECK_EQ(record.seconds, std::uint64_t{185});
    CHECK_EQ(record.sessions, 1);
    launcher::AddPlaytime(file, "PPSA01002", 30, true, 1795000100);
    const auto loaded = launcher::LoadPlaytimes(file);
    CHECK_EQ(loaded.size(), std::size_t{2});
    CHECK_EQ(loaded.at("PPSA01001").seconds, std::uint64_t{185});
    CHECK_EQ(loaded.at("PPSA01001").lastPlayed, std::int64_t{1795000060});
    CHECK(!std::filesystem::exists(temp.Path() / "profile" / "playtime.json.partial"));

    WriteText(file, R"({"games": {"PPSA1": {"seconds": -5}, "PPSA2": {"seconds": 90, "sessions": 2}, "PPSA3": "x"}})");
    const auto mixed = launcher::LoadPlaytimes(file);
    CHECK_EQ(mixed.size(), std::size_t{1});
    CHECK_EQ(mixed.at("PPSA2").sessions, 2);
    WriteText(file, "not json");
    CHECK(launcher::LoadPlaytimes(file).empty());
}

TEST(PlaytimeReadsLikeAPerson) {
    CHECK_EQ(launcher::FormatPlaytime(0), std::string("Under a minute"));
    CHECK_EQ(launcher::FormatPlaytime(59), std::string("Under a minute"));
    CHECK_EQ(launcher::FormatPlaytime(45 * 60 + 30), std::string("45 min"));
    CHECK_EQ(launcher::FormatPlaytime(3 * 3600), std::string("3 h"));
    CHECK_EQ(launcher::FormatPlaytime(3 * 3600 + 10 * 60), std::string("3 h 10 min"));
    CHECK_EQ(launcher::FormatPlaytime(120 * 3600 + 59 * 60), std::string("120 h"));
}

TEST(AllSavesGoInOneZip) {
    fixture::TempDir temp;
    const auto userdata = temp.Path() / "userdata";
    launcher::Layout player{userdata, temp.Path() / "library", "Player"};
    launcher::Layout guest{userdata, temp.Path() / "library", "Guest"};
    launcher::JobControl control;
    const auto zip = temp.Path() / "out" / "saves.zip";
    std::filesystem::create_directories(zip.parent_path());
    CHECK_THROWS(launcher::BackUpAllSaves(userdata, {"Player"}, zip, control));

    WriteText(player.Saves("PPSA00001") / "_sd" / "SAVE0" / "data", "one");
    WriteText(player.Trophies("PPSA00002") / "1_0.txt", "3 63000000000000000\n");  // trophies only
    WriteText(guest.Saves("PPSA00001") / "_sd_mem" / "mem.bin", "guest");
    std::filesystem::create_directories(guest.Saves("PPSA00009"));  // an empty folder isn't a game with saves

    auto result = launcher::BackUpAllSaves(userdata, {"Player"}, zip, control);
    CHECK_EQ(result.profiles, 1);
    CHECK_EQ(result.games, 2);
    {
        launcher::ZipReader reader(zip);
        CHECK(reader.Find("Player/PPSA00001/_sd/SAVE0/data") != nullptr);
        CHECK(reader.Find("Player/PPSA00002/_trophy/1_0.txt") != nullptr);
        CHECK(reader.Find("Guest/PPSA00001/_sd_mem/mem.bin") == nullptr);
    }
    // All profiles, over the same file.
    result = launcher::BackUpAllSaves(userdata, {"Guest", "Player"}, zip, control);
    CHECK_EQ(result.profiles, 2);
    CHECK_EQ(result.games, 3);
    launcher::ZipReader reader(zip);
    CHECK_EQ(reader.Entries().size(), std::size_t{3});
    const auto* guestSave = reader.Find("Guest/PPSA00001/_sd_mem/mem.bin");
    CHECK(guestSave != nullptr);
    const auto bytes = reader.ReadAll(*guestSave);
    CHECK_EQ(std::string(bytes.begin(), bytes.end()), std::string("guest"));
    CHECK(!std::filesystem::exists(temp.Path() / "out" / "saves.zip.partial"));
}

TEST(ReleaseNotesBecomePlainLines) {
    const auto lines = launcher::ReleaseNoteLines(
        "## What's Changed\r\n* **Faster** Game settings by [leskal](https://github.com/leskal)\r\n- Fix uninstall\r\n\r\n\r\n"
        "**Full Changelog**: https://github.com/x/y/compare/v0.1.3...v0.1.4\r\n");
    CHECK_EQ(lines.size(), std::size_t{3});
    CHECK(lines[0] == (launcher::NoteLine{"What's Changed", true}));
    CHECK(lines[1] == (launcher::NoteLine{"\xE2\x80\xA2 Faster Game settings by leskal", false}));
    CHECK(lines[2] == (launcher::NoteLine{"\xE2\x80\xA2 Fix uninstall", false}));
    // The notes the release workflow writes: New and Fixed, each a heading.
    const auto grouped = launcher::ReleaseNoteLines("## New\n- Pad test\n## Fixed\n- Saves\n\n**Full Changelog**: https://x\n");
    CHECK_EQ(grouped.size(), std::size_t{5});
    CHECK(grouped[0] == (launcher::NoteLine{"New", true}));
    CHECK(grouped[2].text.empty());
    CHECK(grouped[3] == (launcher::NoteLine{"Fixed", true}));
    CHECK_EQ(grouped[4].text, std::string("\xE2\x80\xA2 Saves"));
    CHECK(launcher::ReleaseNoteLines("").empty());
    CHECK_EQ(launcher::ReleaseNoteLines("a\nb\nc\nd", 2).size(), std::size_t{2});
}

TEST(WindowPlacementRoundTrips) {
    const launcher::WindowPlacement placement{-1200, 40, 1600, 900, true};
    const auto argument = launcher::FormatWindowArgument(placement);
    CHECK_EQ(argument, std::string("--window=-1200,40,1600,900,max"));
    const auto parsed = launcher::ParseWindowArgument({"anyps5-launcher.exe", "--updated", argument});
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->x, -1200);
    CHECK_EQ(parsed->height, 900);
    CHECK(parsed->maximized);
    CHECK(!launcher::ParseWindowArgument({"--window=1,2,3"}).has_value());
    CHECK(!launcher::ParseWindowArgument({"--window=1,2,10,10"}).has_value());
    CHECK(!launcher::ParseWindowArgument({"--window=1,2,800,x00"}).has_value());
    CHECK(!launcher::ParseWindowArgument({"--window=1,2,800,600,min"}).has_value());
    CHECK(!launcher::ParseWindowArgument({"--game", "PPSA01234"}).has_value());
}

TEST(VersionPinsAreKept) {
    fixture::TempDir temp;
    launcher::VersionStore store;
    launcher::VersionEntry entry;
    entry.id = "boykopovar/AnyPS5@v0.1.5";
    entry.folder = temp.Path() / "boykopovar" / "AnyPS5" / "v0.1.5";
    store.versions.push_back(entry);
    store.pins["PPSA01234"] = entry.id;
    launcher::SaveVersions(temp.Path(), store);
    auto loaded = launcher::LoadVersions(temp.Path());
    CHECK_EQ(loaded.pins.at("PPSA01234"), entry.id);
    launcher::RemoveVersion(temp.Path(), loaded, entry.id);
    CHECK(loaded.pins.empty());
}

TEST(LocalVersionsHaveNamesNotPaths) {
    fixture::TempDir temp;
    launcher::VersionStore store;
    launcher::VersionEntry local;
    local.id = "local:H:/Games/PS5/AnyPS5/anyps5/leskal/AnyPS5";
    local.folder = temp.Path() / "builds" / "AnyPS5";
    store.versions.push_back(local);
    launcher::VersionEntry release;
    release.id = "boykopovar/AnyPS5@v0.1.5";
    release.repository = "boykopovar/AnyPS5";
    release.tag = "v0.1.5";
    release.folder = temp.Path() / "boykopovar" / "AnyPS5" / "v0.1.5";
    store.versions.push_back(release);

    CHECK_EQ(store.versions[0].Label(), std::string("AnyPS5"));  // the folder's name, not the path
    launcher::RenameVersion(store, local.id, "  My nightly build  ");
    CHECK_EQ(store.versions[0].Label(), std::string("My nightly build"));
    launcher::SaveVersions(temp.Path(), store);
    auto loaded = launcher::LoadVersions(temp.Path());
    CHECK_EQ(loaded.versions[0].name, std::string("My nightly build"));
    CHECK(loaded.versions[1].name.empty());

    // Adding the same folder again keeps the name.
    launcher::PutVersion(loaded, local);
    CHECK_EQ(launcher::FindVersion(loaded, local.id)->Label(), std::string("My nightly build"));

    launcher::RenameVersion(loaded, local.id, std::string(60, 'x'));
    CHECK_EQ(launcher::FindVersion(loaded, local.id)->name.size(), std::size_t{40});
    launcher::RenameVersion(loaded, local.id, "   ");
    CHECK_EQ(launcher::FindVersion(loaded, local.id)->Label(), std::string("AnyPS5"));
    CHECK_THROWS(launcher::RenameVersion(loaded, release.id, "x"));
    CHECK_THROWS(launcher::RenameVersion(loaded, "local:nowhere", "x"));
}

TEST(SavesSyncThroughAFolder) {
    fixture::TempDir temp;
    launcher::JobControl control;
    const auto sync = temp.Path() / "sync";
    launcher::Layout pc1{temp.Path() / "pc1", temp.Path() / "lib1", "Player"};
    launcher::Layout pc2{temp.Path() / "pc2", temp.Path() / "lib2", "Player"};
    using Action = launcher::SyncResult::Action;
    const auto setTime = [](const std::filesystem::path& file, int secondsAgo) {
        std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() - std::chrono::seconds(secondsAgo));
    };

    // PC 1 plays and pushes.
    CHECK(launcher::PullSaves(pc1, "PPSA00001", sync, control).action == Action::Nothing);
    WriteText(pc1.Saves("PPSA00001") / "_sd" / "SAVE0" / "data", "from pc1");
    setTime(pc1.Saves("PPSA00001") / "_sd" / "SAVE0" / "data", 100);
    CHECK(launcher::PushSaves(pc1, "PPSA00001", sync, control).action == Action::Pushed);
    CHECK_EQ(ReadText(launcher::SyncFolderFor(sync, pc1, "PPSA00001") / "_sd" / "SAVE0" / "data"), std::string("from pc1"));
    CHECK(launcher::PushSaves(pc1, "PPSA00001", sync, control).action == Action::Nothing);

    // PC 2 has an older save of its own: it is backed up, then replaced.
    WriteText(pc2.Saves("PPSA00001") / "_sd" / "SAVE0" / "data", "old on pc2");
    setTime(pc2.Saves("PPSA00001") / "_sd" / "SAVE0" / "data", 1000);
    const auto pulled = launcher::PullSaves(pc2, "PPSA00001", sync, control);
    CHECK(pulled.action == Action::Pulled);
    CHECK(pulled.backup.has_value());
    CHECK_EQ(ReadText(pc2.Saves("PPSA00001") / "_sd" / "SAVE0" / "data"), std::string("from pc1"));

    // PC 2 plays on and pushes; the folder's older copy is backed up first.
    WriteText(pc2.Saves("PPSA00001") / "_sd" / "SAVE0" / "data", "pc2 later");
    const auto pushed = launcher::PushSaves(pc2, "PPSA00001", sync, control);
    CHECK(pushed.action == Action::Pushed);
    CHECK(pushed.backup.has_value());
    // PC 1 gets it back.
    CHECK(launcher::PullSaves(pc1, "PPSA00001", sync, control).action == Action::Pulled);
    CHECK_EQ(ReadText(pc1.Saves("PPSA00001") / "_sd" / "SAVE0" / "data"), std::string("pc2 later"));
}

// A message with our UTF-8 text and the system's ANSI text after it: the UTF-8 part
// stays as it is (it was read as ANSI as a whole before, and Greek showed as garbage).
TEST(ReadableTextKeepsTheUtf8PartOfAMixedMessage) {
    const std::string utf8 = "cannot move: \xCE\x94\xCE\xB5\xCE\xBD";  // "Δεν"
    CHECK_EQ(launcher::ReadableText(utf8), utf8);
    const auto mixed = launcher::ReadableText(utf8 + ": \xC4\xE5\xED");  // "Δεν" in Windows-1253
    CHECK(mixed.rfind(utf8 + ": ", 0) == 0);
    CHECK(launcher::ReadableText(mixed) == mixed);  // valid UTF-8 now
}
