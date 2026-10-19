// The player as the window drives it: Media Foundation's engine with the player's own source, render thread,
// previewer and sound. Each test runs in a process of its own.

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfmediaengine.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "config.h"
#include "fixture.h"
#include "media/player.h"
#include "media/source.h"

using Microsoft::WRL::ComPtr;
using fx::Messages;

namespace {

constexpr double kFrame30 = 1 / 30.0;

// How far the playback clock moves in `seconds` of real time.
double Advance(double seconds) {
    const double a = PlaybackTime(), start = t::Now();
    t::Pump(int(seconds * 1000));
    const double b = PlaybackTime();
    return (b - a) / (t::Now() - start) * seconds;
}

// The picture of a preview, decoded: its frame number, or -1. Waits for it to be posted.
int PreviewFrame(LONGLONG keyFrame, LONGLONG& got) {
    const size_t from = Messages().size();
    DecodePreview(keyFrame);
    if (fx::WaitMessage(WM_APP_PREVIEW, 10000, from) < 0) return -2;
    const HANDLE handle = TakePreviewPicture(got);
    if (!handle) return -3;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        return -4;
    }
    ComPtr<ID3D11Texture2D> shared, staging;
    ComPtr<IDXGIKeyedMutex> mutex;
    if (FAILED(device->OpenSharedResource(handle, IID_PPV_ARGS(&shared))) || FAILED(shared.As(&mutex)) ||
        mutex->AcquireSync(1, 5000) != S_OK) {
        return -5;
    }
    D3D11_TEXTURE2D_DESC desc;
    shared->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    int frame = -6;
    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        context->CopyResource(staging.Get(), shared.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            frame = fx::FrameNumber(static_cast<const BYTE*>(m.pData), m.RowPitch, desc.Width, desc.Height);
            context->Unmap(staging.Get(), 0);
        }
    }
    mutex->ReleaseSync(0);
    return frame;
}

}  // namespace

// --- Opening ---

CORE_TEST(player_opens_and_plays, 30000) {
    fx::Start(t::Media(L"clip30.mp4"));
    fx::WaitPlaying();
    CHECK(HasVideo());
    CHECK(IsPlaying());
    CHECK(!HasEnded());
    CHECK_NEAR(Duration(), 6.0, 0.05);
    const SIZE size = VideoSize();
    CHECK_EQ(size.cx, LONG(320));
    CHECK_EQ(size.cy, LONG(180));
    CHECK(VideoSwapChain() != nullptr);
    // Events come in the order the window relies on: the metadata, then the first frame.
    const int meta = fx::WaitMedia(MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA, 0);
    const int first = fx::WaitMedia(MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY, 0);
    CHECK(meta >= 0 && first > meta);
}

CORE_TEST(player_knows_the_first_size_before_the_engine, 30000) {
    struct {
        const wchar_t* file;
        LONG w, h;
    } cases[] = {{L"clip30.mp4", 320, 180}, {L"anamorphic.mp4", 427, 240}, {L"rotated.mp4", 180, 320},
                 {L"odd.mp4", 322, 182}, {L"tracks.mkv", 320, 180}};
    // Only the first file is made ahead, once per process; the others through the source itself.
    PrepareVideo(t::Media(cases[0].file));
    t::PumpUntil([] { return PreparedVideoSize().cx > 0; }, 10000);
    CHECK_EQ(PreparedVideoSize().cx, cases[0].w);
    CHECK_EQ(PreparedVideoSize().cy, cases[0].h);
    REQUIRE(InitPlayer(fx::EventWindow()));
    fx::Silence();
    for (const auto& c : cases) {
        NOTE(t::Utf8(c.file));
        const size_t from = Messages().size();
        fx::Open(t::Media(c.file));
        REQUIRE(fx::WaitMedia(MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA, 10000, from) >= 0);
        const SIZE v = VideoSize();
        // The engine's size of the picture as shown: non-square pixels stretch it.
        if (c.h > c.w) {
            // Turned a quarter: the window sizes to the prepared size first, then to the engine's.
            CHECK((v.cx == c.w && v.cy == c.h) || (v.cx == c.h && v.cy == c.w));
        } else {
            CHECK_NEAR(v.cx, c.w, 1);
            CHECK_EQ(v.cy, c.h);
        }
    }
}

