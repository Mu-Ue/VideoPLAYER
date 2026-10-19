// The Matroska reader: subtitle tracks and their cues, read on a thread of its own around seeks; the key frames of
// the index; the audio tracks' CodecDelay.

#include <algorithm>
#include <map>
#include <random>
#include <set>

#include "config.h"
#include "harness.h"
#include "media/mkv.h"
#include "media/subtitles.h"
#include "mkv_writer.h"

namespace {

// What the reader posted: each track's cues, in the order they came, and the seek numbers answered.
struct Posted {
    std::map<DWORD, std::vector<Cue>> cues;
    std::vector<UINT> ready;
    std::vector<std::pair<DWORD, size_t>> order;  // each batch's track and how many cues the track had before it
} posted;

LRESULT CALLBACK Collect(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP_SUBTITLE_CUES) {
        std::unique_ptr<std::vector<Cue>> batch(reinterpret_cast<std::vector<Cue>*>(lp));
        auto& to = posted.cues[DWORD(wp)];
        posted.order.push_back({DWORD(wp), to.size()});
        to.insert(to.end(), batch->begin(), batch->end());
        return 0;
    }
    if (msg == WM_APP_SUBTITLES_READY) {
        posted.ready.push_back(UINT(wp));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND Window() {
    static HWND hwnd = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Collect;
        wc.lpszClassName = L"mkv-tests";
        RegisterClassW(&wc);
        return CreateWindowW(L"mkv-tests", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    }();
    return hwnd;
}

// Starts reading a file's subtitles and waits until all of it is read: the reader then answers its last seek, 0
// if none was asked. Returns the tracks.
std::vector<Track> ReadAll(const std::wstring& path, int ms = 10000) {
    StopMkvSubtitles();
    t::Pump(0);
    posted = {};
    std::vector<Track> tracks = StartMkvSubtitles(path, Window());
    if (!tracks.empty()) {
        // Once done, it no longer answers seeks.
        const bool done = t::PumpUntil([] { return !posted.ready.empty(); }, ms);
        CHECK(done);
        if (!done) t::Fail(__FILE__, __LINE__, "the reader never finished: a hang");
    }
    return tracks;
}

std::wstring Write(const mkv::File& f, const wchar_t* name = L"test.mkv") { return t::WriteTemp(name, mkv::Build(f)); }

mkv::Track Video() {
    mkv::Track v;
    v.number = 1;
    v.type = 1;
    v.codec = "V_MPEG4/ISO/AVC";
    return v;
}

mkv::Track Text(UINT64 number, std::string codec = "S_TEXT/UTF8", std::string language = "") {
    mkv::Track s;
    s.number = number;
    s.codec = std::move(codec);
    s.language = std::move(language);
    return s;
}

// A film of `seconds`: video key frames each second, and on each text track a cue every `every` ms lasting `length`,
// whose text tells its track and number.
mkv::File Film(int seconds, std::vector<mkv::Track> texts, INT64 every = 1000, INT64 length = 500) {
    mkv::File f;
    f.tracks.push_back(Video());
    for (auto& t : texts) f.tracks.push_back(t);
    for (INT64 ms = 0; ms < seconds * 1000LL; ms += 1000) f.blocks.push_back({1, ms, "video"});
    for (const auto& t : texts) {
        for (INT64 k = 0; k * every < seconds * 1000LL; ++k) {
            f.blocks.push_back({t.number, k * every + INT64(t.number) * 10,
                                "t" + std::to_string(t.number) + " c" + std::to_string(k), length});
        }
    }
    std::stable_sort(f.blocks.begin(), f.blocks.end(), [](const mkv::Block& a, const mkv::Block& b) { return a.ms < b.ms; });
    return f;
}

// The cues a text track of a Film has.
size_t FilmCues(int seconds, INT64 every) { return size_t((seconds * 1000LL + every - 1) / every); }

bool AllOnce(const std::vector<Cue>& cues) {
    std::set<std::pair<double, std::wstring>> seen;
    for (const Cue& c : cues) {
        if (!seen.insert({c.start, c.text}).second) return false;
    }
    return true;
}

}  // namespace

// --- Real files, as ffmpeg writes them ---

TEST(mkv_real_file_tracks_and_cues) {
    const std::vector<Track> tracks = ReadAll(t::Media(L"tracks.mkv"));
    REQUIRE_EQ(tracks.size(), size_t(3));
    CHECK_EQ(tracks[0].language, std::wstring(L"eng"));
    CHECK_EQ(tracks[1].language, std::wstring(L"ger"));
    CHECK_EQ(tracks[2].language, std::wstring(L"jpn"));
    CHECK_EQ(tracks[2].name, std::wstring(L"Styled"));
    const char* expected[3][3] = {{"One", "Two", "Three\nsecond line"}, {"Eins", "Zwei", "Drei"},
                                  {"One", "Two", "Three\nsecond line"}};
    const double starts[3] = {0.5, 2, 4}, ends[3] = {1.5, 3, 5.5};
    for (size_t k = 0; k < 3; ++k) {
        NOTE("track " + std::to_string(k));
        std::vector<Cue> cues = posted.cues[tracks[k].id];
        std::sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
        REQUIRE_EQ(cues.size(), size_t(3));
        for (size_t i = 0; i < 3; ++i) {
            CHECK_EQ(t::Utf8(cues[i].text), std::string(expected[k][i]));
            CHECK_NEAR(cues[i].start, starts[i], 0.0011);
            CHECK_NEAR(cues[i].end, ends[i], 0.0011);
        }
        // <i>Two</i> in SRT, and the same in ASS once ffmpeg converts it.
        if (k != 1) CHECK_EQ(cues[1].spans.size(), size_t(1));
    }
}

TEST(mkv_real_file_without_an_index) {
    const std::vector<Track> tracks = ReadAll(t::Media(L"noindex.mkv"));
    REQUIRE_EQ(tracks.size(), size_t(1));
    CHECK_EQ(posted.cues[tracks[0].id].size(), size_t(3));
    CHECK(AllOnce(posted.cues[tracks[0].id]));
}

TEST(mkv_sample_file) {
    const std::vector<Track> tracks = ReadAll(t::Media(L"..\\sample.mkv"));
    REQUIRE_EQ(tracks.size(), size_t(1));
    CHECK_EQ(tracks[0].language, std::wstring(L"eng"));
    CHECK(posted.cues[tracks[0].id].size() >= 5);
}

TEST(mkv_real_file_key_frames_and_delays) {
    const std::vector<LONGLONG> keys = MkvKeyFrames(t::Media(L"tracks.mkv").c_str());
    REQUIRE_EQ(keys.size(), size_t(6));  // 25 fps, a key frame each 25 frames, 6 s
    for (size_t i = 0; i < keys.size(); ++i) CHECK_NEAR(keys[i], LONGLONG(i) * 10'000'000, 10'000);
    CHECK_EQ(MkvAudioDelays(t::Media(L"tracks.mkv").c_str()).size(), size_t(2));
    const std::vector<LONGLONG> pcm = MkvAudioDelays(t::Media(L"av_pcm.mkv").c_str());
    REQUIRE_EQ(pcm.size(), size_t(1));
    CHECK_EQ(pcm[0], LONGLONG(0));
    CHECK(MkvKeyFrames(t::Media(L"noindex.mkv").c_str()).empty());
}

TEST(mkv_other_files_are_not_matroska) {
    for (const wchar_t* name : {L"clip30.mp4", L"audio.m4a", L"subs_en.srt"}) {
        NOTE(t::Utf8(name));
        CHECK(MkvKeyFrames(t::Media(name).c_str()).empty());
        CHECK(MkvAudioDelays(t::Media(name).c_str()).empty());
        CHECK(StartMkvSubtitles(t::Media(name), Window()).empty());
    }
    CHECK(StartMkvSubtitles(t::TempDir() + L"\\missing.mkv", Window()).empty());
    CHECK(MkvKeyFrames(L"NUL").empty());
    CHECK(StartMkvSubtitles(L"\\\\.\\NUL", Window()).empty());
}

// --- Track headers ---

TEST(mkv_track_languages_names_and_ids) {
    mkv::File f = Film(3, {Text(3, "S_TEXT/UTF8", "fre"), Text(4, "S_TEXT/UTF8"), Text(9, "S_TEXT/WEBVTT")});
    f.tracks[2].name = "Forced";
    f.tracks[3].bcp47 = "pt-BR";
    f.tracks[3].language = "por";
    const std::vector<Track> tracks = ReadAll(Write(f));
    REQUIRE_EQ(tracks.size(), size_t(3));
    CHECK_EQ(tracks[0].id, DWORD(3));
    CHECK_EQ(tracks[0].language, std::wstring(L"fre"));
    CHECK_EQ(tracks[1].language, std::wstring(L"eng"));  // Matroska's default
    CHECK_EQ(tracks[1].name, std::wstring(L"Forced"));
    CHECK_EQ(tracks[2].language, std::wstring(L"pt-BR"));  // BCP 47 wins
    for (const Track& t : tracks) CHECK_EQ(posted.cues[t.id].size(), FilmCues(3, 1000));
}

TEST(mkv_only_text_subtitles_are_listed) {
    mkv::File f = Film(2, {Text(2), Text(3, "S_HDMV/PGS"), Text(4, "S_VOBSUB"), Text(5, "S_TEXT/SSA")});
    mkv::Track audio;
    audio.number = 6;
    audio.type = 2;
    audio.codec = "A_AAC";
    f.tracks.push_back(audio);
    const std::vector<Track> tracks = ReadAll(Write(f));
    REQUIRE_EQ(tracks.size(), size_t(2));
    CHECK_EQ(tracks[0].id, DWORD(2));
    CHECK_EQ(tracks[1].id, DWORD(5));
}

TEST(mkv_a_track_it_cant_read_means_none) {
    for (int kind = 0; kind < 3; ++kind) {
        NOTE("kind " + std::to_string(kind));
        mkv::File f = Film(2, {Text(2), Text(3)});
        if (kind == 0) f.tracks[2].compressed = true;
        if (kind == 1) f.tracks[2].encrypted = true;
        if (kind == 2) f.tracks[2].number = 2;  // twice the same number
        CHECK(ReadAll(Write(f)).empty());
    }
}

TEST(mkv_doc_types) {
    mkv::File f = Film(2, {Text(2)});
    f.docType = "webm";
    CHECK_EQ(ReadAll(Write(f)).size(), size_t(1));
    f.docType = "matroskaX";
    CHECK(ReadAll(Write(f)).empty());
    f.docType = "";
    CHECK(ReadAll(Write(f)).empty());
}

// --- Cue contents ---

TEST(mkv_header_stripping_is_undone) {
    mkv::File f = Film(0, {});
    mkv::Track s = Text(2);
    s.strip = "Hello, ";
    f.tracks.push_back(s);
    f.blocks.push_back({2, 1000, "world", 500});
    const auto tracks = ReadAll(Write(f));
    REQUIRE_EQ(tracks.size(), size_t(1));
    REQUIRE_EQ(posted.cues[2].size(), size_t(1));
    CHECK_EQ(posted.cues[2][0].text, std::wstring(L"Hello, world"));
}

TEST(mkv_ass_fields_are_skipped) {
    mkv::File f = Film(0, {});
    f.tracks.push_back(Text(2, "S_TEXT/ASS"));
    f.blocks.push_back({2, 1000, "1,0,Default,Name,0,0,0,,{\\an8}Text, with a comma", 500});
    f.blocks.push_back({2, 2000, "2,0,Default,,0,0,0", 500});  // too few fields: no text
    f.blocks.push_back({2, 3000, "3,0,Default,,0,0,0,,line\\Nbreak\r\n", 500});
    ReadAll(Write(f));
    std::vector<Cue> cues = posted.cues[2];
    std::sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
    REQUIRE_EQ(cues.size(), size_t(2));
    CHECK_EQ(cues[0].text, std::wstring(L"Text, with a comma"));
    CHECK(cues[0].top);
    CHECK_EQ(cues[1].text, std::wstring(L"line\nbreak"));
}

TEST(mkv_durations) {
    mkv::File f = Film(0, {});
    mkv::Track s = Text(2);
    s.defaultDurationNs = 2'000'000'000;
    f.tracks.push_back(s);
    f.blocks.push_back({2, 1000, "default"});              // a SimpleBlock: the track's default
    f.blocks.push_back({2, 5000, "own", 750});            // its group's own
    f.blocks.push_back({2, 9000, "group", -1, true});     // a group without one: the default
    ReadAll(Write(f));
    std::vector<Cue> cues = posted.cues[2];
    std::sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
    REQUIRE_EQ(cues.size(), size_t(3));
    CHECK_NEAR(cues[0].end - cues[0].start, 2.0, 1e-9);
    CHECK_NEAR(cues[1].end - cues[1].start, 0.75, 1e-9);
    CHECK_NEAR(cues[2].end - cues[2].start, 2.0, 1e-9);
}

TEST(mkv_cues_without_text_or_length_are_dropped) {
    mkv::File f = Film(0, {});
    f.tracks.push_back(Text(2));
    f.blocks.push_back({2, 1000, "no duration"});  // and no default: lasts nothing
    f.blocks.push_back({2, 2000, "", 500});
    f.blocks.push_back({2, 3000, "<i></i>", 500});
    f.blocks.push_back({2, 4000, "laced", 500, false, true});
    f.blocks.push_back({2, 5000, "kept", 500});
    ReadAll(Write(f));
    REQUIRE_EQ(posted.cues[2].size(), size_t(1));
    CHECK_EQ(posted.cues[2][0].text, std::wstring(L"kept"));
}

TEST(mkv_timestamp_scale) {
    mkv::File f = Film(0, {});
    f.timestampScale = 100000;  // 0.1 ms
    f.tracks.push_back(Text(2));
    f.blocks.push_back({1, 0, "v"});
    f.blocks.push_back({2, 1, "a", 2});
    f.blocks.push_back({1, 1000, "v"});
    f.blocks.push_back({2, 1234, "b", 100});
    f.clusterMs = 500;  // a block's time within its cluster is 16 bits: 3.2 s at this scale
    ReadAll(Write(f));
    std::vector<Cue> cues = posted.cues[2];
    std::sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
    REQUIRE_EQ(cues.size(), size_t(2));
    CHECK_NEAR(cues[1].start, 1.234, 1e-9);
    CHECK_NEAR(cues[1].end, 1.334, 1e-9);
    const std::vector<LONGLONG> keys = MkvKeyFrames(Write(f, L"scaled.mkv").c_str());
    REQUIRE_EQ(keys.size(), size_t(2));
    CHECK_EQ(keys[1], LONGLONG(10'000'000));
}

// --- Every way through the file ---

namespace {

struct Layout {
    const char* name;
    void (*apply)(mkv::File&);
};

const Layout kLayouts[] = {
    {"full index", [](mkv::File&) {}},
    {"full index with durations", [](mkv::File& f) { f.cueDurations = true; }},
    {"full index without relative positions", [](mkv::File& f) { f.cueRelative = false; }},
    {"index first", [](mkv::File& f) { f.cuesFirst = true; }},
    {"video index", [](mkv::File& f) { f.index = mkv::File::kVideoIndex; }},
    {"no index", [](mkv::File& f) { f.index = mkv::File::kNoIndex; }},
    {"no index, clusters of unknown size",
     [](mkv::File& f) {
         f.index = mkv::File::kNoIndex;
         f.unknownClusters = true;
     }},
    {"full index, clusters of unknown size", [](mkv::File& f) { f.unknownClusters = true; }},
    {"CRC-32s", [](mkv::File& f) { f.crc = true; }},
    {"no seek head", [](mkv::File& f) { f.seekHead = false; }},
    {"segment of unknown size",
     [](mkv::File& f) {
         f.unknownSegment = true;
         f.seekHead = true;
     }},
    {"small clusters, no index",
     [](mkv::File& f) {
         f.clusterMs = 200;
         f.index = mkv::File::kNoIndex;
     }},
};

}  // namespace

TEST_TIMED(mkv_every_layout_delivers_every_cue_once, 60000) {
    for (const Layout& layout : kLayouts) {
        NOTE(layout.name);
        mkv::File f = Film(60, {Text(2), Text(3, "S_TEXT/UTF8", "ger")}, 700, 1500);
        layout.apply(f);
        const std::vector<Track> tracks = ReadAll(Write(f));
        REQUIRE_EQ(tracks.size(), size_t(2));
        for (const Track& t : tracks) {
            const std::vector<Cue>& cues = posted.cues[t.id];
            CHECK_EQ(cues.size(), FilmCues(60, 700));
            CHECK(AllOnce(cues));
            for (const Cue& c : cues) {
                CHECK_NEAR(c.end - c.start, 1.5, 1e-9);
                CHECK(c.text.rfind(L"t" + std::to_wstring(t.id) + L" ", 0) == 0);
            }
        }
        // Batches start at one cue and double, so the first cue comes at once.
        REQUIRE(posted.order.size() >= 2);
        const auto& [first, before] = posted.order[0];
        CHECK_EQ(before, size_t(0));
        const auto next = std::find_if(posted.order.begin() + 1, posted.order.end(), [&](const auto& o) { return o.first == first; });
        if (next != posted.order.end()) CHECK_EQ(next->second, size_t(1));
    }
}

namespace {

// The cues a track of a film shows at a time, by their text.
std::set<std::wstring> ShowingAt(const mkv::File& f, UINT64 track, double seconds) {
    std::set<std::wstring> out;
    for (const mkv::Block& b : f.blocks) {
        if (b.track == track && b.ms / 1000.0 <= seconds && seconds < (b.ms + b.durationMs) / 1000.0) {
            out.insert(std::wstring(b.data.begin(), b.data.end()));
        }
    }
    return out;
}

}  // namespace

TEST_TIMED(mkv_a_seek_brings_the_cues_there_first, 120000) {
    // Over a long film the reader answers a seek once the cues that show there, on every track, have come.
    for (const Layout& layout : kLayouts) {
        NOTE(layout.name);
        mkv::File f = Film(3000, {Text(2), Text(3)}, 2000, 1000);
        // A long cue starting 20 s before a seek target, on a track of its own: without durations in the index the
        // reader looks back only until each track has a cue that ended by then, so it takes a track's cues not to
        // overlap.
        f.tracks.push_back(Text(4));
        f.blocks.push_back({4, 1980'000, "long", 25'000});
        std::stable_sort(f.blocks.begin(), f.blocks.end(), [](const mkv::Block& a, const mkv::Block& b) { return a.ms < b.ms; });
        layout.apply(f);
        StopMkvSubtitles();
        t::Pump(0);
        posted = {};
        REQUIRE(!StartMkvSubtitles(Write(f), Window()).empty());
        UINT number = 0;
        for (const double target : {2000.5, 10.25, 2999.0, 1501.0, 2000.5}) {
            NOTE("seek to " + std::to_string(target));
            ++number;
            if (!SeekMkvSubtitles(target, number)) break;  // read all already: none answered
            const bool answered =
                t::PumpUntil([&] { return std::find(posted.ready.begin(), posted.ready.end(), number) != posted.ready.end(); }, 10000);
            REQUIRE(answered);
            for (const UINT64 track : {UINT64(2), UINT64(3), UINT64(4)}) {
                std::set<std::wstring> got;
                for (const Cue& c : posted.cues[DWORD(track)]) got.insert(c.text);
                for (const std::wstring& want : ShowingAt(f, track, target)) {
                    NOTE("missing " + t::Utf8(want));
                    CHECK(got.count(want));
                }
            }
        }
        // And on to the end by itself: everything once, after which it answers no more seeks.
        const size_t all = 2 * FilmCues(3000, 2000) + 1;
        CHECK(t::PumpUntil([&] { return posted.cues[2].size() + posted.cues[3].size() + posted.cues[4].size() >= all; }, 20000));
        CHECK(t::PumpUntil([&] { return !SeekMkvSubtitles(0, ++number); }, 5000));
        t::Pump(50);
        CHECK_EQ(posted.cues[2].size(), FilmCues(3000, 2000));
        CHECK_EQ(posted.cues[3].size(), FilmCues(3000, 2000));
        CHECK_EQ(posted.cues[4].size(), size_t(1));
        CHECK(AllOnce(posted.cues[2]));
        CHECK(AllOnce(posted.cues[3]));
    }
}

TEST(mkv_seek_answers_follow_the_numbers) {
    // Seeks in quick succession: each answer names a seek asked, never a later one than the last, and the last
    // asked is answered.
    mkv::File f = Film(600, {Text(2)}, 500, 400);
    f.index = mkv::File::kNoIndex;
    StopMkvSubtitles();
    t::Pump(0);
    posted = {};
    REQUIRE(!StartMkvSubtitles(Write(f), Window()).empty());
    UINT asked = 0;
    for (int i = 1; i <= 20; ++i) {
        if (SeekMkvSubtitles(i * 29.0, UINT(i))) asked = UINT(i);
    }
    if (asked) {
        CHECK(t::PumpUntil([&] { return std::find(posted.ready.begin(), posted.ready.end(), asked) != posted.ready.end(); }, 10000));
        for (const UINT n : posted.ready) CHECK(n <= asked);
    }
    StopMkvSubtitles();
}

TEST(mkv_stop_is_prompt_and_final) {
    mkv::File f = Film(4000, {Text(2), Text(3)}, 250, 200);
    f.index = mkv::File::kNoIndex;
    const std::wstring path = Write(f);
    for (int round = 0; round < 5; ++round) {
        posted = {};
        REQUIRE(!StartMkvSubtitles(path, Window()).empty());
        Sleep(DWORD(round * 3));
        const double start = t::Now();
        StopMkvSubtitles();
        CHECK(t::Now() - start < 1.0);
        t::Pump(20);
        const size_t count = posted.cues[2].size() + posted.cues[3].size();
        t::Pump(100);
        CHECK_EQ(posted.cues[2].size() + posted.cues[3].size(), count);  // nothing after it
        CHECK(!SeekMkvSubtitles(1, 1));  // and none answered
    }
    StopMkvSubtitles();
}

TEST(mkv_starting_another_file_stops_the_first) {
    mkv::File a = Film(2000, {Text(2)}, 250, 200), b = Film(5, {Text(7)});
    a.index = mkv::File::kNoIndex;
    REQUIRE(!StartMkvSubtitles(Write(a, L"a.mkv"), Window()).empty());
    const std::vector<Track> tracks = ReadAll(Write(b, L"b.mkv"));
    REQUIRE_EQ(tracks.size(), size_t(1));
    CHECK_EQ(tracks[0].id, DWORD(7));
    CHECK(posted.cues.count(2) == 0);  // none of the first file's after it
    CHECK_EQ(posted.cues[7].size(), FilmCues(5, 1000));
}

TEST(mkv_key_frames_of_the_index) {
    mkv::File f = Film(10, {Text(2)});
    f.index = mkv::File::kVideoIndex;
    std::vector<LONGLONG> keys = MkvKeyFrames(Write(f).c_str());
    REQUIRE_EQ(keys.size(), size_t(10));
    for (size_t i = 0; i < keys.size(); ++i) CHECK_EQ(keys[i], LONGLONG(i) * 10'000'000);
    CHECK(std::is_sorted(keys.begin(), keys.end()));
    // Only the video's: an index of the subtitles alone gives none, and no index none.
    mkv::File subsOnly = Film(0, {});
    subsOnly.tracks.push_back(Text(2));
    subsOnly.blocks.push_back({2, 0, "x", 100});
    CHECK(MkvKeyFrames(Write(subsOnly, L"subs.mkv").c_str()).empty());
    f.index = mkv::File::kNoIndex;
    CHECK(MkvKeyFrames(Write(f, L"none.mkv").c_str()).empty());
}

TEST(mkv_audio_delays_in_track_order) {
    mkv::File f = Film(1, {});
    for (const UINT64 delay : {UINT64(6'500'000), UINT64(0), UINT64(80'000'000'000)}) {
        mkv::Track a;
        a.number = f.tracks.size() + 1;
        a.type = 2;
        a.codec = "A_OPUS";
        a.codecDelayNs = delay;
        f.tracks.push_back(a);
    }
    const std::vector<LONGLONG> delays = MkvAudioDelays(Write(f).c_str());
    REQUIRE_EQ(delays.size(), size_t(3));
    CHECK_EQ(delays[0], LONGLONG(65'000));
    CHECK_EQ(delays[1], LONGLONG(0));
    CHECK_EQ(delays[2], LONGLONG(10'000'000));  // 1 s at most
}

// --- Damaged files ---

TEST_TIMED(mkv_damaged_files_never_crash_or_hang, 180000) {
    // Fuzzing: each layout, damaged at random, through all three readers. A hang shows as the reader never
    // finishing; under AddressSanitizer a bad read stops the test.
    std::mt19937 rng(2024);
    int round = 0;
    for (const Layout& layout : kLayouts) {
        mkv::File f = Film(20, {Text(2), Text(3, "S_TEXT/ASS")}, 700, 900);
        f.tracks[2].strip = "0,0,Default,";
        for (mkv::Block& b : f.blocks) {
            if (b.track == 3) b.data = ",,0,0,0,,{\\an8}" + b.data;
        }
        layout.apply(f);
        const std::string valid = mkv::Build(f);
        for (int i = 0; i < 60; ++i, ++round) {
            std::string bytes = valid;
            for (int n = 1 + int(rng() % 6); n-- > 0;) {
                const size_t at = rng() % bytes.size();
                switch (rng() % 6) {
                    case 0: bytes[at] = char(rng()); break;
                    case 1: bytes[at] = "\x00\x01\xFF\x80\x7F"[rng() % 5]; break;
                    case 2: bytes.resize(at); break;
                    case 3: bytes.insert(at, std::string(1 + rng() % 16, char(rng()))); break;
                    case 4: bytes.erase(at, 1 + rng() % 64); break;
                    case 5: for (int k = 0; k < 8 && at + k < bytes.size(); ++k) bytes[at + k] = '\xFF'; break;
                }
                if (bytes.empty()) bytes = "\x1A";
            }
            NOTE(std::string(layout.name) + ", round " + std::to_string(round));
            const std::wstring path = t::WriteTemp(L"fuzz.mkv", bytes);
            MkvKeyFrames(path.c_str());
            MkvAudioDelays(path.c_str());
            for (const Track& t : ReadAll(path, 5000)) {
                for (const Cue& c : posted.cues[t.id]) {
                    CHECK(c.end > c.start);
                    CHECK(!c.text.empty());
                    CHECK(c.text.size() <= kMaxCueChars);
                }
            }
            if (rng() % 4 == 0) {  // and seeking about while it reads
                posted = {};
                if (!StartMkvSubtitles(path, Window()).empty()) {
                    for (UINT k = 1; k <= 3; ++k) SeekMkvSubtitles(double(rng() % 30), k);
                    t::Pump(int(rng() % 10));
                }
            }
            StopMkvSubtitles();
        }
    }
    t::Pump(0);
}
