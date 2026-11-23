#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include "Fixtures.hpp"
#include "Framework.hpp"

#include "core/AnyPS5.hpp"
#include "core/Utf8.hpp"
#include "core/Versions.hpp"

using namespace launcher;

namespace {

#ifdef _WIN32
const std::string RelinkerAsset = "relinker-v0.1.6.exe";
const std::string RuntimeAsset = "prx-windows-v0.1.6.zip";
#else
const std::string RelinkerAsset = "relinker-v0.1.6";
const std::string RuntimeAsset = "prx-linux-v0.1.6.tar.gz";
#endif

std::string ReleaseJson(const std::string& tag, bool draft, bool prerelease, const std::string& published, const std::string& digest = "") {
    const auto asset = [&](const std::string& name, int size) {
        return R"({"name":")" + name + R"(","size":)" + std::to_string(size) + R"(,"browser_download_url":"https://github.com/boykopovar/AnyPS5/releases/download/)" + tag + "/" + name +
               R"(")" + (digest.empty() ? std::string() : R"(,"digest":")" + digest + R"(")") + "}";
    };
    std::string relinker = RelinkerAsset;
    std::string runtime = RuntimeAsset;
    const auto replace = [&](std::string& name) { name.replace(name.find("v0.1.6"), 6, tag); };
    replace(relinker);
    replace(runtime);
    return R"({"tag_name":")" + tag + R"(","name":")" + tag + R"(","draft":)" + (draft ? "true" : "false") + R"(,"prerelease":)" + (prerelease ? "true" : "false") +
           R"(,"published_at":")" + published + R"(","assets":[)" + asset(relinker, 9) + "," + asset(runtime, 7) + "," + asset("web-" + tag + ".zip", 3) + "]}";
}

std::string ReadFileText(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

struct FakeNetwork {
    std::map<std::string, std::string> files;
    std::vector<std::string> requested;

    launcher::Downloader Get() {
        return [this](const std::string& url, const std::filesystem::path& file, JobControl& control) {
            requested.push_back(url);
            const auto found = files.find(url);
            if (found == files.end()) throw std::runtime_error("HTTP 404 for " + url);
            std::filesystem::create_directories(file.parent_path());
            std::ofstream(file, std::ios::binary) << found->second;
            control.Advance(found->second.size());
        };
    }
};

// Stands in for tar: "unpacks" a runtime archive into libs/.
ProcessRunner FakeTar(std::vector<std::string>* seen) {
    return [seen](const std::vector<std::string>& args, const std::filesystem::path&, const std::filesystem::path&, JobControl&) {
        *seen = args;
        const auto target = FromUtf8(args.back());
        fixture::WriteFolder(target, {{"libs/libc.prx", fixture::Bytes("c")}, {"libs/libkernel.prx", fixture::Bytes("k")},
                                      {"libs/libgcc_s_seh-1.dll", fixture::Bytes("g")}, {"libs/libstdc++-6.dll", fixture::Bytes("s")},
                                      {"libs/libwinpthread-1.dll", fixture::Bytes("w")}});
        return 0;
    };
}

}

TEST(RepositoryNamesAndReleaseUrls) {
    CHECK(IsValidRepository("boykopovar/AnyPS5"));
    CHECK(IsValidRepository("leskal/AnyPS5-fork_2.x"));
    CHECK(!IsValidRepository("AnyPS5"));
    CHECK(!IsValidRepository("a/b/c"));
    CHECK(!IsValidRepository("/AnyPS5"));
    CHECK(!IsValidRepository("owner/.."));
    CHECK(!IsValidRepository("own er/x"));
    CHECK_EQ(LatestReleaseUrl("boykopovar/AnyPS5"), std::string("https://api.github.com/repos/boykopovar/AnyPS5/releases/latest"));
}

