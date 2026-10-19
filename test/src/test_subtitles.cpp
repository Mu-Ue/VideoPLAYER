// SRT reading, cue text and styles, and the cue lookup the frames are drawn with.

#include <algorithm>
#include <cmath>
#include <random>
#include <set>

#include "config.h"
#include "harness.h"
#include "media/subtitles.h"

namespace {

Cue Text(std::wstring_view raw) {
    Cue c{};
    SetCueText(raw, c);
    return c;
}

std::string Utf16Le(std::wstring_view w, bool bom = true) {
    std::string s = bom ? "\xFF\xFE" : "";
    for (wchar_t c : w) s += char(c & 0xFF), s += char(c >> 8);
    return s;
}

std::string Utf16Be(std::wstring_view w) {
    std::string s = "\xFE\xFF";
    for (wchar_t c : w) s += char(c >> 8), s += char(c & 0xFF);
    return s;
}

bool Load(const std::string& bytes, Subtitles& subs, const wchar_t* name = L"subs.srt") {
    return LoadSubtitles(t::WriteTemp(name, bytes), subs);
}

// The reach of each cue: the latest end of it and all before it.
bool ReachHolds(const Subtitles& s) {
    if (s.reach.size() != s.cues.size()) return false;
    double r = 0;
    for (size_t i = 0; i < s.cues.size(); ++i) {
        r = std::max(r, s.cues[i].end);
        if (s.reach[i] != r) return false;
    }
    return true;
}

bool Sorted(const Subtitles& s) {
    return std::is_sorted(s.cues.begin(), s.cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
}

}  // namespace

// --- SetCueText ---

TEST(cue_plain_text_is_kept) {
    const Cue c = Text(L"Hello, world");
    CHECK_EQ(c.text, std::wstring(L"Hello, world"));
    CHECK(c.spans.empty());
    CHECK(!c.top);
}

TEST(cue_italic_and_bold_become_spans) {
    const Cue c = Text(L"a<i>b</i>c<b>d</b>e");
    CHECK_EQ(c.text, std::wstring(L"abcde"));
    REQUIRE_EQ(c.spans.size(), size_t(2));
    CHECK(c.spans[0] == (StyleSpan{1, 1, true, false}));
    CHECK(c.spans[1] == (StyleSpan{3, 1, false, true}));
}

TEST(cue_nested_styles_combine) {
    const Cue c = Text(L"<i>x<b>y</b></i>z");
    CHECK_EQ(c.text, std::wstring(L"xyz"));
    REQUIRE_EQ(c.spans.size(), size_t(2));
    CHECK(c.spans[0] == (StyleSpan{0, 1, true, false}));
    CHECK(c.spans[1] == (StyleSpan{1, 1, true, true}));
}

TEST(cue_uppercase_tags_work) {
    const Cue c = Text(L"<I>x</I><B>y</B>");
    CHECK_EQ(c.text, std::wstring(L"xy"));
    CHECK_EQ(c.spans.size(), size_t(2));
}

TEST(cue_style_running_to_the_end_is_closed) {
    const Cue c = Text(L"<i>unclosed");
    CHECK_EQ(c.text, std::wstring(L"unclosed"));
    REQUIRE_EQ(c.spans.size(), size_t(1));
    CHECK(c.spans[0] == (StyleSpan{0, 8, true, false}));
}

TEST(cue_other_tags_are_dropped) {
    CHECK_EQ(Text(L"<font color=\"#ff0000\">red</font>").text, std::wstring(L"red"));
    CHECK_EQ(Text(L"<u>under</u>").text, std::wstring(L"under"));
    CHECK_EQ(Text(L"<c.yellow>vtt</c>").text, std::wstring(L"vtt"));
}

TEST(cue_text_that_only_looks_like_a_tag_stays) {
    CHECK_EQ(Text(L"I <3 you").text, std::wstring(L"I <3 you"));
    CHECK_EQ(Text(L"a < b > c").text, std::wstring(L"a < b > c"));
    CHECK_EQ(Text(L"x <").text, std::wstring(L"x <"));
    CHECK_EQ(Text(L"<i unclosed").text, std::wstring(L"<i unclosed"));
    CHECK_EQ(Text(L"{plain braces}").text, std::wstring(L"{plain braces}"));
    CHECK_EQ(Text(L"{").text, std::wstring(L"{"));
}

TEST(cue_overlong_tag_is_text) {
    const std::wstring tag = L"<" + std::wstring(200, L'a') + L">";
    CHECK_EQ(Text(tag + L"x").text, tag + L"x");
}

TEST(cue_ass_positions_put_it_at_the_top) {
    for (const wchar_t* code : {L"{\\an7}", L"{\\an8}", L"{\\an9}", L"{\\pos(1,2)\\an8}"}) {
        NOTE(t::Utf8(code));
        const Cue c = Text(std::wstring(code) + L"Top");
        CHECK(c.top);
        CHECK_EQ(c.text, std::wstring(L"Top"));
    }
    for (const wchar_t* code : {L"{\\an1}", L"{\\an2}", L"{\\an5}", L"{\\fad(100,100)}"}) {
        NOTE(t::Utf8(code));
        const Cue c = Text(std::wstring(code) + L"Bottom");
        CHECK(!c.top);
        CHECK_EQ(c.text, std::wstring(L"Bottom"));
    }
}

TEST(cue_ass_style_overrides) {
    const Cue c = Text(L"{\\i1}it{\\i0} {\\b1}bo{\\b0}");
    CHECK_EQ(c.text, std::wstring(L"it bo"));
    REQUIRE_EQ(c.spans.size(), size_t(2));
    CHECK(c.spans[0] == (StyleSpan{0, 2, true, false}));
    CHECK(c.spans[1] == (StyleSpan{3, 2, false, true}));
}

TEST(cue_ass_line_breaks) {
    CHECK_EQ(Text(L"one\\Ntwo\\nthree").text, std::wstring(L"one\ntwo\nthree"));
    CHECK_EQ(Text(L"back\\slash").text, std::wstring(L"back\\slash"));
}

TEST(cue_trailing_space_and_breaks_are_trimmed) {
    const Cue c = Text(L"<i>end   \n\n</i>");
    CHECK_EQ(c.text, std::wstring(L"end"));
    REQUIRE_EQ(c.spans.size(), size_t(1));
    CHECK_EQ(c.spans[0].length, UINT32(3));  // clipped to the text
}

TEST(cue_only_tags_is_empty) {
    CHECK(Text(L"<i></i>").text.empty());
    CHECK(Text(L"{\\an8}").text.empty());
    CHECK(Text(L"   ").text.empty());
}

TEST(cue_text_is_capped) {
    const std::wstring raw = L"<i>" + std::wstring(kMaxCueChars * 3, L'x') + L"</i>";
    const Cue c = Text(raw);
    CHECK(c.text.size() <= kMaxCueChars);
    CHECK(c.text.size() >= kMaxCueChars - 1);
    for (const StyleSpan& s : c.spans) CHECK(size_t(s.start) + s.length <= c.text.size());
}

TEST(cue_spans_always_lie_within_the_text) {
    // Random mixes of text, tags and overrides: whatever comes in, the spans stay in the text, in order and apart.
    std::mt19937 rng(7);
    const wchar_t* pieces[] = {L"<i>", L"</i>", L"<b>", L"</b>", L"{\\i1}", L"{\\i0}", L"{\\b1}", L"{\\b0}", L"{\\an8}",
                               L"a",   L"bc",   L" ",   L"\n",   L"\\N",    L"<",     L">",     L"{",     L"}",
                               L"<3",  L"\t"};
    for (int round = 0; round < 3000; ++round) {
        std::wstring raw;
        for (int n = int(rng() % 24); n-- > 0;) raw += pieces[rng() % std::size(pieces)];
        NOTE("raw: " + t::Escaped(raw));
        const Cue c = Text(raw);
        UINT32 previousEnd = 0;
        for (const StyleSpan& s : c.spans) {
            // A style that only covered trimmed line breaks is left empty, past the text, which styles nothing.
            if (s.length == 0) continue;
            CHECK(size_t(s.start) + s.length <= c.text.size());
            CHECK(s.start >= previousEnd);
            CHECK(s.italic || s.bold);
            previousEnd = s.start + s.length;
        }
        CHECK(c.text.empty() || (c.text.back() != L'\n' && c.text.back() != L' ' && c.text.back() != L'\t'));
    }
}

// --- LoadSubtitles ---

TEST(srt_basic_file) {
    Subtitles s;
    REQUIRE(Load("1\r\n00:00:01,000 --> 00:00:02,500\r\nHello\r\n\r\n"
                 "2\r\n00:00:03,000 --> 00:00:04,000\r\nTwo\r\nlines\r\n",
                 s));
    REQUIRE_EQ(s.cues.size(), size_t(2));
    CHECK_EQ(s.cues[0].start, 1.0);
    CHECK_EQ(s.cues[0].end, 2.5);
    CHECK_EQ(s.cues[0].text, std::wstring(L"Hello"));
    CHECK_EQ(s.cues[1].text, std::wstring(L"Two\nlines"));
    CHECK(ReachHolds(s));
}

TEST(srt_unix_line_ends_and_no_numbers) {
    Subtitles s;
    REQUIRE(Load("00:00:01,000 --> 00:00:02,000\nA\n00:00:03,000 --> 00:00:04,000\nB\n", s));
    REQUIRE_EQ(s.cues.size(), size_t(2));
    CHECK_EQ(s.cues[0].text, std::wstring(L"A"));
    CHECK_EQ(s.cues[1].text, std::wstring(L"B"));
}

TEST(srt_timing_forms) {
    struct {
        const char* timing;
        double start, end;
    } cases[] = {
        {"00:00:01,000 --> 00:00:02,000", 1, 2},
        {"00:00:01.250 --> 00:00:02.750", 1.25, 2.75},                 // WebVTT-style dot
        {"01:02:03,004 --> 01:02:04,000", 3723.004, 3724},             // hours
        {"02:03,500 --> 02:04,000", 123.5, 124},                       // no hours
        {"00:00:01,5 --> 00:00:02,25", 1.5, 2.25},                     // short fractions
        {"00:00:01 --> 00:00:02", 1, 2},                               // none
        {"  00:00:01,000   -->   00:00:02,000  ", 1, 2},               // spaces
        {"00:00:01,000-->00:00:02,000", 1, 2},                         // none
        {"00:00:01,000 --> 00:00:02,000 X1:10 X2:20 Y1:5 Y2:6", 1, 2},  // positions after it
        {"100:00:00,000 --> 100:00:01,000", 360000, 360001},           // long films
    };
    for (const auto& c : cases) {
        NOTE(c.timing);
        Subtitles s;
        REQUIRE(Load(std::string(c.timing) + "\ntext\n", s));
        REQUIRE_EQ(s.cues.size(), size_t(1));
        CHECK_NEAR(s.cues[0].start, c.start, 1e-9);
        CHECK_NEAR(s.cues[0].end, c.end, 1e-9);
    }
}

TEST(srt_bad_timings_are_text_or_dropped) {
    for (const char* timing : {"00:00:02,000 --> 00:00:01,000", "00:00:01,000 --> 00:00:01,000", "1 --> 2",
                               "00:00:01,000 -> 00:00:02,000", "aa:bb:cc --> 00:00:02,000"}) {
        NOTE(timing);
        Subtitles s;
        CHECK(!Load(std::string(timing) + "\ntext\n", s));
    }
}

TEST(srt_text_before_the_first_timing_is_ignored) {
    Subtitles s;
    REQUIRE(Load("WEBVTT\n\nNOTE a comment\n\n00:00:01.000 --> 00:00:02.000\nHi\n", s));
    REQUIRE_EQ(s.cues.size(), size_t(1));
    CHECK_EQ(s.cues[0].text, std::wstring(L"Hi"));
}

TEST(srt_cue_without_text_is_dropped) {
    Subtitles s;
    REQUIRE(Load("1\n00:00:01,000 --> 00:00:02,000\n\n2\n00:00:03,000 --> 00:00:04,000\nKept\n", s));
    REQUIRE_EQ(s.cues.size(), size_t(1));
    CHECK_EQ(s.cues[0].text, std::wstring(L"Kept"));
}

TEST(srt_arrow_in_text_stays_text) {
    Subtitles s;
    REQUIRE(Load("00:00:01,000 --> 00:00:02,000\nleft --> right\n", s));
    REQUIRE_EQ(s.cues.size(), size_t(1));
    CHECK_EQ(s.cues[0].text, std::wstring(L"left --> right"));
}

TEST(srt_blank_lines_with_spaces_end_a_cue) {
    Subtitles s;
    REQUIRE(Load("00:00:01,000 --> 00:00:02,000\nA\n \t \nnot part of it\n", s));
    REQUIRE_EQ(s.cues.size(), size_t(1));
    CHECK_EQ(s.cues[0].text, std::wstring(L"A"));
}

TEST(srt_out_of_order_cues_are_sorted) {
    Subtitles s;
    REQUIRE(Load("00:00:05,000 --> 00:00:06,000\nC\n\n00:00:01,000 --> 00:00:09,000\nA\n\n"
                 "00:00:03,000 --> 00:00:04,000\nB\n\n00:00:03,000 --> 00:00:03,500\nB2\n",
                 s));
    REQUIRE_EQ(s.cues.size(), size_t(4));
    CHECK(Sorted(s));
    CHECK_EQ(s.cues[0].text, std::wstring(L"A"));
    CHECK_EQ(s.cues[1].text, std::wstring(L"B"));  // stable: in file order at the same start
    CHECK_EQ(s.cues[2].text, std::wstring(L"B2"));
    CHECK(ReachHolds(s));
    CHECK_EQ(s.reach[3], 9.0);
}

TEST(srt_encodings) {
    const std::wstring text = L"00:00:01,000 --> 00:00:02,000\nGrüße – 日本語\n";
    const std::string utf8 = "00:00:01,000 --> 00:00:02,000\nGr\xC3\xBC\xC3\x9F" "e \xE2\x80\x93 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\n";
    const std::pair<const char*, std::string> files[] = {
        {"utf-8", utf8},
        {"utf-8 with BOM", "\xEF\xBB\xBF" + utf8},
        {"utf-16 LE", Utf16Le(text)},
        {"utf-16 BE", Utf16Be(text)},
    };
    for (const auto& [name, bytes] : files) {
        NOTE(name);
        Subtitles s;
        REQUIRE(Load(bytes, s));
        REQUIRE_EQ(s.cues.size(), size_t(1));
        CHECK_EQ(s.cues[0].text, std::wstring(L"Grüße – 日本語"));
    }
}

TEST(srt_ansi_fallback) {
    // Not valid UTF-8, so read in the ANSI code page.
    Subtitles s;
    REQUIRE(Load("00:00:01,000 --> 00:00:02,000\ncaf\xE9\n", s));
    REQUIRE_EQ(s.cues.size(), size_t(1));
    wchar_t expected[8] = {};
    MultiByteToWideChar(CP_ACP, 0, "caf\xE9", 4, expected, 8);
    CHECK_EQ(s.cues[0].text, std::wstring(expected));
    CHECK_EQ(s.cues[0].text.size(), size_t(4));
}

TEST(srt_styles_and_position_from_the_file) {
    Subtitles s;
    REQUIRE(Load("00:00:01,000 --> 00:00:02,000\n{\\an8}<i>Top italic</i>\n", s));
    REQUIRE_EQ(s.cues.size(), size_t(1));
    CHECK(s.cues[0].top);
    CHECK_EQ(s.cues[0].text, std::wstring(L"Top italic"));
    REQUIRE_EQ(s.cues[0].spans.size(), size_t(1));
}

TEST(srt_unreadable_files_fail) {
    Subtitles s;
    CHECK(!Load("", s));
    CHECK(!Load("just some text\nwith no timings\n", s));
    CHECK(!Load(std::string(4096, '\0'), s));
    CHECK(!LoadSubtitles(t::TempDir() + L"\\missing.srt", s));
    CHECK(!LoadSubtitles(t::TempDir(), s));  // a folder
    // Devices, which could block or never end, aren't read.
    CHECK(!LoadSubtitles(L"NUL", s));
    CHECK(!LoadSubtitles(L"\\\\.\\NUL", s));
    CHECK(!LoadSubtitles(L"CONIN$", s));
}

TEST(srt_failure_leaves_the_subtitles_as_they_were) {
    Subtitles s;
    REQUIRE(Load("00:00:01,000 --> 00:00:02,000\nKeep\n", s));
    CHECK(!Load("no cues", s, L"bad.srt"));
    REQUIRE_EQ(s.cues.size(), size_t(1));
    CHECK_EQ(s.cues[0].text, std::wstring(L"Keep"));
}

TEST(srt_files_over_the_size_cap_are_refused) {
    std::string big = "00:00:01,000 --> 00:00:02,000\nx\n";
    big.resize(size_t(kMaxSubtitleBytes) + 1, ' ');
    Subtitles s;
    CHECK(!Load(big, s));
    big.resize(size_t(kMaxSubtitleBytes));
    CHECK(Load(big, s));
}

TEST(srt_cue_count_is_capped) {
    std::string file;
    file.reserve((kMaxCues + 10) * 40);
    char line[64];
    for (size_t i = 0; i < kMaxCues + 10; ++i) {
        const size_t ms = i * 10;
        sprintf_s(line, "%02zu:%02zu:%02zu,%03zu --> %02zu:%02zu:%02zu,%03zu\nx\n\n", ms / 3600000, ms / 60000 % 60,
                  ms / 1000 % 60, ms % 1000, (ms + 5) / 3600000, (ms + 5) / 60000 % 60, (ms + 5) / 1000 % 60,
                  (ms + 5) % 1000);
        file += line;
    }
    Subtitles s;
    REQUIRE(Load(file, s));
    CHECK_EQ(s.cues.size(), kMaxCues);
    CHECK(ReachHolds(s));
}

TEST(srt_long_cue_text_is_capped) {
    std::string file = "00:00:01,000 --> 00:00:02,000\n";
    for (int i = 0; i < 2000; ++i) file += std::string(50, 'x') + "\n";
    Subtitles s;
    REQUIRE(Load(file, s));
    REQUIRE_EQ(s.cues.size(), size_t(1));
    CHECK(s.cues[0].text.size() <= kMaxCueChars);
}

TEST(srt_the_sample_file_loads) {
    Subtitles s;
    REQUIRE(LoadSubtitles(std::wstring(t::Media(L"..\\sample.srt")), s));
    CHECK(s.cues.size() >= 5);
    CHECK(Sorted(s));
    CHECK(ReachHolds(s));
    CHECK_EQ(s.cues[0].text, std::wstring(L"Welcome to VideoPLAYER."));
}

TEST_TIMED(srt_random_bytes_never_crash, 60000) {
    // Fuzzing: mutations of a valid file. Under AddressSanitizer a read out of bounds stops the test.
    const std::string valid =
        "1\r\n00:00:01,000 --> 00:00:02,000\r\n<i>Hello</i> {\\an8}there\r\n\r\n2\r\n00:01:00.5 --> 00:01:02\r\nB\\Nc\r\n";
    std::mt19937 rng(1234);
    for (int round = 0; round < 1500; ++round) {
        std::string bytes = round % 3 == 0 ? Utf16Le(std::wstring(valid.begin(), valid.end())) : valid;
        for (int n = 1 + int(rng() % 8); n-- > 0;) {
            const size_t at = rng() % (bytes.size() + 1);
            switch (rng() % 5) {
                case 0: if (at < bytes.size()) bytes[at] = char(rng()); break;
                case 1: bytes.insert(at, 1, "<>{}\\-:,.\n\r\xFF\xFE"[rng() % 14]); break;
                case 2: if (at < bytes.size()) bytes.erase(at, 1 + rng() % 4); break;
                case 3: bytes.resize(at); break;
                case 4: bytes.insert(at, bytes.substr(0, rng() % (bytes.size() + 1))); break;
            }
        }
        NOTE("round " + std::to_string(round));
        Subtitles s;
        if (Load(bytes, s, L"fuzz.srt")) {
            CHECK(!s.cues.empty());
            CHECK(Sorted(s));
            CHECK(ReachHolds(s));
            for (const Cue& c : s.cues) {
                CHECK(c.end > c.start);
                CHECK(!c.text.empty());
                CHECK(c.text.size() <= kMaxCueChars);
            }
        }
    }
}

// --- AddCues and ActiveCues ---

TEST(add_cues_merges_in_start_order) {
    Subtitles s;
    CHECK(AddCues(s, {{5, 6, L"c"}, {1, 2, L"a"}}));
    CHECK(AddCues(s, {{3, 4, L"b"}, {7, 8, L"d"}}));
    REQUIRE_EQ(s.cues.size(), size_t(4));
    CHECK(Sorted(s));
    CHECK(ReachHolds(s));
    CHECK_EQ(s.cues[1].text, std::wstring(L"b"));
}

TEST(add_cues_skips_ones_already_there) {
    Subtitles s;
    CHECK(AddCues(s, {{1, 2, L"a"}, {1, 2, L"a"}}));  // the same twice in one batch
    CHECK_EQ(s.cues.size(), size_t(1));
    CHECK(!AddCues(s, {{1, 2, L"a"}}));
    CHECK_EQ(s.cues.size(), size_t(1));
    // Differing in any of end, text or position makes another.
    CHECK(AddCues(s, {{1, 3, L"a"}}));
    CHECK(AddCues(s, {{1, 2, L"b"}}));
    Cue top{1, 2, L"a"};
    top.top = true;
    CHECK(AddCues(s, {top}));
    CHECK_EQ(s.cues.size(), size_t(4));
    CHECK(!AddCues(s, {}));
}

TEST(add_cues_keeps_arrival_order_at_the_same_start) {
    Subtitles s;
    AddCues(s, {{1, 2, L"first"}});
    AddCues(s, {{1, 3, L"second"}, {0.5, 9, L"earlier"}});
    REQUIRE_EQ(s.cues.size(), size_t(3));
    CHECK_EQ(s.cues[0].text, std::wstring(L"earlier"));
    CHECK_EQ(s.cues[1].text, std::wstring(L"first"));
    CHECK_EQ(s.cues[2].text, std::wstring(L"second"));
    CHECK(ReachHolds(s));
}

TEST(add_cues_random_batches_keep_every_invariant) {
    // As the Matroska reader posts cues: batches in any order around seeks. The result is always the set of
    // distinct cues, sorted, with each reach right.
    std::mt19937 rng(99);
    for (int round = 0; round < 300; ++round) {
        Subtitles s;
        std::set<std::tuple<double, double, std::wstring>> expected;
        for (int batch = int(rng() % 12); batch-- > 0;) {
            std::vector<Cue> added;
            for (int n = int(rng() % 10); n-- > 0;) {
                const double start = double(rng() % 40) / 4;
                Cue c{start, start + 0.25 * (1 + rng() % 12), std::wstring(1, wchar_t(L'a' + rng() % 3))};
                expected.insert({c.start, c.end, c.text});
                added.push_back(std::move(c));
            }
            const size_t before = s.cues.size();
            const bool grew = AddCues(s, added);
            CHECK_EQ(grew, s.cues.size() > before);
            CHECK(Sorted(s));
            CHECK(ReachHolds(s));
        }
        CHECK_EQ(s.cues.size(), expected.size());
    }
}

namespace {

// What ActiveCues must answer, the slow way.
double Oracle(const Subtitles& s, double t, std::vector<size_t>& active) {
    active.clear();
    double next = INFINITY;
    for (size_t i = 0; i < s.cues.size(); ++i) {
        const Cue& c = s.cues[i];
        if (c.start <= t && t < c.end) {
            active.push_back(i);
            next = std::min(next, c.end);
        }
        if (c.start > t) next = std::min(next, c.start);
    }
    return next;
}

}  // namespace

TEST(active_cues_edges) {
    Subtitles s;
    AddCues(s, {{1, 2, L"a"}, {1.5, 3, L"b"}});
    std::vector<size_t> active;
    CHECK_EQ(ActiveCues(s, 0.5, active), 1.0);
    CHECK(active.empty());
    CHECK_EQ(ActiveCues(s, 1.0, active), 1.5);  // shows from its start
    CHECK(active == std::vector<size_t>{0});
    CHECK_EQ(ActiveCues(s, 1.75, active), 2.0);
    CHECK((active == std::vector<size_t>{0, 1}));
    CHECK_EQ(ActiveCues(s, 2.0, active), 3.0);  // not at its end
    CHECK(active == std::vector<size_t>{1});
    CHECK(std::isinf(ActiveCues(s, 3.0, active)));
    CHECK(active.empty());
    Subtitles none;
    CHECK(std::isinf(ActiveCues(none, 1, active)));
    CHECK(active.empty());
}

TEST(active_cues_long_cue_under_short_ones) {
    // A cue lasting the whole film must show however many short ones start after it: the reach finds it.
    Subtitles s;
    std::vector<Cue> cues{{0, 1000, L"long"}};
    for (int i = 0; i < 500; ++i) cues.push_back({1.0 + i, 1.5 + i, L"short"});
    AddCues(s, cues);
    std::vector<size_t> active;
    ActiveCues(s, 300.25, active);
    CHECK((active == std::vector<size_t>{0, 300}));
    ActiveCues(s, 300.75, active);
    CHECK((active == std::vector<size_t>{0}));
}

TEST(active_cues_match_a_brute_force_lookup) {
    std::mt19937 rng(5);
    for (int round = 0; round < 200; ++round) {
        Subtitles s;
        std::vector<Cue> cues;
        for (int n = int(rng() % 30); n-- > 0;) {
            const double start = double(rng() % 100) / 8;
            cues.push_back({start, start + double(1 + rng() % 40) / 8, L"x" + std::to_wstring(n)});
        }
        AddCues(s, cues);
        for (int probe = 0; probe < 60; ++probe) {
            // On the grid the times are on, where edges are, and between.
            const double time = probe % 2 ? double(rng() % 140) / 8 : double(rng() % 14000) / 1000 - 0.5;
            NOTE("round " + std::to_string(round) + " t=" + std::to_string(time));
            std::vector<size_t> got, want;
            const double next = ActiveCues(s, time, got), expected = Oracle(s, time, want);
            CHECK(got == want);
            CHECK_EQ(next, expected);
        }
    }
}
