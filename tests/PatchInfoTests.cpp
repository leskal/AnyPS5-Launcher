#include <stdexcept>
#include <string>

#include "Fixtures.hpp"
#include "Framework.hpp"

#include "core/Metadata.hpp"
#include "core/PatchInfo.hpp"
#include "core/Scanner.hpp"

namespace {

// Trimmed from https://prosperopatches.com/PPSA21402 (November 2026).
const char* const TitleHtml = R"(<!doctype html><html><head><title>PPSA21402: Balatro &amp; Friends</title></head><body>
<ul>
	<li class="bd-links-group py-2">
		<strong class="bd-links-heading d-flex w-100 align-items-center fw-semibold">
			Title ID
		</strong>
		PPSA21402					</li>
	<li class="bd-links-group py-2">
		<strong class="bd-links-heading d-flex w-100 align-items-center fw-semibold">
			Content ID
		</strong>
		EP3495-PPSA21402_00-0421077938281357					</li>
	<li class="bd-links-group py-2">
		<strong class="bd-links-heading d-flex w-100 align-items-center fw-semibold">
			Publisher<small class="ms-2 fw-normal"><a href="/EP3495">View</a></small>						</strong>
		<a href="/EP3495" class="text-dark text-decoration-none">PLAYSTACK LIMITED</a>					</li>
	<li class="bd-links-group py-2">
		<strong class="bd-links-heading d-flex w-100 align-items-center fw-semibold">
			Region <small class="ms-2 fw-normal"><a href="#" data-url="switch-region" dynamicmodal>Switch</a></small>						</strong>
		EU					</li>
</ul>
<div class="bd-links toggling-links">
	<a href="#" class="view-toggle-btn" data-id="dynpatch">Game Updates</a>
	<a href="#" class="view-toggle-btn" data-id="dynac">Additional Content</a>
</div>
<div class="pageview" id="dynpatch" data-titleid="PPSA21402" data-key="f65a72ff9f6ad70ac518922dabbe7dd9547a227cb1fde861fd8a5618da6b9afa">
</body></html>)";

const char* const PatchJson = R"({"success":true,"titleid":"PPSA21402","patches":[
{"is_latest":false,"content_ver":"01.014.000","filesize":"249.8 MB","required_firmware":"10.60.00.06","import_date":"2025-02-24 19:25:31"},
{"is_latest":true,"content_ver":"01.015.000","filesize":"250.1 MB","required_firmware":"10.60.00.06","import_date":"2025-06-19 11:55:04"},
{"is_latest":false,"content_ver":"01.009.000","filesize":"240 MB","required_firmware":"9.00","import_date":"2024-01-01 00:00:00"}],"count":3})";


// Trimmed from https://orbispatches.com/CUSA01715 and its update list (November 2026).
const char* const Ps4TitleHtml = R"(<!doctype html><html><head><title>CUSA01715: God of War&reg; III Remastered | ORBISPatches.com</title></head><body>
<ul>
	<li class="bd-links-group py-2">
		<strong class="bd-links-heading d-flex w-100 align-items-center fw-semibold">
			Region <small class="ms-2 fw-normal"><a href="#">Switch</a></small>						</strong>
		EU					</li>
</ul>
<a href="#" class="view-toggle-btn" data-id="dynpatch">Game Updates</a>
<div id="dynpatch" data-loadparams="{ 'titleid': 'CUSA01715', 'key': 'dbff9adb553544297f6f76bf1b5a726fc3c80c99facfafe76d983d047ebdb6c6' }">
</body></html>)";

const char* const Ps4PatchJson = R"({"success":true,"titleid":"CUSA01715","patches":[
{"is_latest":true,"version":"01.02","filesize":"9.4MB","required_firmware":"7.55","creation_date":"2020-09-25","changelog_preview":"Various bug fixes.","changelog_charcount":18},
{"is_latest":false,"version":"01.01","filesize":"9.4MB","required_firmware":"2.00","creation_date":"2015-06-03","changelog_preview":"Best available audio mode","changelog_charcount":57}],
"name":"God of War\u00ae III Remastered","count":2})";

}

