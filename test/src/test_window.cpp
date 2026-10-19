// The window: its helpers, and the player as a user drives it, with keys, the mouse, dropped files and the media
// keys. This file compiles the window and the entry point itself, so the tests reach their insides; what they look
// at of the window's state is in the probes below, so a rename inside it means one place to change here.

#include "ui/window.cpp"
#include "main.cpp"

#include <shlobj.h>  // DROPFILES

#include <filesystem>

#include "fixture.h"
#include "harness.h"

namespace {

// --- Probes into the window's state ---

HWND Hwnd() { return g.hwnd; }
bool Playing() { return g.playing; }
double Rate() { return g.rate; }
double WindowVolume() { return g.volume; }
bool WindowMuted() { return g.muted; }
bool SeekSettled() { return !g.seekBusy; }
int ShownSubtitle() { return g.subtitle; }
size_t SubtitleCount() { return g.tracks.size(); }
std::wstring SubtitleLabel(size_t i) { return g.tracks[i].label; }
size_t ReadSubtitleCues(size_t i) { return g.tracks[i].subs.cues.size(); }
std::vector<std::wstring> AudioLabels() { return g.audio; }
int AudioRow() { return g.audioTrack; }
bool PanelOpen() { return g.panelOpen; }
bool Fullscreen() { return g.fullscreen; }
std::wstring OsdText() { return g.osdText; }
double WindowDuration() { return g.duration; }
std::wstring Title() { return g.title; }

// The localized name Windows gives a locale, as the panel shows it.
std::wstring LocaleName(const wchar_t* locale) {
    wchar_t name[128] = L"";
    GetLocaleInfoEx(locale, LOCALE_SLOCALIZEDDISPLAYNAME, name, 128);
    return name;
}

// --- Driving it ---

// Starts the player with files as wWinMain does, its sound silent, and waits until a video plays.
void Start(const std::vector<std::wstring>& files, bool video = true) {
    // As wWinMain does, which also keeps out the overlays some tools inject into every window.
    HardenProcess();
    HardenPlayer();
    fx::Silence();
    REQUIRE(StartPlayer(GetModuleHandleW(nullptr), files, SW_SHOWNOACTIVATE));
    if (!video) {
        REQUIRE(t::PumpUntil([] { return IsWindowVisible(Hwnd()) != 0; }, 5000));
        return;
    }
    REQUIRE(t::PumpUntil(
        [] { return IsWindowVisible(Hwnd()) && std::isfinite(WindowDuration()) && FrameTime() >= 0 && Playing(); },
        10000));
}

void Start(const std::wstring& file) { Start(std::vector<std::wstring>{file}); }

// Waits for a file the window opened to load and play.
void WaitLoaded() {
    REQUIRE(t::PumpUntil([] { return std::isfinite(WindowDuration()) && IsPlaying(); }, 10000));
}

// A key pressed and released, with Ctrl or Shift held if asked; `repeat` as when held down.
void Key(UINT vk, bool ctrl = false, bool shift = false, bool repeat = false) {
    BYTE keys[256] = {};
    if (ctrl) keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
    if (shift) keys[VK_SHIFT] = keys[VK_LSHIFT] = 0x80;
    SetKeyboardState(keys);
    SendMessageW(Hwnd(), WM_KEYDOWN, vk, repeat ? LPARAM(1 | (1 << 30)) : 1);
    SendMessageW(Hwnd(), WM_KEYUP, vk, LPARAM(0xC0000001));
    BYTE none[256] = {};
    SetKeyboardState(none);
}

void Settle() {
    REQUIRE(t::PumpUntil(SeekSettled, 10000));
    t::Pump(100);
}

void Pause() {
    if (Playing()) Key(VK_SPACE);
    t::PumpUntil([] { return !IsPlaying(); }, 3000);
}

// Drops files on the window, as Explorer does.
void Drop(const std::vector<std::wstring>& files) {
    size_t chars = 1;
    for (const auto& f : files) chars += f.size() + 1;
    const HGLOBAL mem = GlobalAlloc(GHND, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    auto* drop = static_cast<DROPFILES*>(GlobalLock(mem));
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    auto* p = reinterpret_cast<wchar_t*>(drop + 1);
    for (const auto& f : files) {
        wcscpy_s(p, f.size() + 1, f.c_str());
        p += f.size() + 1;
    }
    GlobalUnlock(mem);
    SendMessageW(Hwnd(), WM_DROPFILES, WPARAM(mem), 0);  // the window frees it
}

LPARAM At(POINT p) { return MAKELPARAM(p.x, p.y); }

// A folder of its own with a copy of a test video, for files next to it.
std::wstring CopyMedia(const wchar_t* file, const std::wstring& as) {
    const std::wstring to = t::TempDir() + L"\\" + as;
    REQUIRE(CopyFileW(t::Media(file).c_str(), to.c_str(), FALSE));
    return to;
}

void WriteText(const std::wstring& path, const std::string& text) {
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) t::Fail(__FILE__, __LINE__, "can't write " + t::Utf8(path) + ": " + std::to_string(GetLastError()));
    REQUIRE(f != INVALID_HANDLE_VALUE);
    DWORD written;
    WriteFile(f, text.data(), DWORD(text.size()), &written, nullptr);
    CloseHandle(f);
}

const char kSrt[] = "1\r\n00:00:00,500 --> 00:00:05,000\r\nHello\r\n";

}  // namespace

// --- Helpers, without a window ---

TEST(window_time_format) {
    CHECK_EQ(FormatTime(0), std::wstring(L"0:00"));
    CHECK_EQ(FormatTime(59.99), std::wstring(L"0:59"));
    CHECK_EQ(FormatTime(60), std::wstring(L"1:00"));
    CHECK_EQ(FormatTime(605.5), std::wstring(L"10:05"));
    CHECK_EQ(FormatTime(3599.9), std::wstring(L"59:59"));
    CHECK_EQ(FormatTime(3600), std::wstring(L"1:00:00"));
    CHECK_EQ(FormatTime(3909), std::wstring(L"1:05:09"));
    CHECK_EQ(FormatTime(36000 * 10), std::wstring(L"100:00:00"));
    CHECK_EQ(FormatTime(-3), std::wstring(L"0:00"));
    CHECK_EQ(FormatTime(NAN), std::wstring(L"0:00"));
    CHECK_EQ(FormatTime(INFINITY), std::wstring(L"0:00"));
}