CORE_TEST(player_rotated_video_sizes_agree, 30000) {
    // The window shows at the size the source made ahead tells, and resizes once the engine tells another: for a
    // turned video the two must agree, or the window opens at one shape and jumps to the other.
    PrepareVideo(t::Media(L"rotated.mp4"));
    t::PumpUntil([] { return PreparedVideoSize().cx > 0; }, 10000);
    const SIZE prepared = PreparedVideoSize();
    REQUIRE(InitPlayer(fx::EventWindow()));
    fx::Silence();
    fx::Open(t::Media(L"rotated.mp4"));
    REQUIRE(fx::WaitMedia(MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA, 10000) >= 0);
    const SIZE engine = VideoSize();
    CHECK_EQ(prepared.cx, LONG(180));
    CHECK_EQ(prepared.cy, LONG(320));
    CHECK_EQ(engine.cx, prepared.cx);
    CHECK_EQ(engine.cy, prepared.cy);
}

CORE_TEST(player_sound_only_file, 30000) {
    // Since the sound plays apart from it, the engine is handed no audio stream, so a file with only sound has nothing it can play.
    KNOWN_BUG("a file of sound alone fails with SRC_NOT_SUPPORTED since the commit \"Play the sound apart from the media engine\"");
    fx::Start(t::Media(L"audio.m4a"));
    t::PumpUntil([] {
        for (const auto& m : Messages()) {
            if (m.msg == WM_APP_MEDIA && (m.wp == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA || m.wp == MF_MEDIA_ENGINE_EVENT_ERROR)) return true;
        }
        return false;
    }, 10000);
    for (const auto& m : Messages()) {
        if (m.msg == WM_APP_MEDIA && m.wp == MF_MEDIA_ENGINE_EVENT_ERROR) {
            t::Fail(__FILE__, __LINE__, "the engine reports error " + std::to_string(m.lp) + " for a file of sound alone");
        }
    }
    REQUIRE(fx::WaitMedia(MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA, 10000) >= 0);
    REQUIRE(fx::WaitMedia(MF_MEDIA_ENGINE_EVENT_PLAYING, 10000) >= 0);
    CHECK(!HasVideo());
    CHECK_EQ(VideoSize().cx, LONG(0));
    CHECK_NEAR(Duration(), 4.0, 0.05);
    REQUIRE(fx::WaitMessage(WM_APP_AUDIO_TRACKS, 5000) >= 0);
    CHECK_EQ(AudioTracks().size(), size_t(1));
    CHECK(Advance(0.5) > 0.3);
    OpenSeekHelpers();
    t::Pump(500);
    CHECK_EQ(PreviewSize().cx, LONG(0));  // no pictures to preview
}

CORE_TEST(player_odd_and_anamorphic_frames_play, 30000) {
    for (const wchar_t* file : {L"odd.mp4", L"anamorphic.mp4", L"rotated.mp4", L"surround.mp4", L"surround.mkv"}) {
        NOTE(t::Utf8(file));
        static bool started = false;
        const size_t from = Messages().size();
        if (!started) {
            fx::Start(t::Media(file));
            started = true;
        } else {
            fx::Open(t::Media(file));
        }
        fx::WaitPlaying(from);
        CHECK(fx::WaitFrameAt(0.4, 5000));
    }
}

// --- Failing to open ---

CORE_TEST(player_refuses_device_paths, 30000) {
    REQUIRE(InitPlayer(fx::EventWindow()));
    // Devices that could block or never end, named every way: none is opened.
    for (const wchar_t* path :
         {L"\\\\.\\pipe\\x", L"\\\\.\\C:", L"\\\\.\\PhysicalDrive0", L"\\??\\C:\\x.mp4", L"//./pipe/x", L"\\\\?\\pipe\\x",
          L"\\\\?\\GLOBALROOT\\Device\\Null", L"\\\\server\\pipe\\x", L"\\\\server\\PIPE\\x", L"\\\\server\\mailslot\\x",
          L"\\\\?\\UNC\\server\\pipe\\x", L"\\\\.\\CON", L"/\\.\\NUL"}) {
        NOTE(t::Utf8(path));
        CHECK_EQ(OpenVideo(path), HRESULT(E_ACCESSDENIED));
    }
    // Files, in every form a path to one takes, are tried, and missing here.
    const std::wstring dir = t::TempDir();
    // (Not UNC paths, which would wait on the network.)
    for (const std::wstring& path : {dir + L"\\missing.mp4", L"\\\\?\\" + dir + L"\\missing.mp4", dir + L"/missing.mp4",
                                     std::wstring(L"\\missing-root\\x.mp4"), std::wstring(L"relative-missing.mp4")}) {
        NOTE(t::Utf8(path));
        const HRESULT hr = OpenVideo(path);
        CHECK(FAILED(hr));
        CHECK(hr != E_ACCESSDENIED);
    }
}