TEST(Ps4TitlesUseOrbisPatches) {
    CHECK_EQ(launcher::PatchPageUrl("CUSA01715"), std::string("https://orbispatches.com/CUSA01715"));
    CHECK_EQ(launcher::PatchPageUrl("PPSA21402"), std::string("https://prosperopatches.com/PPSA21402"));
    CHECK_EQ(launcher::PatchSiteName("CUSA01715"), std::string("ORBISPatches"));
    const auto page = launcher::ParseTitlePage(Ps4TitleHtml);
    CHECK_EQ(page.name, std::string("God of War\xC2\xAE III Remastered"));
    CHECK_EQ(page.region, std::string("EU"));
    CHECK_EQ(page.key, std::string("dbff9adb553544297f6f76bf1b5a726fc3c80c99facfafe76d983d047ebdb6c6"));

    std::string postedUrl;
    const launcher::TextFetcher get = [](const std::string& url) {
        if (url != "https://orbispatches.com/CUSA01715") throw std::runtime_error("unexpected " + url);
        return std::string(Ps4TitleHtml);
    };
    const launcher::TextPoster post = [&](const std::string& url, const std::string& body, const std::string&) {
        postedUrl = url;
        CHECK(body.find("dbff9adb") != std::string::npos);
        return std::string(Ps4PatchJson);
    };
    const auto info = launcher::FetchTitleOnline("CUSA01715", get, post, 1000);
    CHECK_EQ(postedUrl, std::string("https://orbispatches.com/api/internal/loadpatches"));
    CHECK_EQ(info.patches.size(), std::size_t{2});
    CHECK_EQ(info.patches[0].contentVersion, std::string("01.02"));
    CHECK_EQ(info.patches[0].importDate, std::string("2020-09-25"));
    CHECK_EQ(info.patches[0].notes, std::string("Various bug fixes."));
    CHECK_EQ(info.patches[1].notesLength, 57);

    fixture::TempDir temp;
    launcher::SaveCachedTitle(temp.Path(), info);
    const auto loaded = launcher::LoadCachedTitle(temp.Path(), "CUSA01715", 2000, 86400);
    CHECK(loaded.has_value());
    CHECK_EQ(loaded->patches[1].notes, std::string("Best available audio mode"));
    CHECK_EQ(loaded->patches[1].notesLength, 57);
}

TEST(ParsesTitlePageDetails) {
    const auto page = launcher::ParseTitlePage(TitleHtml);
    CHECK_EQ(page.name, std::string("Balatro & Friends"));
    CHECK_EQ(page.contentId, std::string("EP3495-PPSA21402_00-0421077938281357"));
    CHECK_EQ(page.publisher, std::string("PLAYSTACK LIMITED"));
    CHECK_EQ(page.region, std::string("EU"));
    CHECK_EQ(page.key, std::string("f65a72ff9f6ad70ac518922dabbe7dd9547a227cb1fde861fd8a5618da6b9afa"));
}

TEST(ParsesPatchListNewestFirst) {
    const auto patches = launcher::ParsePatchList(PatchJson);
    CHECK_EQ(patches.size(), std::size_t{3});
    CHECK_EQ(patches[0].contentVersion, std::string("01.015.000"));
    CHECK(patches[0].latest);
    CHECK_EQ(patches[0].size, std::string("250.1 MB"));
    CHECK_EQ(patches[2].contentVersion, std::string("01.009.000"));
    CHECK_THROWS(launcher::ParsePatchList(R"({"success":false,"message":"Key is invalid."})"));
    CHECK_THROWS(launcher::ParsePatchList("not json"));
}

TEST(FetchesTitleAndPostsKey) {
    std::string postedUrl;
    std::string postedBody;
    const launcher::TextFetcher get = [](const std::string& url) {
        if (url != "https://prosperopatches.com/PPSA21402") throw std::runtime_error("unexpected " + url);
        return std::string(TitleHtml);
    };
    const launcher::TextPoster post = [&](const std::string& url, const std::string& body, const std::string&) {
        postedUrl = url;
        postedBody = body;
        return std::string(PatchJson);
    };
    const auto info = launcher::FetchTitleOnline("PPSA21402", get, post, 1000);
    CHECK(info.found);
    CHECK_EQ(postedUrl, std::string("https://prosperopatches.com/api/internal/loadpatches"));
    CHECK(postedBody.find("\"titleid\":\"PPSA21402\"") != std::string::npos);
    CHECK(postedBody.find("f65a72ff") != std::string::npos);
    const auto* newest = launcher::NewestPatch(info);
    CHECK(newest != nullptr);
    CHECK_EQ(newest->contentVersion, std::string("01.015.000"));
    CHECK_THROWS(launcher::FetchTitleOnline("../etc", get, post, 1000));
}

TEST(UnknownTitleIsNotFound) {
    const launcher::TextFetcher get = [](const std::string& url) -> std::string { throw std::runtime_error("HTTP 404 for " + url + " (not found)"); };
    const launcher::TextPoster post = [](const std::string&, const std::string&, const std::string&) -> std::string { throw std::runtime_error("not called"); };
    const auto info = launcher::FetchTitleOnline("PPSA99999", get, post, 1000);
    CHECK(!info.found);
    const launcher::TextFetcher offline = [](const std::string&) -> std::string { throw std::runtime_error("cannot reach"); };
    CHECK_THROWS(launcher::FetchTitleOnline("PPSA99999", offline, post, 1000));
}