TEST(window_rate_format) {
    CHECK_EQ(FormatRate(1), std::wstring(L"1×"));
    CHECK_EQ(FormatRate(1.25), std::wstring(L"1.25×"));
    CHECK_EQ(FormatRate(0.5), std::wstring(L"0.5×"));
    CHECK_EQ(FormatRate(2), std::wstring(L"2×"));
}

TEST(window_language_names) {
    const std::wstring english = LocaleName(L"en"), german = LocaleName(L"de"), french = LocaleName(L"fr");
    REQUIRE(!english.empty());
    CHECK_EQ(LanguageName(L"en"), english);
    CHECK_EQ(LanguageName(L"eng"), english);
    CHECK_EQ(LanguageName(L"ENG"), english);
    CHECK_EQ(LanguageName(L"deu"), german);
    CHECK_EQ(LanguageName(L"ger"), german);  // the bibliographic code
    CHECK_EQ(LanguageName(L"fre"), french);
    CHECK_EQ(LanguageName(L"en-US"), LocaleName(L"en-US"));
    CHECK_EQ(LanguageName(L"pt-BR"), LocaleName(L"pt-BR"));
    for (const wchar_t* none : {L"", L"e", L"und", L"mul", L"zxx", L"mis", L"qqq", L"xx", L"not-a-tag-at-all-x", L"123"}) {
        NOTE(t::Utf8(none));
        CHECK(LanguageName(none).empty());
    }
}

TEST(window_track_labels) {
    const std::wstring english = LocaleName(L"en");
    CHECK_EQ(TrackLabel({L"eng", L""}), english);
    CHECK_EQ(TrackLabel({L"eng", L"Commentary"}), english + L" \x2013 Commentary");
    CHECK_EQ(TrackLabel({L"", L"Commentary"}), std::wstring(L"Commentary"));
    CHECK_EQ(TrackLabel({L"und", L"Signs"}), std::wstring(L"Signs"));
    CHECK_EQ(TrackLabel({L"", L""}), std::wstring(L"Unknown"));
    CHECK_EQ(TrackLabel({L"qqq", L""}), std::wstring(L"Unknown"));
    // A name that is just the language isn't said twice.
    std::wstring upper = english;
    CharUpperBuffW(upper.data(), DWORD(upper.size()));
    CHECK_EQ(TrackLabel({L"eng", upper}), english);
}

TEST(window_subtitle_file_labels) {
    CHECK_EQ(SubtitleFileLabel(L"C:\\films\\Movie.en.srt"), LocaleName(L"en"));
    CHECK_EQ(SubtitleFileLabel(L"C:\\films\\Movie.ger.srt"), LocaleName(L"de"));
    CHECK_EQ(SubtitleFileLabel(L"C:\\films\\Movie.de-AT.srt"), LocaleName(L"de-AT"));
    CHECK_EQ(SubtitleFileLabel(L"C:\\films\\Movie.srt"), std::wstring(L"Movie"));
    CHECK_EQ(SubtitleFileLabel(L"C:/films/Movie.forced.srt"), std::wstring(L"Movie.forced"));
    CHECK_EQ(SubtitleFileLabel(L"C:\\film.s\\Movie"), std::wstring(L"Movie"));
    // Short names, which once cut 4 characters off a shorter name and crashed.
    CHECK_EQ(SubtitleFileLabel(L"C:\\ab"), std::wstring(L"ab"));
    CHECK_EQ(SubtitleFileLabel(L"a"), std::wstring(L"a"));
    CHECK_EQ(SubtitleFileLabel(L".srt"), std::wstring(L""));
    CHECK_EQ(SubtitleFileLabel(L""), std::wstring(L""));
}

TEST(window_subtitle_files_by_extension) {
    CHECK(IsSubtitleFile(L"a.srt"));
    CHECK(IsSubtitleFile(L"C:\\x\\B.SRT"));
    CHECK(IsSubtitleFile(L".srt"));
    CHECK(!IsSubtitleFile(L"srt"));
    CHECK(!IsSubtitleFile(L"a.srtx"));
    CHECK(!IsSubtitleFile(L"a.mp4"));
    CHECK(!IsSubtitleFile(L""));
}

TEST(window_fit_keeps_the_aspect) {
    const SIZE a = Fit({1920, 1080}, 960, 2000);
    CHECK_EQ(a.cx, LONG(960));
    CHECK_EQ(a.cy, LONG(540));
    const SIZE b = Fit({1920, 1080}, 4000, 540);
    CHECK_EQ(b.cx, LONG(960));
    CHECK_EQ(b.cy, LONG(540));
    const SIZE c = Fit({180, 320}, 1000, 1000);
    CHECK_EQ(c.cx, LONG(563));
    CHECK_EQ(c.cy, LONG(1000));
    const SIZE d = Fit({10000, 10}, 100, 100);  // never 0 wide or high
    CHECK_EQ(d.cx, LONG(100));
    CHECK_EQ(d.cy, LONG(1));
}

TEST(window_command_line) {
    using V = std::vector<std::wstring>;
    CHECK(ParseArgs(nullptr).empty());
    CHECK(ParseArgs(L"").empty());
    CHECK(ParseArgs(L"  \t ").empty());
    CHECK((ParseArgs(L"--register") == V{L"--register"}));
    CHECK((ParseArgs(L"  --unregister  ") == V{L"--unregister"}));
    CHECK((ParseArgs(L"\"C:\\a b\\film.mp4\" \"C:\\a b\\film.srt\"") == V{L"C:\\a b\\film.mp4", L"C:\\a b\\film.srt"}));
    CHECK((ParseArgs(L"a.mp4\tb.srt") == V{L"a.mp4", L"b.srt"}));
    CHECK((ParseArgs(L"\"unclosed quote") == V{L"unclosed quote"}));
    CHECK((ParseArgs(L"\"\"") == V{L""}));
    // An unquoted line naming a file that exists is that one path, spaces and all.
    const std::wstring spaced = t::TempDir() + L"\\a film with spaces.mp4";
    WriteText(spaced, "x");
    CHECK((ParseArgs(spaced.c_str()) == V{spaced}));
    CHECK((ParseArgs((L"  " + spaced + L"  ").c_str()) == V{spaced}));
}

