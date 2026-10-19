// The player's sound, recorded from the output as this process plays it (process loopback): which track plays, at
// what pitch and level, in step with the picture. The recording is of the sound as heard, after the session's and
// the system's volume, so these tests are heard too and run only when asked for (tests --audible). Levels are
// told against the full level as recorded, so the system's volume doesn't matter.

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mfmediaengine.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

#include "config.h"
#include "fixture.h"
#include "media/player.h"

using Microsoft::WRL::ComPtr;
using fx::Messages;

namespace {

class Activated final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
  public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Activated() { CloseHandle(done); }
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *object = static_cast<IAgileObject*>(this);
        } else {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done);
        return S_OK;
    }
};

constexpr UINT32 kRate = 48000, kChannels = 2;

// Records this process's sound, as 48 kHz stereo, each frame with its performance counter time.
class Recorder {
  public:
    bool Start() {
        AUDIOCLIENT_ACTIVATION_PARAMS params{AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK};
        params.ProcessLoopbackParams = {GetCurrentProcessId(), PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE};
        PROPVARIANT activation{};
        activation.vt = VT_BLOB;
        activation.blob = {sizeof(params), reinterpret_cast<BYTE*>(&params)};
        Activated handler;
        ComPtr<IActivateAudioInterfaceAsyncOperation> op;
        ComPtr<IUnknown> unknown;
        HRESULT result = E_FAIL;
        WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT, kChannels, kRate, kRate * kChannels * 4, kChannels * 4, 32, 0};
        if (FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                               &activation, &handler, &op)) ||
            WaitForSingleObject(handler.done, 5000) != WAIT_OBJECT_0 || FAILED(op->GetActivateResult(&result, &unknown)) ||
            FAILED(result) || FAILED(unknown.As(&client_)) ||
            FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                       2'000'000, 0, &format, nullptr)) ||
            FAILED(client_->GetService(IID_PPV_ARGS(&capture_))) || FAILED(client_->Start())) {
            return false;
        }
        thread_ = std::thread([this] { Loop(); });
        return true;
    }
    ~Recorder() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (client_) client_->Stop();
    }

    // The left channel recorded between two performance counter times, in seconds.
    std::vector<float> Between(double from, double to) {
        std::lock_guard l(lock_);
        std::vector<float> out;
        for (size_t i = 0; i < times_.size(); ++i) {
            if (times_[i] >= from && times_[i] < to) out.push_back(left_[i]);
        }
        return out;
    }
    // Records for a while, from now, and returns that.
    std::vector<float> Next(double seconds) {
        const double from = t::Now();
        t::Pump(int(seconds * 1000) + 150);  // what the output holds comes late
        return Between(from + 0.1, from + seconds + 0.1);
    }
    // The times sound starts after a silence of at least `gap` seconds: each click's onset.
    std::vector<double> Onsets(double from, double to, double gap = 0.3) {
        std::lock_guard l(lock_);
        std::vector<double> out;
        double last = -1e9;
        for (size_t i = 0; i < times_.size(); ++i) {
            if (times_[i] < from || times_[i] >= to || std::abs(left_[i]) < 0.05) continue;
            if (times_[i] - last > gap) out.push_back(times_[i]);
            last = times_[i];
        }
        return out;
    }

  private:
    void Loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        while (!stop_) {
            Sleep(2);
            UINT32 packet = 0;
            while (SUCCEEDED(capture_->GetNextPacketSize(&packet)) && packet) {
                BYTE* data;
                UINT32 frames;
                DWORD flags;
                UINT64 position, qpc;  // qpc in 100 ns units
                if (FAILED(capture_->GetBuffer(&data, &frames, &flags, &position, &qpc))) break;
                std::lock_guard l(lock_);
                const float* s = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < frames; ++i) {
                    times_.push_back((double(qpc) + i * 1e7 / kRate) / 1e7);
                    left_.push_back(flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0.0f : s[size_t(i) * kChannels]);
                }
                capture_->ReleaseBuffer(frames);
            }
        }
    }

    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    std::thread thread_;
    std::atomic<bool> stop_ = false;
    std::mutex lock_;
    std::vector<double> times_;
    std::vector<float> left_;
};