TEST(ParsesGitHubReleasesAndPicksTheFilesForThisSystem) {
    const std::string digest = "sha256:" + std::string(64, 'A');
    const auto json = "[" + ReleaseJson("v0.1.6", false, false, "2026-11-20T10:00:00Z", digest) + "," + ReleaseJson("v0.1.7", true, false, "2026-11-22T10:00:00Z") + "," +
                      ReleaseJson("v0.2.0-rc1", false, true, "2026-11-23T10:00:00Z") + "]";
    const auto releases = ParseReleases(json);
    CHECK_EQ(releases.size(), std::size_t{2});  // the draft is dropped
    CHECK_EQ(releases[0].tag, std::string("v0.1.6"));
    CHECK_EQ(releases[0].assets.size(), std::size_t{3});
    CHECK_EQ(releases[0].assets[0].sha256, std::string(64, 'a'));
    const auto files = FilesFor(releases[0]);
    CHECK(files.has_value());
    CHECK_EQ(files->relinker.name, RelinkerAsset);
    CHECK_EQ(files->runtime.name, RuntimeAsset);
    const auto newest = NewestRelease(releases);
    const auto newestAny = NewestRelease(releases, true);
    CHECK_EQ(newest->tag, std::string("v0.1.6"));
    CHECK_EQ(newestAny->tag, std::string("v0.2.0-rc1"));

    Release bare;
    bare.tag = "v9";
    std::string problem;
    CHECK(!FilesFor(bare, &problem).has_value());
    CHECK(problem.find("has no") != std::string::npos);
    CHECK_THROWS(ParseReleases(R"({"message":"Not Found"})"));
    CHECK_THROWS(ParseRelease("not json"));
    const auto single = ParseRelease(ReleaseJson("v0.1.6", false, false, "x"));
    CHECK_EQ(single.tag, std::string("v0.1.6"));
}

TEST(DownloadsAReleaseIntoItsOwnFolder) {
    fixture::TempDir temp;
    const auto root = temp.Path() / "anyps5";
    const auto release = ParseRelease(ReleaseJson("v0.1.6", false, false, "2026-11-20T10:00:00Z"));
    const auto files = *FilesFor(release);
    FakeNetwork network;
    network.files[files.relinker.url] = std::string(9, 'r');  // the sizes the release lists
    network.files[files.runtime.url] = std::string(7, 'a');
    network.files["https://raw.githubusercontent.com/boykopovar/AnyPS5/v0.1.6/tools/titles.json"] = R"({"PPSA00001":{"relink":["--to-intel"]}})";
    std::vector<std::string> tar;
    JobControl control;
    const auto entry = DownloadRelease(root, OfficialRepository, release, network.Get(), FakeTar(&tar), control);

    const auto folder = root / "boykopovar" / "AnyPS5" / "v0.1.6";
    CHECK(entry.folder == folder);
    CHECK_EQ(entry.id, std::string("boykopovar/AnyPS5@v0.1.6"));
    CHECK(std::filesystem::is_regular_file(folder / FromUtf8(RelinkerAsset)));
    CHECK(std::filesystem::is_regular_file(folder / "libs" / "libc.prx"));
    CHECK(std::filesystem::is_regular_file(folder / "tools" / "titles.json"));
    CHECK(!std::filesystem::exists(folder / FromUtf8(RuntimeAsset)));
    CHECK(!std::filesystem::exists(root / "boykopovar" / "AnyPS5" / "v0.1.6.partial"));
    CHECK_EQ(tar[1], std::string("-xf"));
    CHECK_EQ(control.Done(), control.Total());
    const auto probe = ProbeAnyPS5(ToUtf8(folder));
    CHECK(probe.runtime.has_value());
    CHECK(probe.runtime->titleCatalog.has_value());

    // The list round-trips, with downloaded folders kept relative to the root.
    VersionStore store;
    PutVersion(store, entry);
    store.repositories.push_back("leskal/AnyPS5");
    SaveVersions(root, store);
    CHECK(ReadFileText(root / VersionsFileName).find("\"folder\": \"boykopovar/AnyPS5/v0.1.6\"") != std::string::npos);
    auto loaded = LoadVersions(root);
    CHECK_EQ(loaded.versions.size(), std::size_t{1});
    CHECK(loaded.versions[0].folder == folder);
    CHECK(loaded.repositories == std::vector<std::string>{"leskal/AnyPS5"});
    RemoveVersion(root, loaded, entry.id);
    CHECK(loaded.versions.empty());
    CHECK(!std::filesystem::exists(folder));
}