TEST(window_registered_types_match_the_open_dialog) {
    // config.h asks to keep them in step.
    const std::wstring filter(kFileFilter, std::size(kFileFilter));
    const size_t videos = filter.find(L"Videos\0", 1, 7);
    REQUIRE(videos != std::wstring::npos);
    const std::wstring patterns = filter.substr(videos + 7, filter.find(L'\0', videos + 7) - videos - 7);
    std::wstring registered;
    for (const wchar_t* ext : kVideoExtensions) registered += (registered.empty() ? L"*" : L";*") + std::wstring(ext);
    CHECK_EQ(patterns, registered);
    CHECK(filter.find(registered + L";*.srt") != std::wstring::npos);  // all of them and subtitles, first
}

// --- The window, driven ---

CORE_TEST(window_opens_at_the_video_size, 30000) {
    Start(t::Media(L"clip30.mp4"));
    wchar_t text[64];
    GetWindowTextW(Hwnd(), text, 64);
    CHECK_EQ(std::wstring(text), std::wstring(L"clip30.mp4"));
    CHECK_EQ(Title(), std::wstring(L"clip30"));
    RECT client;
    GetClientRect(Hwnd(), &client);
    const int bar = Px(26);
    // Its client area is the video, 16:9, below the menu bar.
    CHECK_NEAR(double(client.right) / (client.bottom - bar), 16.0 / 9, 0.02);
    CHECK_EQ(g.videoRect.top, LONG(bar));
    CHECK_EQ(g.videoRect.right - g.videoRect.left, client.right);
    CHECK(client.right >= Px(320));
    CHECK_NEAR(WindowDuration(), 6.0, 0.05);
}

CORE_TEST(window_play_pause_by_key_click_and_media_key, 30000) {
    Start(t::Media(L"clip30.mp4"));
    Key(VK_SPACE);
    CHECK(!Playing());
    CHECK(t::PumpUntil([] { return !IsPlaying(); }, 3000));
    const double held = PlaybackTime();
    t::Pump(300);
    CHECK_NEAR(PlaybackTime(), held, 0.001);
    Key(VK_SPACE);
    CHECK(Playing());
    CHECK(t::PumpUntil([&] { return PlaybackTime() > held + 0.1; }, 3000));
    // Media keys.
    SendMessageW(Hwnd(), WM_APPCOMMAND, WPARAM(Hwnd()), MAKELPARAM(0, APPCOMMAND_MEDIA_PLAY_PAUSE));
    CHECK(!Playing());
    SendMessageW(Hwnd(), WM_APPCOMMAND, WPARAM(Hwnd()), MAKELPARAM(0, APPCOMMAND_MEDIA_PAUSE));
    CHECK(!Playing());  // pause while paused stays paused
    SendMessageW(Hwnd(), WM_APPCOMMAND, WPARAM(Hwnd()), MAKELPARAM(0, APPCOMMAND_MEDIA_PLAY));
    CHECK(Playing());
    SendMessageW(Hwnd(), WM_APPCOMMAND, WPARAM(Hwnd()), MAKELPARAM(0, APPCOMMAND_MEDIA_STOP));
    CHECK(!Playing());
    // A click on the video toggles too.
    const POINT middle{(g.videoRect.left + g.videoRect.right) / 2, (g.videoRect.top + g.videoRect.bottom) / 2};
    SetControlsShown(false);
    SendMessageW(Hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, At(middle));
    SendMessageW(Hwnd(), WM_LBUTTONUP, 0, At(middle));
    CHECK(Playing());
}

CORE_TEST(window_arrow_keys_seek, 40000) {
    Start(t::Media(L"gop5s_nob.mp4"));  // key frames at 0, 5 and 10 s
    Pause();
    Key(VK_RIGHT);
    Settle();
    CHECK_NEAR(PlaybackTime(), 5.0, 0.01);  // to the key frame nearest 5 s on
    CHECK(OsdText().find(L"/ 0:12") != std::wstring::npos);
    Key(VK_RIGHT);
    Settle();
    CHECK_NEAR(PlaybackTime(), 10.0, 0.01);
    Key(VK_LEFT);
    Settle();
    CHECK_NEAR(PlaybackTime(), 5.0, 0.01);
    Key(VK_RIGHT, true);  // a minute on, past the end: to the last key frame
    Settle();
    CHECK_NEAR(PlaybackTime(), 10.0, 0.01);
    Key(VK_LEFT, true);
    Settle();
    CHECK_NEAR(PlaybackTime(), 0.0, 0.01);
    // Mouse side buttons step too.
    SendMessageW(Hwnd(), WM_APPCOMMAND, WPARAM(Hwnd()), MAKELPARAM(0, APPCOMMAND_BROWSER_FORWARD));
    Settle();
    CHECK_NEAR(PlaybackTime(), 5.0, 0.01);
    // Home and End go exactly there.
    Key(VK_HOME);
    Settle();
    CHECK_NEAR(PlaybackTime(), 0.0, 0.002);
    Key(VK_SPACE);
    Key(VK_END);
    CHECK(t::PumpUntil(HasEnded, 5000));
    CHECK(t::PumpUntil([] { return !Playing(); }, 3000));
}

