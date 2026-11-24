#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Http.hpp"
#include "core/Job.hpp"
#include "core/Process.hpp"

namespace launcher {

// AnyPS5 versions the launcher can relink with. A version is a relinker and the
// runtime libraries of the same release, in one folder that ProbeAnyPS5 accepts:
//   downloaded  <launcher>/anyps5/<owner>/<repo>/<tag>/relinker-<tag>.exe, libs/, tools/titles.json
//   local       a folder of the user's: an AnyPS5 clone with a build, or a release extracted by hand
// The list lives in <launcher>/anyps5/versions.json.
inline constexpr const char* OfficialRepository = "boykopovar/AnyPS5";
inline constexpr const char* VersionsFileName = "versions.json";

struct ReleaseAsset {
    std::string name;
    std::string url;
    std::uint64_t size = 0;
    std::string sha256;  // from GitHub's "digest" field, lower-case hex; empty when GitHub gives none
};

struct Release {
    std::string tag;
    std::string name;
    std::string publishedAt;
    std::string notes;  // the release's text (GitHub's "body", Markdown)
    bool prerelease = false;
    std::vector<ReleaseAsset> assets;
};

// owner/repo as on GitHub.
bool IsValidRepository(std::string_view repository);

std::string ReleasesUrl(std::string_view repository);
std::string LatestReleaseUrl(std::string_view repository);

// GitHub's JSON for a list of releases, or for one release. Drafts are dropped.
std::vector<Release> ParseReleases(std::string_view json);
Release ParseRelease(std::string_view json);

// The two files this platform needs from a release: relinker-<tag>[.exe] and
// prx-<platform>-<tag>.zip (Windows) or .tar.gz (Linux).
struct ReleaseFiles {
    ReleaseAsset relinker;
    ReleaseAsset runtime;
};
std::optional<ReleaseFiles> FilesFor(const Release& release, std::string* problem = nullptr);

struct VersionEntry {
    std::string id;          // "<owner>/<repo>@<tag>" or "local:<folder>"
    std::string repository;  // empty for a local version
    std::string tag;         // empty for a local version
    std::filesystem::path folder;
    std::string addedAt;

    bool Local() const { return repository.empty(); }
    std::string Label() const;
};

struct VersionStore {
    std::vector<VersionEntry> versions;
    std::vector<std::string> repositories;  // custom repositories the user added, besides the official one
    std::map<std::string, std::string> pins;  // title ID -> version id: the game installs and plays with that version
};

std::filesystem::path VersionsRoot(const std::filesystem::path& launcherFolder);
VersionStore LoadVersions(const std::filesystem::path& root);
void SaveVersions(const std::filesystem::path& root, const VersionStore& store);

const VersionEntry* FindVersion(const VersionStore& store, std::string_view id);

// Adds or replaces an entry by id.
void PutVersion(VersionStore& store, VersionEntry entry);

// Downloads a release's relinker and runtime into root/<owner>/<repo>/<tag>,
// checks GitHub's SHA-256 when it gives one, unpacks the runtime with the
// system's tar (built into Windows 10 and later) and fetches the release's
// tools/titles.json when it can. The folder appears only when complete.
VersionEntry DownloadRelease(const std::filesystem::path& root, const std::string& repository, const Release& release,
                             const Downloader& download, const ProcessRunner& run, JobControl& control);

// A local AnyPS5: a relinker file or a folder. Finds the folder ProbeAnyPS5
// accepts: the folder itself, the relinker's folder (release layout), or the
// clone that holds build/core/relinker/<relinker> (build layout). Throws with the
// probe's problem when it isn't usable.
VersionEntry LocalVersion(const std::filesystem::path& relinkerOrFolder);

// Removes the entry; a downloaded version's folder is deleted too, a local one never is.
void RemoveVersion(const std::filesystem::path& root, VersionStore& store, std::string_view id);

// Newest first by publish date; prereleases only when asked.
// Downloads a release asset and checks its size and SHA-256 against GitHub's.
void FetchAsset(const Downloader& download, const ReleaseAsset& asset, const std::filesystem::path& file, JobControl& control);
// The system's tar (Windows 10 1803 and later ship one that reads .zip).
std::filesystem::path SystemTar();

std::optional<Release> NewestRelease(const std::vector<Release>& releases, bool includePrereleases = false);

}