TEST(RefusesADownloadThatDoesNotMatchGitHub) {
    fixture::TempDir temp;
    const auto root = temp.Path() / "anyps5";
    const std::string zeros(64, '0');
    const auto release = ParseRelease(ReleaseJson("v0.1.6", false, false, "x", "sha256:" + zeros));
    const auto files = *FilesFor(release);
    FakeNetwork network;
    network.files[files.relinker.url] = std::string(9, 'r');
    network.files[files.runtime.url] = std::string(7, 'a');
    std::vector<std::string> tar;
    JobControl control;
    std::string message;
    try {
        DownloadRelease(root, OfficialRepository, release, network.Get(), FakeTar(&tar), control);
    } catch (const std::exception& failure) {
        message = failure.what();
    }
    CHECK(message.find("SHA-256") != std::string::npos);
    CHECK(!std::filesystem::exists(root / "boykopovar" / "AnyPS5" / "v0.1.6"));
    CHECK(!std::filesystem::exists(root / "boykopovar" / "AnyPS5" / "v0.1.6.partial"));

    const auto plain = ParseRelease(ReleaseJson("v0.1.6", false, false, "x"));
    network.files[files.relinker.url] = "short";
    JobControl again;
    CHECK_THROWS(DownloadRelease(root, OfficialRepository, plain, network.Get(), FakeTar(&tar), again));
    CHECK_THROWS(DownloadRelease(root, "not a repo", plain, network.Get(), FakeTar(&tar), again));
}

TEST(LocalVersionsComeFromABuildOrARelease) {
    fixture::TempDir temp;
#ifdef _WIN32
    const std::string relinker = "relinker.exe";
#else
    const std::string relinker = "relinker";
#endif
    const auto clone = temp.Path() / "AnyPS5";
    fixture::WriteFolder(clone, {{"build/core/relinker/" + relinker, fixture::Bytes("r")}, {"build/core/libs/libs/libc.prx", fixture::Bytes("c")},
                                 {"build/core/libs/libs/libgcc_s_seh-1.dll", fixture::Bytes("g")}, {"build/core/libs/libs/libstdc++-6.dll", fixture::Bytes("s")},
                                 {"build/core/libs/libs/libwinpthread-1.dll", fixture::Bytes("w")}});
    const auto fromFile = LocalVersion(clone / "build" / "core" / "relinker" / FromUtf8(relinker));
    CHECK(fromFile.folder == std::filesystem::absolute(clone).lexically_normal());
    CHECK(fromFile.Local());
    CHECK(fromFile.id.rfind("local:", 0) == 0);
    CHECK(LocalVersion(clone).folder == fromFile.folder);

    const auto release = temp.Path() / "release";
    fixture::WriteFolder(release, {{"relinker-v0.1.6" + std::string(TargetIsWindows ? ".exe" : ""), fixture::Bytes("r")}, {"libs/libc.prx", fixture::Bytes("c")},
                                   {"libs/libgcc_s_seh-1.dll", fixture::Bytes("g")}, {"libs/libstdc++-6.dll", fixture::Bytes("s")},
                                   {"libs/libwinpthread-1.dll", fixture::Bytes("w")}});
    CHECK(LocalVersion(release / FromUtf8("relinker-v0.1.6" + std::string(TargetIsWindows ? ".exe" : ""))).folder == std::filesystem::absolute(release).lexically_normal());

    fixture::WriteFolder(temp.Path() / "lonely", {{relinker, fixture::Bytes("r")}});
    std::string message;
    try {
        LocalVersion(temp.Path() / "lonely" / FromUtf8(relinker));
    } catch (const std::exception& failure) {
        message = failure.what();
    }
    CHECK(message.find("not a usable AnyPS5") != std::string::npos);

    VersionStore store;
    PutVersion(store, fromFile);
    RemoveVersion(temp.Path() / "anyps5", store, fromFile.id);
    CHECK(store.versions.empty());
    CHECK(std::filesystem::exists(clone / "build" / "core" / "relinker" / FromUtf8(relinker)));
}