CORE_TEST(window_steps_never_go_the_wrong_way, 40000) {
    // Key frames at 0 and 20 s: 5 s on from 3 s, the nearest key frame is behind, so the step is redone exactly.
    Start(t::Media(L"gop20s.mp4"));
    Pause();
    Key(VK_HOME);
    Settle();
    JumpTo(3.0);
    Settle();
    Key(VK_RIGHT);
    Settle();
    CHECK_NEAR(PlaybackTime(), 8.0, 0.01);
    // From 9 s, the nearest to 14 s is the one at 20 s, which is on: it goes there.
    JumpTo(9.0);
    Settle();
    Key(VK_RIGHT);
    Settle();
    CHECK_NEAR(PlaybackTime(), 20.0, 0.01);
    JumpTo(17.0);
    Settle();
    Key(VK_LEFT);
    Settle();
    CHECK_NEAR(PlaybackTime(), 12.0, 0.01);
}

CORE_TEST(window_held_arrows_dont_pile_up_seeks, 40000) {
    Start(t::Media(L"gop5s_nob.mp4"));
    Pause();
    for (int i = 0; i < 60; ++i) Key(VK_RIGHT, false, false, i > 0);
    // Each waits for the one in flight, replacing the one waiting: they end at once, at the end's key frame.
    const double start = t::Now();
    Settle();
    CHECK(t::Now() - start < 3);
    // As far on as it goes: the last key frame, or the end if the last step was redone exactly, which depends on
    // whether it came while another was in flight.
    CHECK(PlaybackTime() > 9.99);
    for (int i = 0; i < 60; ++i) Key(VK_LEFT, false, false, i > 0);
    Settle();
    CHECK_NEAR(PlaybackTime(), 0.0, 0.01);
}

CORE_TEST(window_frame_steps_right_after_a_seek, 30000) {
    // Paused at once, before two frames in a row have shown, then a seek: the shortest gap between frames the
    // player has seen is the one across the seek, and a step goes that far.
    Start(t::Media(L"clip30.mp4"));
    Pause();
    JumpTo(2.0);
    Settle();
    t::PumpUntil([] { return std::abs(FrameTime() - 2.0) < 0.002; }, 3000);
    Key(VK_OEM_PERIOD);
    Settle();
    t::PumpUntil([] { return std::abs(FrameTime() - 2.0) > 0.002; }, 3000);
    CHECK_NEAR(FrameTime(), 61 / 30.0, 0.002);
    CHECK_NEAR(FrameGap(), 1 / 30.0, 0.001);
}

CORE_TEST(window_frame_steps, 40000) {
    Start(t::Media(L"clip30.mp4"));
    CHECK(fx::WaitFrameAt(0.3, 5000));  // frames in a row, which tell the time between them
    Pause();
    JumpTo(2.0);
    Settle();
    REQUIRE(t::PumpUntil([] { return std::abs(FrameTime() - 2.0) < 0.002; }, 3000));
    // '.' pauses, if playing, and shows the next frame; ',' the one before.
    for (int i = 1; i <= 5; ++i) {
        NOTE("step " + std::to_string(i));
        Key(VK_OEM_PERIOD);
        Settle();
        CHECK(!Playing());
        const double want = (60 + i) / 30.0;
        t::PumpUntil([&] { return std::abs(FrameTime() - want) < 0.002; }, 3000);
        CHECK_NEAR(FrameTime(), want, 0.002);
        CHECK_NEAR(PlaybackTime(), want, 1 / 30.0);
    }
    for (int i = 4; i >= 0; --i) {
        Key(VK_OEM_COMMA);
        Settle();
        const double want = (60 + i) / 30.0;
        CHECK(t::PumpUntil([&] { return std::abs(FrameTime() - want) < 0.002; }, 3000));
    }
    // Held, the steps go on from the one in flight.
    for (int i = 0; i < 10; ++i) Key(VK_OEM_PERIOD, false, false, i > 0);
    Settle();
    CHECK(t::PumpUntil([] { return std::abs(FrameTime() - 70 / 30.0) < 0.002; }, 3000));
}

CORE_TEST(window_volume_keys_wheel_and_mute, 30000) {
    Start(t::Media(L"clip30.mp4"));
    CHECK_EQ(WindowVolume(), 1.0);
    Key(VK_UP);
    CHECK_NEAR(WindowVolume(), 1.05, 1e-9);
    CHECK_NEAR(Volume(), 1.05, 1e-9);
    CHECK(OsdText() == L"105%");
    Key(VK_DOWN);
    Key(VK_DOWN);
    CHECK_NEAR(WindowVolume(), 0.95, 1e-9);
    for (int i = 0; i < 30; ++i) Key(VK_UP, false, false, true);
    CHECK_EQ(WindowVolume(), kMaxVolume);
    // A notch of the wheel is a step; a touchpad's smaller steps add up to one, and turning back starts over.
    POINT center{(g.videoRect.left + g.videoRect.right) / 2, (g.videoRect.top + g.videoRect.bottom) / 2};
    ClientToScreen(Hwnd(), &center);
    SetVolume(1);
    g.volume = 1;
    SendMessageW(Hwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), At(center));
    CHECK_NEAR(WindowVolume(), 0.95, 1e-9);
    for (int i = 0; i < 2; ++i) SendMessageW(Hwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, 40), At(center));
    CHECK_NEAR(WindowVolume(), 0.95, 1e-9);
    SendMessageW(Hwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, 40), At(center));
    CHECK_NEAR(WindowVolume(), 1.0, 1e-9);
    SendMessageW(Hwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, 80), At(center));
    SendMessageW(Hwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, -80), At(center));  // back the other way: starts over
    CHECK_NEAR(WindowVolume(), 1.0, 1e-9);
    // M mutes and unmutes; turning it up unmutes.
    Key('M');
    CHECK(WindowMuted());
    CHECK(IsMuted());
    Key('M');
    CHECK(!WindowMuted());
    Key('M');
    Key(VK_UP);
    CHECK(!WindowMuted());
    // At 0% M brings back the volume from before it went down.
    SetVolume(0.4);
    g.volume = 0.4;
    for (int i = 0; i < 10; ++i) Key(VK_DOWN, false, false, true);
    CHECK_EQ(WindowVolume(), 0.0);
    Key('M');
    CHECK_NEAR(WindowVolume(), 0.05, 1e-9);  // the last step down started at 5%
    CHECK(!WindowMuted());
}