TEST(CachesTitleForADay) {
    fixture::TempDir temp;
    launcher::TitleOnlineInfo info;
    info.titleId = "PPSA21402";
    info.found = true;
    info.publisher = "PLAYSTACK LIMITED";
    info.region = "EU";
    info.fetchedAt = 1000;
    info.patches = launcher::ParsePatchList(PatchJson);
    launcher::SaveCachedTitle(temp.Path(), info);
    const auto loaded = launcher::LoadCachedTitle(temp.Path(), "PPSA21402", 2000, 86400);
    CHECK(loaded.has_value());
    CHECK_EQ(loaded->publisher, std::string("PLAYSTACK LIMITED"));
    CHECK_EQ(loaded->patches.size(), std::size_t{3});
    CHECK(loaded->patches[0].latest);
    CHECK(!launcher::LoadCachedTitle(temp.Path(), "PPSA21402", 1000 + 86401, 86400).has_value());
    CHECK(!launcher::LoadCachedTitle(temp.Path(), "PPSA00001", 2000, 86400).has_value());
}

TEST(PlatformComesFromTheTitleId) {
    CHECK_EQ(launcher::PlatformOf("PPSA01325"), std::string("PS5"));
    CHECK_EQ(launcher::PlatformOf("CUSA00001"), std::string("PS4"));
    CHECK(launcher::PlatformOf("ABCD12345").empty());
    CHECK(launcher::PlatformOf("PPSA").empty());
}

TEST(RegionFromContentId) {
    CHECK_EQ(launcher::RegionOf("EP9000-PPSA21567_00-0000000000000000"), std::string("Europe"));
    CHECK_EQ(launcher::RegionOf("UP0001-PPSA00001_00-X"), std::string("USA"));
    CHECK_EQ(launcher::RegionOf("JP0001-PPSA00001_00-X"), std::string("Japan"));
    CHECK_EQ(launcher::RegionOf(""), std::string());
    const auto json = R"({"titleId":"PPSA21567","contentId":"EP9000-PPSA21567_00-0000000000000000","contentVersion":"01.018.000"})";
    const auto metadata = launcher::ParseParamJson(fixture::Bytes(json));
    CHECK_EQ(metadata.contentId, std::string("EP9000-PPSA21567_00-0000000000000000"));
}

TEST(ComparesVersionsNumerically) {
    CHECK_EQ(launcher::CompareVersions("01.018.000", "01.18"), 0);
    CHECK_EQ(launcher::CompareVersions("01.009.000", "01.010.000"), -1);
    CHECK_EQ(launcher::CompareVersions("02.000.000", "01.999.000"), 1);
}

TEST(ParsesChangeInfo) {
    const auto notes = launcher::ParseChangeInfo(R"(<?xml version="1.0" encoding="utf-8"?>
<changeinfo>
  <changes app_ver="01.01"><![CDATA[
    Fixed a crash.
    Better <b>frame</b> pacing.
  ]]></changes>
  <changes app_ver="01.02">
    Tom &amp; Jerry mode &#233;
  </changes>
  <changes app_ver="01.03"/>
</changeinfo>)");
    CHECK_EQ(notes.size(), std::size_t{2});
    CHECK_EQ(notes[0].version, std::string("01.02"));
    CHECK_EQ(notes[0].text, std::string("Tom & Jerry mode \xC3\xA9"));
    CHECK_EQ(notes[1].text, std::string("Fixed a crash.\nBetter <b>frame</b> pacing."));
    const auto* note = launcher::ChangeNoteFor(notes, "01.001.000");
    CHECK(note != nullptr);
    CHECK_EQ(note->version, std::string("01.01"));
}

TEST(ScannerReadsChangeInfo) {
    fixture::TempDir temp;
    auto tree = fixture::Game("PPSA00001", "First");
    tree["sce_sys/changeinfo/changeinfo.xml"] = fixture::Bytes(R"(<changeinfo><changes app_ver="01.00">First release</changes></changeinfo>)");
    fixture::WriteFolder(temp.Path() / "First", tree);
    const auto result = launcher::Scan({.roots = {temp.Path()}});
    CHECK_EQ(result.games.size(), std::size_t{1});
    CHECK_EQ(result.games[0].changes.size(), std::size_t{1});
    CHECK_EQ(result.games[0].changes[0].text, std::string("First release"));
}