double Rms(const std::vector<float>& s) {
    double sum = 0;
    for (float v : s) sum += double(v) * v;
    return s.empty() ? 0 : std::sqrt(sum / double(s.size()));
}

// The strength of one frequency in the sound (Goertzel), normalized to its amplitude.
double Tone(const std::vector<float>& s, double hz) {
    const double w = 2 * 3.14159265358979 * hz / kRate, c = 2 * std::cos(w);
    double a = 0, b = 0;
    for (float v : s) {
        const double n = v + c * a - b;
        b = a;
        a = n;
    }
    return s.empty() ? 0 : 2 * std::sqrt(std::max(0.0, a * a + b * b - c * a * b)) / double(s.size());
}

// The pitch among candidates that is clearly strongest, or 0.
double Pitch(const std::vector<float>& s) {
    double best = 0, top = 0, second = 0;
    for (const double hz : {220.0, 330.0, 440.0, 660.0, 880.0, 1760.0}) {
        const double v = Tone(s, hz);
        if (v > top) second = top, top = v, best = hz;
        else if (v > second) second = v;
    }
    return top > 4 * second && top > 1e-4 ? best : 0;
}

// The player started on a file, with a recorder of its sound; skips if this machine has no sound output.
std::unique_ptr<Recorder> StartRecording(const std::wstring& file, bool picture = true) {
    if (!GetEnvironmentVariableW(L"VIDEOPLAYER_TESTS_AUDIBLE", nullptr, 0)) SKIP("plays tones out loud: run with --audible");
    if (!t::HasAudioOutput()) SKIP("no audio output");
    auto r = std::make_unique<Recorder>();
    if (!r->Start()) SKIP("process loopback recording isn't available");
    fx::Start(t::Media(file));
    if (picture) {
        fx::WaitPlaying();
    } else {  // no first frame comes
        REQUIRE(fx::WaitMedia(MF_MEDIA_ENGINE_EVENT_PLAYING, 10000) >= 0);
    }
    return r;
}

}  // namespace

CORE_TEST(audio_plays_the_file_sound, 30000) {
    auto rec = StartRecording(L"clip30.mp4");
    t::Pump(300);
    const auto s = rec->Next(1.0);
    CHECK(s.size() > kRate * 0.9);
    CHECK(Rms(s) > 1e-4);
    CHECK_EQ(Pitch(s), 440.0);
}

CORE_TEST(audio_switches_tracks, 40000) {
    auto rec = StartRecording(L"tracks.mkv");
    t::Pump(300);
    CHECK_EQ(Pitch(rec->Next(0.8)), 440.0);
    // As the window does: select, then seek into the frame before, as one within the frame does nothing, so the
    // source starts again and reads it.
    SelectAudioTrack(1);
    fx::SeekAndWait(FrameTime() - FrameGap() / 2, false);
    t::Pump(300);
    CHECK_EQ(Pitch(rec->Next(0.8)), 880.0);
    SelectAudioTrack(0);
    fx::SeekAndWait(1.0, false);
    t::Pump(300);
    CHECK_EQ(Pitch(rec->Next(0.8)), 440.0);
}

CORE_TEST(audio_keeps_its_pitch_at_every_speed, 60000) {
    auto rec = StartRecording(L"clip30.mp4");
    for (const double rate : {2.0, 0.5, 1.25, 1.0}) {
        NOTE("rate " + std::to_string(rate));
        fx::SeekAndWait(0.5, false);
        SetPlaybackRate(rate);
        t::Pump(400);
        const auto s = rec->Next(0.6);
        CHECK_EQ(Pitch(s), 440.0);
    }
}

CORE_TEST(audio_follows_volume_and_mute, 40000) {
    auto rec = StartRecording(L"clip30.mp4");
    SetVolume(1);
    t::Pump(300);
    const double full = Rms(rec->Next(0.5));
    REQUIRE(full > 1e-4);
    SetVolume(0.5);
    t::Pump(200);
    CHECK_NEAR(Rms(rec->Next(0.5)) / full, 0.5, 0.06);
    SetVolume(kMaxVolume);  // boosted past full
    t::Pump(200);
    CHECK_NEAR(Rms(rec->Next(0.5)) / full, kMaxVolume, 0.2);
    ToggleMute();
    t::Pump(200);
    CHECK(Rms(rec->Next(0.5)) < full * 0.01);
    ToggleMute();
    SetVolume(0);
    t::Pump(200);
    CHECK(Rms(rec->Next(0.5)) < full * 0.01);
    SetVolume(1);
    t::Pump(200);
    CHECK_NEAR(Rms(rec->Next(0.5)) / full, 1.0, 0.06);
}