CORE_TEST(window_keeps_the_volume_and_position_when_closed, 30000) {
    const Settings saved{0.4, true};
    SaveSettings(saved);
    Start(t::Media(L"clip30.mp4"));
    CHECK_NEAR(Volume(), 0.4, 1e-9);  // as the last run left it
    CHECK(IsMuted());
    Key('M');
    Key(VK_UP);
    Pause();
    JumpTo(3.25);
    Settle();
    SendMessageW(Hwnd(), WM_CLOSE, 0, 0);
    CHECK(!IsWindow(Hwnd()));
    const Settings now = LoadSettings();
    CHECK_NEAR(now.volume, 0.45, 1e-9);
    CHECK(!now.muted);
    CHECK_NEAR(LoadPosition(t::Media(L"clip30.mp4")), 3.25, 0.05);
}

CORE_TEST(window_resumes_where_each_video_was_left, 40000) {
    const std::wstring a = t::Media(L"clip30.mp4"), b = t::Media(L"gop5s_nob.mp4");
    SavePosition(a, 4.0);
    Start(a);
    CHECK(t::PumpUntil([] { return PlaybackTime() >= 4.0; }, 5000));  // where the last run left it
    Pause();
    JumpTo(2.5);
    Settle();
    CHECK(OpenFiles({b}));
    WaitLoaded();
    CHECK(PlaybackTime() < 1.0);
    CHECK_NEAR(LoadPosition(a), 2.5, 0.05);
    CHECK(OpenFiles({a}));
    WaitLoaded();
    CHECK(t::PumpUntil([] { return PlaybackTime() >= 2.5 && PlaybackTime() < 3.5; }, 5000));
    // One watched to the end starts over next time.
    Key(VK_END);
    CHECK(t::PumpUntil(HasEnded, 5000));
    CHECK(OpenFiles({b}));
    WaitLoaded();
    CHECK_EQ(LoadPosition(a), 0.0);
}

CORE_TEST(window_speed_keys, 30000) {
    Start(t::Media(L"gop5s_nob.mp4"));
    Key(VK_OEM_PERIOD, false, true);
    CHECK_EQ(Rate(), 1.25);
    CHECK(OsdText() == L"1.25×");
    for (int i = 0; i < 10; ++i) Key(VK_OEM_PERIOD, false, true, true);
    CHECK_EQ(Rate(), kMaxRate);
    t::Pump(500);
    const double a = PlaybackTime(), start = t::Now();
    t::Pump(1000);
    CHECK_NEAR((PlaybackTime() - a) / (t::Now() - start), 2.0, 0.4);
    for (int i = 0; i < 10; ++i) Key(VK_OEM_COMMA, false, true, true);
    CHECK_EQ(Rate(), kMinRate);
    Press(kSpeed);  // the speed button goes back to normal
    CHECK_EQ(Rate(), 1.0);
    // Each video starts at normal speed.
    Key(VK_OEM_PERIOD, false, true);
    CHECK(OpenFiles({t::Media(L"clip30.mp4")}));
    WaitLoaded();
    CHECK_EQ(Rate(), 1.0);
}

CORE_TEST(window_subtitle_tracks_of_the_video, 30000) {
    Start(t::Media(L"tracks.mkv"));
    REQUIRE_EQ(SubtitleCount(), size_t(3));
    CHECK_EQ(SubtitleLabel(0), LocaleName(L"en"));
    CHECK_EQ(SubtitleLabel(1), LocaleName(L"de"));
    CHECK_EQ(SubtitleLabel(2), LocaleName(L"ja") + L" \x2013 Styled");
    // They start hidden, with a hint.
    CHECK_EQ(ShownSubtitle(), -1);
    CHECK(OsdText() == L"Subtitles available (S)");
    // The reader brings their cues in.
    CHECK(t::PumpUntil([] { return ReadSubtitleCues(0) == 3 && ReadSubtitleCues(1) == 3 && ReadSubtitleCues(2) == 3; }, 5000));
    Key('S');
    CHECK_EQ(ShownSubtitle(), 0);
    Key('S');
    CHECK_EQ(ShownSubtitle(), -1);
    // The panel picks another; S then brings back the last one shown.
    Press(kSubtitles);
    CHECK(PanelOpen());
    PickTrack(1, 2);
    CHECK_EQ(ShownSubtitle(), 1);
    PickTrack(1, 0);
    CHECK_EQ(ShownSubtitle(), -1);
    Key(VK_ESCAPE);  // closes the panel, not the window
    CHECK(!PanelOpen());
    CHECK(IsWindow(Hwnd()));
    Key('S');
    CHECK_EQ(ShownSubtitle(), 1);
    // A seek with a track the reader reads waits for its cues there, and lands.
    Pause();
    JumpTo(4.5);
    Settle();
    CHECK_NEAR(PlaybackTime(), 4.5, 0.05);
}

CORE_TEST(window_audio_tracks_in_the_panel, 30000) {
    Start(t::Media(L"tracks.mkv"));
    REQUIRE(t::PumpUntil([] { return AudioLabels().size() == 2; }, 5000));
    CHECK_EQ(AudioLabels()[0], LocaleName(L"en"));
    CHECK_EQ(AudioLabels()[1], LocaleName(L"fr") + L" \x2013 Commentary");
    CHECK_EQ(AudioRow(), 0);
    JumpTo(2.0);
    Settle();
    Press(kSubtitles);
    REQUIRE(PanelOpen());
    PickTrack(0, 1);
    CHECK_EQ(AudioRow(), 1);
    CHECK(AudioTracks()[1].selected);
    Settle();
    // Switching seeks to have the source read the other track, staying where it was.
    CHECK_NEAR(PlaybackTime(), 2.0, 0.3);
}