CORE_TEST(player_damaged_files_report_an_error, 60000) {
    REQUIRE(InitPlayer(fx::EventWindow()));
    fx::Silence();
    std::string clip;
    {
        const HANDLE f = CreateFileW(t::Media(L"clip30.mp4").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        REQUIRE(f != INVALID_HANDLE_VALUE);
        clip.resize(GetFileSize(f, nullptr));
        DWORD read;
        ReadFile(f, clip.data(), DWORD(clip.size()), &read, nullptr);
        CloseHandle(f);
    }
    const std::pair<const wchar_t*, std::string> files[] = {
        {L"empty.mp4", ""},
        {L"text.mp4", "this is not a video at all"},
        {L"noise.mkv", std::string(100000, '\x5A')},
        {L"truncated.mp4", clip.substr(0, 2000)},
    };
    for (const auto& [name, bytes] : files) {
        NOTE(t::Utf8(name));
        const size_t from = Messages().size();
        const HRESULT hr = OpenVideo(t::WriteTemp(name, bytes));
        if (FAILED(hr)) continue;  // refused at once also counts
        // Either an error, or for a cut-short file, maybe the start of it plays: never a hang.
        const double end = t::Now() + 10;
        bool settled = false;
        while (!settled && t::Now() < end) {
            t::Pump(20);
            for (size_t i = from; i < Messages().size(); ++i) {
                const auto& m = Messages()[i];
                if (m.msg == WM_APP_MEDIA && (m.wp == MF_MEDIA_ENGINE_EVENT_ERROR || m.wp == MF_MEDIA_ENGINE_EVENT_ENDED ||
                                              m.wp == MF_MEDIA_ENGINE_EVENT_PLAYING)) {
                    settled = true;
                }
            }
        }
        CHECK(settled);
    }
    // And a good file plays after them.
    const size_t from = Messages().size();
    fx::Open(t::Media(L"clip30.mp4"));
    fx::WaitPlaying(from);
}

// --- The clock ---

CORE_TEST(player_plays_in_real_time, 30000) {
    fx::Start(t::Media(L"clip30.mp4"));
    fx::WaitPlaying();
    t::Pump(300);
    CHECK_NEAR(Advance(1.0), 1.0, 0.15);
}

CORE_TEST(player_pause_holds_the_clock, 30000) {
    fx::Start(t::Media(L"clip30.mp4"));
    fx::WaitPlaying();
    t::Pump(300);
    const size_t from = Messages().size();
    SetPlaying(false);
    CHECK(!IsPlaying());
    CHECK(fx::WaitMedia(MF_MEDIA_ENGINE_EVENT_PAUSE, 5000, from) >= 0);
    t::Pump(100);
    CHECK_NEAR(Advance(0.5), 0.0, 0.001);
    SetPlaying(true);
    t::Pump(300);
    CHECK_NEAR(Advance(0.5), 0.5, 0.1);
}

CORE_TEST(player_rates, 40000) {
    fx::Start(t::Media(L"gop5s.mp4"));
    fx::WaitPlaying();
    for (const double rate : {2.0, 0.5, 1.5, 1.0}) {
        NOTE("rate " + std::to_string(rate));
        SetPlaybackRate(rate);
        t::Pump(400);  // the engine restarts at a new rate
        CHECK_NEAR(Advance(1.0), rate, 0.2 * rate);
    }
}

CORE_TEST(player_ends_and_starts_over, 30000) {
    fx::Start(t::Media(L"clip30.mp4"));
    fx::WaitPlaying();
    const size_t from = Messages().size();
    fx::SeekAndWait(5.5, false);
    REQUIRE(fx::WaitMedia(MF_MEDIA_ENGINE_EVENT_ENDED, 5000, from) >= 0);
    CHECK(HasEnded());
    CHECK(!IsPlaying());
    CHECK_NEAR(PlaybackTime(), Duration(), 0.1);
    SetPlaying(true);  // from the start
    t::Pump(500);
    CHECK(IsPlaying());
    CHECK(PlaybackTime() < 1.0);
}

// --- Seeking ---

CORE_TEST(player_exact_seeks_land_on_their_frame, 40000) {
    fx::Start(t::Media(L"clip30.mp4"));
    fx::WaitPlaying();
    SetPlaying(false);
    // A seek between two frames shows the one before it, and the player knows that is the frame on screen.
    for (const double target : {3.0, 3.02, 1.5, 0.0, 5.5, 4.01, 2.999}) {
        NOTE("seek to " + std::to_string(target));
        fx::SeekAndWait(target, false);
        CHECK_NEAR(PlaybackTime(), target, 0.002);
        const double frame = std::floor(target / kFrame30 + 1e-6) * kFrame30;
        CHECK(t::PumpUntil([&] { return std::abs(FrameTime() - frame) < 0.002; }, 3000));
        CHECK_NEAR(FrameTime(), frame, 0.002);
    }
}

namespace {

// Fast seeks on a video with key frames at 0, 5 and 10 s go exactly to the nearest, which shows at once.
void FastSeeksLandOnKeyFrames(const wchar_t* file) {
    fx::Start(t::Media(file));
    fx::WaitPlaying();
    // The previewer opens the file for where its key frames are, at 0, 5 and 10 s.
    REQUIRE(t::PumpUntil([] { return SeekLanding(2.6, true) != 2.6; }, 10000));
    CHECK_NEAR(SeekLanding(2.4, true), 0.0005, 1e-6);
    CHECK_NEAR(SeekLanding(2.6, true), 5.0005, 1e-6);
    CHECK_NEAR(SeekLanding(9.0, true), 10.0005, 1e-6);
    CHECK_NEAR(SeekLanding(11.9, true), 10.0005, 1e-6);
    CHECK_EQ(SeekLanding(2.6, false), 2.6);  // an exact one goes where asked
    SetPlaying(false);
    fx::SeekAndWait(6.0, true);
    CHECK_NEAR(PlaybackTime(), 5.0, 0.002);
    CHECK(t::PumpUntil([] { return std::abs(FrameTime() - 5.0) < 0.002; }, 3000));
}

}  // namespace

CORE_TEST(player_seeks_after_the_last_frame_show_their_frame, 180000) {
    // Paused, a seek to the last frame, then elsewhere: each shows its own frame. Now and then the second shows
    // none, the picture staying on the last frame, until a seek to somewhere else; so it goes back and forth until
    // that happens or plainly doesn't.
    KNOWN_BUG("now and then, paused, the frame of a seek after one to the last frame never shows");
    fx::Start(t::Media(L"clip30.mp4"));
    fx::WaitPlaying();
    SetPlaying(false);
    for (int round = 0; round < 80; ++round) {
        NOTE("round " + std::to_string(round));
        fx::SeekAndWait(5.95, false);
        t::PumpUntil([&] { return std::abs(FrameTime() - 177 / 30.0) < 0.002; }, 3000);
        fx::SeekAndWait(2.0, false);
        t::PumpUntil([&] { return std::abs(FrameTime() - 2.0) < 0.002; }, 3000);
        if (!CHECK_NEAR(FrameTime(), 2.0, 0.002)) return;
    }
}

CORE_TEST(player_fast_seeks_land_on_key_frames, 40000) { FastSeeksLandOnKeyFrames(L"gop5s_nob.mp4"); }

CORE_TEST(player_fast_seeks_land_on_key_frames_with_b_frames, 40000) {
    KNOWN_BUG("MP4 key frame times aren't moved by the edit list's shift, so fast seeks land 2 frames past them");
    FastSeeksLandOnKeyFrames(L"gop5s.mp4");
}

CORE_TEST(player_matroska_key_frames_from_its_index, 40000) {
    fx::Start(t::Media(L"tracks.mkv"));
    fx::WaitPlaying();
    // Media Foundation's Matroska parser doesn't tell, so they come from the file's index: a key frame a second.
    REQUIRE(t::PumpUntil([] { return SeekLanding(2.4, true) != 2.4; }, 10000));
    CHECK_NEAR(SeekLanding(2.4, true), 2.0005, 1e-6);
    CHECK_NEAR(SeekLanding(2.6, true), 3.0005, 1e-6);
    SetPlaying(false);
    fx::SeekAndWait(3.4, true);
    CHECK_NEAR(PlaybackTime(), 3.0, 0.05);
}

CORE_TEST(player_frame_gaps, 30000) {
    for (const auto& [file, gap] : {std::pair{L"clip30.mp4", 1 / 30.0}, std::pair{L"clip60.mp4", 1 / 60.0}}) {
        NOTE(t::Utf8(file));
        static bool started = false;
        const size_t from = Messages().size();
        if (!started) {
            fx::Start(t::Media(file));
            started = true;
        } else {
            fx::Open(t::Media(file));
        }
        fx::WaitPlaying(from);
        CHECK(fx::WaitFrameAt(0.5, 5000));
        CHECK_NEAR(FrameGap(), gap, 0.0005);
    }
}

CORE_TEST(player_seek_storm_settles, 60000) {
    // Seeks one on another, as a drag along the seek bar makes them, never wedge the engine.
    fx::Start(t::Media(L"gop5s.mp4"));
    fx::WaitPlaying();
    for (int i = 0; i < 200; ++i) {
        SeekTo(SeekLanding(double(i * 7 % 12), i % 2 == 0), i % 2 == 0);
        if (i % 10 == 0) t::Pump(1);
    }
    fx::SeekAndWait(7.0, false);
    CHECK_NEAR(PlaybackTime(), 7.0, 0.01);
    t::Pump(300);
    CHECK(PlaybackTime() > 7.1);  // and plays on
}

// --- Volume ---

CORE_TEST(player_volume_rules, 20000) {
    fx::Silence();
    REQUIRE(InitPlayer(fx::EventWindow()));
    CHECK_EQ(SetVolume(0.5), 0.5);
    CHECK_EQ(Volume(), 0.5);
    CHECK_EQ(SetVolume(3), kMaxVolume);
    CHECK_EQ(SetVolume(-1), 0.0);
    CHECK_EQ(SetVolume(0.123), 0.12);  // to the percent
    // Turning it up unmutes; down doesn't; setting 0 keeps it muted.
    CHECK(ToggleMute());
    CHECK(IsMuted());
    CHECK_NEAR(ChangeVolume(-0.05), 0.07, 1e-9);
    CHECK(IsMuted());
    CHECK_NEAR(ChangeVolume(0.05), 0.12, 1e-9);
    CHECK(!IsMuted());
    CHECK(ToggleMute());
    CHECK_EQ(SetVolume(0), 0.0);
    CHECK(IsMuted());
    CHECK_EQ(SetVolume(0.3), 0.3);
    CHECK(!IsMuted());
    CHECK(ToggleMute());
    CHECK(!ToggleMute());
    // Steps add up without drifting off the percent.
    SetVolume(0);
    for (int i = 0; i < 40; ++i) ChangeVolume(kVolumeStep);
    CHECK_EQ(Volume(), kMaxVolume);
    for (int i = 0; i < 20; ++i) ChangeVolume(-kVolumeStep);
    CHECK_EQ(Volume(), 1.0);
}

// --- Tracks ---

CORE_TEST(player_audio_tracks, 30000) {
    fx::Start(t::Media(L"tracks.mkv"));
    REQUIRE(fx::WaitMessage(WM_APP_AUDIO_TRACKS, 10000) >= 0);
    std::vector<Track> tracks = AudioTracks();
    REQUIRE_EQ(tracks.size(), size_t(2));
    CHECK(tracks[0].selected);
    CHECK(!tracks[1].selected);
    CHECK(tracks[0].language.rfind(L"en", 0) == 0);
    CHECK(tracks[1].language.rfind(L"fr", 0) == 0);
    CHECK_EQ(tracks[1].name, std::wstring(L"Commentary"));
    SelectAudioTrack(1);
    tracks = AudioTracks();
    CHECK(!tracks[0].selected);
    CHECK(tracks[1].selected);
    SelectAudioTrack(7);  // none such: nothing changes
    CHECK(AudioTracks()[1].selected);
    // Each file tells its own.
    const size_t from = Messages().size();
    fx::Open(t::Media(L"silent30.mp4"));
    REQUIRE(fx::WaitMessage(WM_APP_AUDIO_TRACKS, 10000, from) >= 0);
    CHECK(AudioTracks().empty());
}

// --- The seek bar previews ---

CORE_TEST(player_previews_show_their_key_frames, 60000) {
    fx::Start(t::Media(L"clip30.mp4"));
    fx::WaitPlaying();
    REQUIRE(t::PumpUntil([] { return PreviewSize().cx > 0; }, 10000));
    CHECK_EQ(PreviewSize().cx, LONG(320));
    CHECK_EQ(PreviewSize().cy, LONG(180));
    const double duration = Duration();
    // Until the key frames are known, a span's middle.
    REQUIRE(t::PumpUntil([&] { return PreviewKeyFrame(2.2, duration) != 25'000'000; }, 10000));
    // A span is a second of this video: its picture is the key frame nearest its middle, one a second. Whatever
    // time the key frame is told at, its picture is the right frame.
    for (const double at : {0.1, 2.2, 3.7, 5.9}) {
        NOTE("at " + std::to_string(at));
        const LONGLONG key = PreviewKeyFrame(at, duration);
        CHECK_NEAR(key, std::floor(at) * 1e7, 0.1 * 1e7);
        LONGLONG got = -1;
        CHECK_EQ(PreviewFrame(key, got), int(std::floor(at)) * 30);
        CHECK_EQ(got, key);
    }
    CHECK_EQ(PreviewKeyFrameNear(2.2, duration, 1), PreviewKeyFrame(3.2, duration));
    CHECK_EQ(PreviewKeyFrameNear(2.2, duration, -2), PreviewKeyFrame(0.2, duration));
    CHECK_EQ(PreviewKeyFrameNear(2.2, duration, -3), LONGLONG(-1));
    CHECK_EQ(PreviewKeyFrameNear(2.2, duration, 4), LONGLONG(-1));
}

CORE_TEST(player_previews_of_a_long_gop, 60000) {
    fx::Start(t::Media(L"gop5s.mp4"));
    fx::WaitPlaying();
    REQUIRE(t::PumpUntil([] { return PreviewSize().cx > 0 && PreviewKeyFrame(6.0, 12.0) != 65'000'000; }, 10000));
    for (const auto& [at, frame] : {std::pair{1.0, 0}, std::pair{4.0, 125}, std::pair{11.0, 250}}) {
        NOTE("at " + std::to_string(at));
        const LONGLONG key = PreviewKeyFrame(at, Duration());
        CHECK_NEAR(key, frame / 25.0 * 1e7, 0.1 * 1e7);
        LONGLONG got = -1;
        CHECK_EQ(PreviewFrame(key, got), frame);
    }
}

// --- Several files and shutting down ---

CORE_TEST(player_opens_file_after_file, 90000) {
    const wchar_t* files[] = {L"clip30.mp4", L"tracks.mkv", L"gop5s.mp4", L"av_pcm.mkv", L"surround.mp4", L"clip30.mp4"};
    bool started = false;
    for (int round = 0; round < 2; ++round) {
        for (const wchar_t* file : files) {
            NOTE(t::Utf8(file));
            const size_t from = Messages().size();
            if (!started) {
                fx::Start(t::Media(file));
                started = true;
            } else {
                fx::Open(t::Media(file));
            }
            REQUIRE(fx::WaitMedia(MF_MEDIA_ENGINE_EVENT_PLAYING, 10000, from) >= 0);
            if (round == 0) t::Pump(200);  // and the second time at once, before anything settles
        }
    }
    const size_t from = Messages().size();
    fx::Open(t::Media(L"clip30.mp4"));
    fx::WaitPlaying(from);
    CHECK_NEAR(Duration(), 6.0, 0.05);
    CHECK(fx::WaitFrameAt(0.3, 5000));
}

CORE_TEST(player_shuts_down_promptly_while_busy, 30000) {
    fx::Start(t::Media(L"tracks.mkv"));
    fx::WaitPlaying();
    SetPlaybackRate(2);
    DecodePreview(20'000'000);
    SeekTo(3.0, false);
    const double start = t::Now();
    ShutdownPlayer();
    CHECK(t::Now() - start < 3.0);
}