CORE_TEST(audio_sound_only_file_plays_and_follows_volume, 30000) {
    // Its sound plays in the engine, not apart from it like a video's.
    auto rec = StartRecording(L"audio.m4a", false);
    SetVolume(1);
    t::Pump(300);
    const auto s = rec->Next(0.5);
    CHECK_EQ(Pitch(s), 440.0);
    const double full = Rms(s);
    REQUIRE(full > 1e-4);
    SetVolume(0.5);
    t::Pump(200);
    CHECK_NEAR(Rms(rec->Next(0.5)) / full, 0.5, 0.06);
    ToggleMute();
    t::Pump(200);
    CHECK(Rms(rec->Next(0.5)) < full * 0.01);
    ToggleMute();
    SetVolume(1);
    t::Pump(200);
    CHECK_NEAR(Rms(rec->Next(0.5)) / full, 1.0, 0.06);
}

CORE_TEST(audio_stops_when_paused, 30000) {
    auto rec = StartRecording(L"clip30.mp4");
    t::Pump(300);
    const double full = Rms(rec->Next(0.5));
    SetPlaying(false);
    t::Pump(300);
    CHECK(Rms(rec->Next(0.5)) < full * 0.01);
    SetPlaying(true);
    t::Pump(300);
    CHECK_EQ(Pitch(rec->Next(0.5)), 440.0);
}

CORE_TEST(audio_in_step_with_the_picture, 60000) {
    // Clicks on the frames at each 2 s: the picture's clock reads that time as each click is heard. Coarse, to
    // catch sound that runs off from the picture, as after a seek; tools\bench measures it to the millisecond.
    for (const wchar_t* file : {L"av_pcm.mkv", L"av_aac.mp4"}) {
        NOTE(t::Utf8(file));
        static std::unique_ptr<Recorder> rec;
        const size_t from = Messages().size();
        if (!rec) {
            rec = StartRecording(file);
        } else {
            fx::Open(t::Media(file));
            fx::WaitPlaying(from);
        }
        for (const double seekTo : {0.0, 3.3, 5.1}) {
            NOTE("from " + std::to_string(seekTo));
            fx::SeekAndWait(seekTo, false);
            // The picture's time against the counter, as it plays.
            std::vector<std::pair<double, double>> clock;
            const double start = t::Now();
            t::PumpUntil([&] {
                clock.push_back({t::Now(), PlaybackTime()});
                return t::Now() - start > 2.6;
            }, 4000);
            t::Pump(150);
            int checked = 0;
            for (const double onset : rec->Onsets(start + 0.3, start + 2.5)) {
                // The picture's time at the click, between the two readings around it.
                const auto after = std::find_if(clock.begin(), clock.end(), [&](const auto& c) { return c.first >= onset; });
                if (after == clock.begin() || after == clock.end()) continue;
                const auto before = after - 1;
                const double at = before->second + (onset - before->first) / (after->first - before->first) * (after->second - before->second);
                const double click = std::round(at / 2) * 2;
                NOTE("click at " + std::to_string(click));
                CHECK_NEAR(at, click, 0.06);
                ++checked;
            }
            CHECK(checked >= 1);
        }
    }
}

CORE_TEST(audio_surround_plays, 30000) {
    for (const wchar_t* file : {L"surround.mkv", L"surround.mp4"}) {
        NOTE(t::Utf8(file));
        static std::unique_ptr<Recorder> rec;
        const size_t from = Messages().size();
        if (!rec) {
            rec = StartRecording(file);
        } else {
            fx::Open(t::Media(file));
            fx::WaitPlaying(from);
        }
        t::Pump(300);
        const auto s = rec->Next(0.6);
        CHECK(Rms(s) > 1e-4);
        CHECK_EQ(Pitch(s), 440.0);
    }
}
