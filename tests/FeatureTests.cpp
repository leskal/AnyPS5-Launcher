#include <filesystem>
#include <fstream>
#include <string>

#include "Fixtures.hpp"
#include "Framework.hpp"

#include "core/Diagnosis.hpp"
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