CORE_TEST(window_subtitle_files_next_to_the_video, 30000) {
    const std::wstring video = CopyMedia(L"clip30.mp4", L"movie.mp4");
    const std::wstring dir = t::TempDir() + L"\\";
    WriteText(dir + L"movie.en.srt", kSrt);
    WriteText(dir + L"movie.de.srt", kSrt);
    WriteText(dir + L"movie.srt", kSrt);
    WriteText(dir + L"movie.broken.srt", "no cues here");
    WriteText(dir + L"other.srt", kSrt);
    WriteText(dir + L"movie.srt.txt", kSrt);
    Start(video);
    REQUIRE_EQ(SubtitleCount(), size_t(3));  // the readable ones whose names start with the video's
    std::vector<std::wstring> labels;
    for (size_t i = 0; i < SubtitleCount(); ++i) labels.push_back(SubtitleLabel(i));
    std::sort(labels.begin(), labels.end());
    std::vector<std::wstring> want{LocaleName(L"en"), LocaleName(L"de"), L"movie"};
    std::sort(want.begin(), want.end());
    CHECK(labels == want);
    CHECK_EQ(ShownSubtitle(), -1);  // found, but hidden
}

CORE_TEST(window_dropped_and_added_subtitles, 30000) {
    Start(t::Media(L"clip30.mp4"));
    const std::wstring srt = t::TempDir() + L"\\dropped.en.srt";
    WriteText(srt, kSrt);
    Drop({srt});
    REQUIRE_EQ(SubtitleCount(), size_t(1));
    CHECK_EQ(ShownSubtitle(), 0);  // a file added is shown
    CHECK(OsdText() == L"Subtitles: " + LocaleName(L"en"));
    Drop({srt});  // the same again: not twice
    CHECK_EQ(SubtitleCount(), size_t(1));
    // One that can't be read says so, with its name, whatever the name.
    for (const wchar_t* name : {L"broken.srt", L"ab", L"x"}) {
        NOTE(t::Utf8(name));
        const std::wstring bad = t::TempDir() + L"\\" + name;
        WriteText(bad, "not subtitles");
        LoadSubtitleFile(bad);
        const std::wstring dialog = t::WaitDialog(3000);
        CHECK(dialog.find(L"Can't open the subtitles \"" + std::wstring(name) + L"\"") != std::wstring::npos);
    }
    CHECK_EQ(SubtitleCount(), size_t(1));
    // A video and its subtitles dropped together: the video plays with them shown.
    const std::wstring video = CopyMedia(L"gop5s_nob.mp4", L"dropped.mp4");
    Drop({srt, video});
    WaitLoaded();
    CHECK_EQ(Title(), std::wstring(L"dropped"));
    REQUIRE(SubtitleCount() >= 1);
    CHECK_EQ(ShownSubtitle(), int(SubtitleCount()) - 1);
}

CORE_TEST(window_new_video_starts_fresh, 30000) {
    Start(t::Media(L"tracks.mkv"));
    Key('S');
    Press(kSubtitles);
    Key(VK_OEM_PERIOD, false, true);
    REQUIRE(PanelOpen());
    CHECK(OpenFiles({t::Media(L"clip30.mp4")}));
    WaitLoaded();
    CHECK_EQ(SubtitleCount(), size_t(0));
    CHECK_EQ(ShownSubtitle(), -1);
    CHECK(!PanelOpen());
    CHECK_EQ(Rate(), 1.0);
    CHECK_NEAR(WindowDuration(), 6.0, 0.05);
    CHECK(t::PumpUntil([] { return AudioLabels().size() == 1; }, 5000));
}

CORE_TEST(window_missing_and_damaged_files, 40000) {
    Start(t::Media(L"clip30.mp4"));
    CHECK(!OpenFiles({t::TempDir() + L"\\gone.mp4"}));
    CHECK(t::WaitDialog(3000).find(L"Can't open \"gone.mp4\".\n\nThe file doesn't exist.") != std::wstring::npos);
    CHECK(!OpenFiles({L"\\\\.\\pipe\\video.mp4"}));
    CHECK(t::WaitDialog(3000).find(L"Can't open \"video.mp4\"") != std::wstring::npos);
    // The video playing goes on.
    CHECK(IsPlaying());
    // One the engine can't play says so once it finds out, and the window stays.
    const std::wstring bad = t::TempDir() + L"\\noise.mp4";
    WriteText(bad, std::string(50000, '\x33'));
    CHECK(OpenFiles({bad}));
    CHECK(t::WaitDialog(10000).find(L"Can't play this video.") == 0);
    CHECK(IsWindow(Hwnd()));
    CHECK(!Playing());
    CHECK(OpenFiles({t::Media(L"clip30.mp4")}));
    WaitLoaded();
}

CORE_TEST(window_starting_with_a_damaged_file_closes, 30000) {
    const std::wstring bad = t::TempDir() + L"\\noise.mkv";
    WriteText(bad, std::string(50000, '\x33'));
    fx::Silence();
    REQUIRE(StartPlayer(GetModuleHandleW(nullptr), {bad}, SW_SHOWNOACTIVATE));
    CHECK(t::WaitDialog(10000).find(L"Can't play this video.") == 0);
    CHECK(t::PumpUntil([] { return !IsWindow(Hwnd()); }, 5000));  // as a failed start, it closes
}

CORE_TEST(window_starting_with_a_missing_file_fails, 30000) {
    fx::Silence();
    CHECK(!StartPlayer(GetModuleHandleW(nullptr), {t::TempDir() + L"\\gone.mp4"}, SW_SHOWNOACTIVATE));
    CHECK(t::WaitDialog(3000).find(L"Can't open \"gone.mp4\"") != std::wstring::npos);
    CHECK(!IsWindow(Hwnd()));
}

