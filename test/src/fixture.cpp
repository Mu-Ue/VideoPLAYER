#include "fixture.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mfmediaengine.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <memory>

#include "media/mkv.h"
#include "media/player.h"
#include "media/subtitles.h"

using Microsoft::WRL::ComPtr;

namespace fx {

namespace {

LRESULT CALLBACK Record(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg < WM_APP || msg > WM_APP + 64) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_APP_SUBTITLE_CUE) delete reinterpret_cast<Cue*>(lp);
    if (msg == WM_APP_SUBTITLE_CUES) delete reinterpret_cast<std::vector<Cue>*>(lp);
    Messages().push_back({msg, wp, lp, t::Now()});
    return 0;
}

}  // namespace

std::vector<Message>& Messages() {
    static std::vector<Message> messages;
    return messages;
}

HWND EventWindow() {
    static HWND hwnd = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Record;
        wc.lpszClassName = L"player-tests";
        RegisterClassW(&wc);
        // A real top-level window, never shown: the swap chain and composition don't need it to be.
        return CreateWindowW(L"player-tests", L"player tests", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr,
                             nullptr, nullptr);
    }();
    return hwnd;
}

void Silence() {
    // Windows keeps an app's session volume between runs, so it is set either way.
    const float level = GetEnvironmentVariableW(L"VIDEOPLAYER_TESTS_AUDIBLE", nullptr, 0) ? 1.0f : 0.0f;
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioSessionManager> sessions;
    ComPtr<ISimpleAudioVolume> volume;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devices))) &&
        SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_INPROC_SERVER, nullptr, &sessions)) &&
        SUCCEEDED(sessions->GetSimpleAudioVolume(nullptr, FALSE, &volume))) {
        volume->SetMasterVolume(level, nullptr);
    }
}

void Start(const std::wstring& path) {
    Silence();
    PrepareVideo(path);
    REQUIRE(InitPlayer(EventWindow()));
    SetVideoSize(320, 180);  // as the window does once it knows the size
    const HRESULT hr = OpenVideo(path);
    if (FAILED(hr)) t::Fail(__FILE__, __LINE__, "OpenVideo failed: " + std::to_string(hr));
    REQUIRE(SUCCEEDED(hr));
}

void Open(const std::wstring& path) {
    StopMkvSubtitles();
    const HRESULT hr = OpenVideo(path);
    if (FAILED(hr)) t::Fail(__FILE__, __LINE__, "OpenVideo failed: " + std::to_string(hr));
    REQUIRE(SUCCEEDED(hr));
}

int WaitMessage(UINT msg, int ms, size_t from) {
    int found = -1;
    t::PumpUntil(
        [&] {
            const auto& m = Messages();
            for (size_t i = from; i < m.size(); ++i) {
                if (m[i].msg == msg) {
                    found = int(i);
                    return true;
                }
            }
            return false;
        },
        ms);
    return found;
}

int WaitMedia(DWORD event, int ms, size_t from) {
    int found = -1;
    t::PumpUntil(
        [&] {
            const auto& m = Messages();
            for (size_t i = from; i < m.size(); ++i) {
                if (m[i].msg == WM_APP_MEDIA && m[i].wp == event) {
                    found = int(i);
                    return true;
                }
                if (m[i].msg == WM_APP_MEDIA && m[i].wp == MF_MEDIA_ENGINE_EVENT_ERROR && event != MF_MEDIA_ENGINE_EVENT_ERROR) {
                    return true;  // it won't come
                }
            }
            return false;
        },
        ms);
    return found;
}

void WaitPlaying(size_t from) {
    REQUIRE(WaitMedia(MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA, 10000, from) >= 0);
    const int first = WaitMedia(MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY, 10000, from);
    REQUIRE(first >= 0);
    OpenSeekHelpers();  // as the window does on the first frame
    REQUIRE(WaitMedia(MF_MEDIA_ENGINE_EVENT_PLAYING, 10000, from) >= 0);
}

void SeekAndWait(double seconds, bool fast) {
    const size_t from = Messages().size();
    SeekTo(SeekLanding(seconds, fast), fast);
    REQUIRE(WaitMedia(MF_MEDIA_ENGINE_EVENT_SEEKED, 10000, from) >= 0);
}