CORE_TEST(window_without_a_video, 30000) {
    Start({}, false);
    CHECK(!HasVideo());
    RECT client;
    GetClientRect(Hwnd(), &client);
    CHECK_EQ(client.right, LONG(Px(960)));
    // Keys that need a video do nothing.
    for (const UINT key : {UINT(VK_SPACE), UINT(VK_RIGHT), UINT(VK_HOME), UINT(VK_END), UINT(VK_OEM_PERIOD), UINT('S'), UINT(VK_F11)}) {
        Key(key);
    }
    CHECK(!Fullscreen());
    Key(VK_UP);  // the volume does
    CHECK_NEAR(WindowVolume(), 1.05, 1e-9);
    // The first video dropped starts the player.
    Drop({t::Media(L"clip30.mp4")});
    WaitLoaded();
    CHECK(HasVideo());
    CHECK(fx::WaitFrameAt(0.2, 5000));
}

CORE_TEST(window_starting_with_subtitles_only, 30000) {
    const std::wstring srt = t::TempDir() + L"\\only.srt";
    WriteText(srt, kSrt);
    Start({srt}, false);
    CHECK(!HasVideo());
    CHECK(IsWindow(Hwnd()));
}

CORE_TEST(window_fullscreen_and_escape, 30000) {
    Start(t::Media(L"clip30.mp4"));
    RECT before;
    GetWindowRect(Hwnd(), &before);
    Key(VK_F11);
    CHECK(Fullscreen());
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(Hwnd(), MONITOR_DEFAULTTONEAREST), &mi);
    RECT full;
    GetWindowRect(Hwnd(), &full);
    CHECK(EqualRect(&full, &mi.rcMonitor));
    CHECK_EQ(MenuBarHeight(), 0);
    Key(VK_ESCAPE);  // leaves fullscreen first
    CHECK(!Fullscreen());
    RECT after;
    GetWindowRect(Hwnd(), &after);
    CHECK(EqualRect(&before, &after));
    // A double click on the video goes fullscreen, leaving playback as it was.
    const POINT middle{(g.videoRect.left + g.videoRect.right) / 2, (g.videoRect.top + g.videoRect.bottom) / 2};
    SetControlsShown(false);
    const bool playing = Playing();
    SendMessageW(Hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, At(middle));
    SendMessageW(Hwnd(), WM_LBUTTONUP, 0, At(middle));
    SendMessageW(Hwnd(), WM_LBUTTONDBLCLK, MK_LBUTTON, At(middle));
    SendMessageW(Hwnd(), WM_LBUTTONUP, 0, At(middle));
    CHECK(Fullscreen());
    CHECK_EQ(Playing(), playing);
    Key(VK_F11);
    CHECK(!Fullscreen());
    Key(VK_ESCAPE);  // and then closes
    CHECK(t::PumpUntil([] { return !IsWindow(Hwnd()); }, 5000));
}

CORE_TEST(window_seek_bar_click_and_drag, 30000) {
    Start(t::Media(L"gop5s_nob.mp4"));
    Pause();
    Wake();  // the controls show, as for a mouse move
    const RECT bar = ControlRect(kSeek);
    const int y = (bar.top + bar.bottom) / 2;
    const auto x = [&](double fraction) { return int(bar.left + fraction * (bar.right - bar.left)); };
    // A click seeks exactly to the time under it.
    SendMessageW(Hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x(0.5), y));
    SendMessageW(Hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(x(0.5), y));
    Settle();
    CHECK_NEAR(PlaybackTime(), 6.0, 0.1);
    // A drag seeks nowhere until let go.
    SendMessageW(Hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x(0.1), y));
    for (int i = 1; i <= 20; ++i) SendMessageW(Hwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x(0.1 + 0.03 * i), y));
    t::Pump(100);
    CHECK_NEAR(PlaybackTime(), 6.0, 0.1);
    SendMessageW(Hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(x(0.75), y));
    Settle();
    CHECK_NEAR(PlaybackTime(), 9.0, 0.1);
    // Past either end of the bar is its end.
    SendMessageW(Hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x(0.5), y));
    SendMessageW(Hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(x(-1), y));
    Settle();
    CHECK_NEAR(PlaybackTime(), 0.0, 0.01);
}

CORE_TEST(window_closes_cleanly_while_busy, 30000) {
    Start(t::Media(L"tracks.mkv"));
    Key('S');
    Key(VK_OEM_PERIOD, false, true);
    for (int i = 0; i < 20; ++i) Key(VK_RIGHT, false, false, true);
    const double start = t::Now();
    SendMessageW(Hwnd(), WM_CLOSE, 0, 0);
    CHECK(!IsWindow(Hwnd()));
    CHECK(t::Now() - start < 3);
}


// --- What the screen shows ---

namespace {

// The window on top, clear of the mouse so the controls stay hidden, and the screen read where the video is.
class OnScreen {
  public:
    OnScreen() {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(Hwnd(), MONITOR_DEFAULTTONEAREST), &mi);
        RECT r;
        GetWindowRect(Hwnd(), &r);
        POINT cursor;
        GetCursorPos(&cursor);
        // In the corner of the work area the mouse isn't in.
        const bool left = cursor.x > (mi.rcWork.left + mi.rcWork.right) / 2;
        const bool top = cursor.y > (mi.rcWork.top + mi.rcWork.bottom) / 2;
        const int x = left ? mi.rcWork.left : mi.rcWork.right - (r.right - r.left);
        const int y = top ? mi.rcWork.top : mi.rcWork.bottom - (r.bottom - r.top);
        SetWindowPos(Hwnd(), HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        GetWindowRect(Hwnd(), &r);
        if (PtInRect(&r, cursor)) SKIP("the mouse is over the window wherever it goes");
        if (!screen_.Open(Hwnd())) SKIP("can't capture the screen (locked, or a remote session)");
    }