bool WaitFrameAt(double seconds, int ms) {
    return t::PumpUntil([&] { return FrameTime() >= seconds - 1e-6; }, ms);
}

struct Screen::Kit {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutput1> output;
    ComPtr<IDXGIOutputDuplication> dup;
    ComPtr<ID3D11Texture2D> last;  // a copy of the last desktop image acquired
    POINT origin{};
};

Screen::Screen() : kit_(new Kit) {}
Screen::~Screen() { delete kit_; }

bool Screen::Open(HWND window) {
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; !kit_->output && factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) == S_OK; ++o) {
            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);
            if (desc.Monitor != monitor) continue;
            kit_->origin = {desc.DesktopCoordinates.left, desc.DesktopCoordinates.top};
            output.As(&kit_->output);
            if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                         D3D11_SDK_VERSION, &kit_->device, nullptr, &kit_->context))) {
                return false;
            }
            break;
        }
    }
    return kit_->output && SUCCEEDED(kit_->output->DuplicateOutput(kit_->device.Get(), &kit_->dup));
}

bool Screen::Read(const RECT& rect, std::vector<BYTE>& pixels, int ms) {
    t::Pump(ms);
    Kit& k = *kit_;
    // The latest composition: all that came since the last read, or that one again if nothing changed.
    for (int wait = k.last ? 0 : 500;; wait = 0) {
        DXGI_OUTDUPL_FRAME_INFO info;
        ComPtr<IDXGIResource> resource;
        const HRESULT hr = k.dup->AcquireNextFrame(UINT(wait), &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) break;
        if (FAILED(hr)) return false;
        ComPtr<ID3D11Texture2D> desktop;
        if (SUCCEEDED(resource.As(&desktop))) {
            if (!k.last) {
                D3D11_TEXTURE2D_DESC d;
                desktop->GetDesc(&d);
                d.Usage = D3D11_USAGE_STAGING;
                d.BindFlags = 0;
                d.MiscFlags = 0;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                k.device->CreateTexture2D(&d, nullptr, &k.last);
            }
            if (k.last) k.context->CopyResource(k.last.Get(), desktop.Get());
        }
        k.dup->ReleaseFrame();
    }
    if (!k.last) return false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(k.context->Map(k.last.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    const UINT w = UINT(rect.right - rect.left), h = UINT(rect.bottom - rect.top);
    pixels.resize(size_t(w) * h * 4);
    for (UINT y = 0; y < h; ++y) {
        memcpy(pixels.data() + size_t(y) * w * 4,
               static_cast<const BYTE*>(m.pData) + size_t(rect.top - k.origin.y + LONG(y)) * m.RowPitch +
                   size_t(rect.left - k.origin.x) * 4,
               size_t(w) * 4);
    }
    k.context->Unmap(k.last.Get(), 0);
    return true;
}

int FrameNumber(const BYTE* bgra, UINT pitch, UINT w, UINT h) {
    const auto luma = [&](UINT x, UINT y) {
        const BYTE* p = bgra + size_t(y) * pitch + size_t(x) * 4;
        return (p[0] + 2 * p[1] + p[2]) / 4;
    };
    const UINT rows[2] = {UINT(h * 0.175), UINT(h * 0.225)};
    int frame = 0;
    for (UINT b = 0; b < 16; ++b) {
        const UINT x = UINT((b + 0.5) * w / 16);
        const bool one = luma(x, rows[0]) > 128, inverse = luma(x, rows[1]) > 128;
        if (one == inverse) return -1;
        if (one) frame |= 1 << b;
    }
    return frame;
}

bool HasWhiteText(const BYTE* bgra, UINT pitch, UINT w, UINT h) {
    int bright = 0;
    for (UINT y = h * 2 / 3; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            const BYTE* p = bgra + size_t(y) * pitch + size_t(x) * 4;
            bright += (p[0] + 2 * p[1] + p[2]) / 4 > 220;
        }
    }
    return bright > 20;
}

}  // namespace fx