    // The frame number the video on screen shows, or -1; reads whether a subtitle shows with it.
    int Frame(bool* subtitle = nullptr) {
        // The controls and the seek's time over the video hidden, which would cover its bits.
        SetControlsShown(false);
        HideOsd();
        RECT v = g.videoRect;
        MapWindowPoints(Hwnd(), nullptr, reinterpret_cast<POINT*>(&v), 2);
        // Something that pops up over every window, such as a notification, would be read instead.
        const POINT corners[] = {{v.left + 2, v.top + 2}, {v.right - 3, v.top + 2}, {v.left + 2, v.bottom - 3},
                                 {v.right - 3, v.bottom - 3}, {(v.left + v.right) / 2, (v.top + v.bottom) / 2}};
        const auto ours = [&] {
            for (const POINT& p : corners) {
                if (GetAncestor(WindowFromPoint(p), GA_ROOT) != Hwnd()) return false;
            }
            return true;
        };
        if (!t::PumpUntil(ours, 5000)) SKIP("another window covers the player");
        std::vector<BYTE> px;
        if (!screen_.Read(v, px, 400)) return -2;
        const UINT w = UINT(v.right - v.left), h = UINT(v.bottom - v.top);
        if (subtitle) *subtitle = fx::HasWhiteText(px.data(), w * 4, w, h);
        const int frame = fx::FrameNumber(px.data(), w * 4, w, h);
        if (frame < 0) {  // kept for a look at what showed instead
            BITMAPINFOHEADER bi{sizeof(bi), LONG(w), -LONG(h), 1, 32, BI_RGB};
            BITMAPFILEHEADER bf{0x4D42, DWORD(sizeof(bf) + sizeof(bi) + px.size()), 0, 0, DWORD(sizeof(bf) + sizeof(bi))};
            std::string bmp(reinterpret_cast<const char*>(&bf), sizeof(bf));
            bmp.append(reinterpret_cast<const char*>(&bi), sizeof(bi));
            bmp.append(reinterpret_cast<const char*>(px.data()), px.size());
            wchar_t base[MAX_PATH];
            GetTempPathW(MAX_PATH, base);
            const std::wstring path = std::wstring(base) + L"VideoPLAYER-unreadable-frame.bmp";
            if (const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); f != INVALID_HANDLE_VALUE) {
                DWORD n;
                WriteFile(f, bmp.data(), DWORD(bmp.size()), &n, nullptr);
                CloseHandle(f);
                printf("  the screen showed no frame number; saved as %s\n", t::Utf8(path).c_str());
            }
        }
        return frame;
    }

  private:
    fx::Screen screen_;
};

}  // namespace

CORE_TEST(screen_shows_the_frame_each_seek_and_step_lands_on, 60000) {
    Start(t::Media(L"clip30.mp4"));
    CHECK(fx::WaitFrameAt(0.3, 5000));
    Pause();
    OnScreen screen;
    const auto at = [&](double seconds) {
        JumpTo(seconds);
        Settle();
        return screen.Frame();
    };
    CHECK_EQ(at(2.0), 60);
    CHECK_EQ(at(3.02), 90);   // between two frames: the one before
    CHECK_EQ(at(5.5), 165);  // (not the last frame: see player_seeks_after_the_last_frame_show_their_frame)
    Key(VK_HOME);
    Settle();
    CHECK_EQ(screen.Frame(), 0);
    CHECK_EQ(at(2.0), 60);
    for (int i = 1; i <= 3; ++i) {
        Key(VK_OEM_PERIOD);
        Settle();
        CHECK_EQ(screen.Frame(), 60 + i);
    }
    Key(VK_OEM_COMMA);
    Settle();
    CHECK_EQ(screen.Frame(), 62);
}

CORE_TEST(screen_fast_seeks_show_the_key_frame, 60000) {
    Start(t::Media(L"gop5s_nob.mp4"));
    Pause();
    OnScreen screen;
    Key(VK_RIGHT);
    Settle();
    CHECK_EQ(screen.Frame(), 125);
    Key(VK_RIGHT);
    Settle();
    CHECK_EQ(screen.Frame(), 250);
    Key(VK_LEFT);
    Settle();
    CHECK_EQ(screen.Frame(), 125);
}

CORE_TEST(screen_frames_move_on_while_playing, 30000) {
    Start(t::Media(L"clip30.mp4"));
    OnScreen screen;
    const int a = screen.Frame();
    const double start = t::Now();
    t::Pump(1000);
    const int b = screen.Frame();
    const double frames = (t::Now() - start) * 30;
    REQUIRE(a >= 0);
    REQUIRE(b >= 0);
    CHECK_NEAR(b - a, frames, 6);
}

CORE_TEST(screen_subtitles_show_on_their_frames, 60000) {
    // English cues: 0.5-1.5, 2-3 and 4-5.5 s, read from the file.
    Start(t::Media(L"tracks.mkv"));
    REQUIRE(t::PumpUntil([] { return ReadSubtitleCues(0) == 3; }, 5000));
    Pause();
    HideOsd();
    OnScreen screen;
    Key('S');
    REQUIRE_EQ(ShownSubtitle(), 0);
    struct {
        double at;
        bool shows;
    } cases[] = {{0.2, false}, {0.6, true}, {1.4, true}, {1.6, false}, {2.5, true}, {3.5, false}, {4.2, true},
                 {5.6, false}, {2.0, true}, {1.96, false}};
    for (const auto& c : cases) {
        NOTE("at " + std::to_string(c.at));
        JumpTo(c.at);
        Settle();
        HideOsd();
        bool subtitle = false;
        const int frame = screen.Frame(&subtitle);
        CHECK_EQ(frame, int(c.at * 25 + 1e-6));
        CHECK_EQ(subtitle, c.shows);
    }
    // Off, none.
    Key('S');
    HideOsd();
    JumpTo(2.5);
    Settle();
    bool subtitle = true;
    screen.Frame(&subtitle);
    CHECK(!subtitle);
}

CORE_TEST(screen_subtitle_file_shows, 30000) {
    Start(t::Media(L"silent30.mp4"));
    Pause();
    const std::wstring srt = t::TempDir() + L"\\added.srt";
    WriteText(srt, "1\r\n00:00:01,000 --> 00:00:02,000\r\nAdded\r\n");
    LoadSubtitleFile(srt);
    HideOsd();
    OnScreen screen;
    bool subtitle = false;
    JumpTo(1.5);
    Settle();
    CHECK_EQ(screen.Frame(&subtitle), 45);
    CHECK(subtitle);
    JumpTo(2.5);
    Settle();
    screen.Frame(&subtitle);
    CHECK(!subtitle);
}
