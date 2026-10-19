// The text drawn onto video frames, read back: subtitles burned in where and as asked.

#include <d3d11.h>
#include <wrl/client.h>

#include "fixture.h"
#include "ui/overlay.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> frame;
    UINT w = 640, h = 360;

    Gpu() {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) {
            SKIP("no Direct3D 11 device");
        }
        NewFrame();
    }

    // A fresh frame as the swap chain's: gray, as the test videos.
    void NewFrame() {
        D3D11_TEXTURE2D_DESC d{w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
        std::vector<UINT> gray(size_t(w) * h, 0xFF606060);
        D3D11_SUBRESOURCE_DATA init{gray.data(), w * 4};
        frame.Reset();
        REQUIRE(SUCCEEDED(device->CreateTexture2D(&d, &init, &frame)));
    }

    // The frame's pixels.
    std::vector<BYTE> Read() {
        D3D11_TEXTURE2D_DESC d;
        frame->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        REQUIRE(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &staging)));
        context->CopyResource(staging.Get(), frame.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        REQUIRE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)));
        std::vector<BYTE> out(size_t(w) * h * 4);
        for (UINT y = 0; y < h; ++y) memcpy(out.data() + size_t(y) * w * 4, static_cast<BYTE*>(m.pData) + size_t(y) * m.RowPitch, w * 4);
        context->Unmap(staging.Get(), 0);
        return out;
    }
};

// The box around pixels brighter (white text) or darker (its outline) than the gray.
struct Ink {
    RECT white{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN}, dark{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
    int whites = 0, darks = 0;
};

Ink Find(const std::vector<BYTE>& px, UINT w, UINT h) {
    Ink ink;
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            const BYTE* p = px.data() + (size_t(y) * w + x) * 4;
            const int l = (p[0] + 2 * p[1] + p[2]) / 4;
            RECT* r = l > 230 ? &ink.white : l < 40 ? &ink.dark : nullptr;
            if (!r) continue;
            (l > 230 ? ink.whites : ink.darks)++;
            r->left = std::min(r->left, LONG(x));
            r->top = std::min(r->top, LONG(y));
            r->right = std::max(r->right, LONG(x) + 1);
            r->bottom = std::max(r->bottom, LONG(y) + 1);
        }
    }
    return ink;
}

}  // namespace

CORE_TEST(overlay_draws_text_where_asked, 20000) {
    Gpu gpu;
    FramePainter painter;
    REQUIRE(painter.Open(gpu.device.Get()));
    const SIZE s = painter.Prepare(0, L"Hello there", {}, 28, 500);
    REQUIRE(s.cx > 50);
    REQUIRE(s.cy > 20);
    const POINT at[2] = {{100, 250}, {0, 0}};
    REQUIRE(painter.Draw(gpu.frame.Get(), at));
    const Ink ink = Find(gpu.Read(), gpu.w, gpu.h);
    CHECK(ink.whites > 100);
    CHECK(ink.darks > 50);  // its outline
    // All of it within the image placed at `at`.
    CHECK(ink.white.left >= 100 && ink.white.top >= 250);
    CHECK(ink.white.right <= 100 + s.cx && ink.white.bottom <= 250 + s.cy);
    CHECK(ink.dark.left >= 100 && ink.dark.right <= 100 + s.cx);
}

CORE_TEST(overlay_empty_text_draws_nothing, 20000) {
    Gpu gpu;
    FramePainter painter;
    REQUIRE(painter.Open(gpu.device.Get()));
    const SIZE s = painter.Prepare(0, L"", {}, 28, 500);
    CHECK_EQ(s.cx, LONG(0));
    const POINT at[2] = {{10, 10}, {10, 10}};
    CHECK(painter.Draw(gpu.frame.Get(), at));
    const Ink ink = Find(gpu.Read(), gpu.w, gpu.h);
    CHECK_EQ(ink.whites + ink.darks, 0);
}

CORE_TEST(overlay_top_and_bottom_slots_and_swap, 20000) {
    Gpu gpu;
    FramePainter painter;
    REQUIRE(painter.Open(gpu.device.Get()));
    painter.Prepare(0, L"bottom", {}, 24, 500);
    painter.Prepare(1, L"top", {}, 24, 500);
    const POINT at[2] = {{50, 300}, {50, 10}};
    REQUIRE(painter.Draw(gpu.frame.Get(), at));
    Ink ink = Find(gpu.Read(), gpu.w, gpu.h);
    CHECK(ink.white.top < 60);
    CHECK(ink.white.bottom > 300);
    // What is drawn ahead waits in slots 2 and 3 until swapped in.
    painter.Prepare(2, L"", {}, 24, 500);
    painter.Prepare(3, L"", {}, 24, 500);
    painter.DropTarget();  // as before resizing the frames
    gpu.NewFrame();
    REQUIRE(painter.Draw(gpu.frame.Get(), at));
    CHECK(Find(gpu.Read(), gpu.w, gpu.h).whites > 0);
    painter.Swap();
    painter.DropTarget();
    gpu.NewFrame();
    REQUIRE(painter.Draw(gpu.frame.Get(), at));
    CHECK_EQ(Find(gpu.Read(), gpu.w, gpu.h).whites, 0);
}

CORE_TEST(overlay_wraps_long_lines_and_keeps_styles, 20000) {
    Gpu gpu;
    FramePainter painter;
    REQUIRE(painter.Open(gpu.device.Get()));
    const std::wstring text = L"A rather long line of subtitle text that cannot fit on one line at this width";
    const SIZE wide = painter.Prepare(0, text, {}, 24, 2000);
    const SIZE narrow = painter.Prepare(0, text, {}, 24, 300);
    CHECK(narrow.cx <= 300 + 16);  // the outline reaches a little past
    CHECK(narrow.cy > wide.cy * 2);
    const std::vector<StyleSpan> spans{{0, 8, true, false}, {9, 4, false, true}, {70, 500, true, true}};
    const SIZE styled = painter.Prepare(0, text, spans, 24, 300);  // a span past the end is clipped, not a crash
    CHECK(styled.cx > 0);
    const SIZE lines = painter.Prepare(1, L"one\ntwo\nthree", {}, 24, 2000);
    const SIZE line = painter.Prepare(1, L"one", {}, 24, 2000);
    CHECK(lines.cy > line.cy * 2);
}

CORE_TEST(overlay_text_widths, 20000) {
    Gpu gpu;
    SetOverlayDevice(gpu.device.Get());  // as the window does before drawing
    const float a = TrackTextWidth(L"English", 1, false), b = TrackTextWidth(L"English \x2013 Commentary", 1, false);
    CHECK(a > 10);
    CHECK(b > a);
    CHECK_NEAR(TrackTextWidth(L"English", 2, false), 2 * a, a * 0.15);
    CHECK_EQ(TrackTextWidth(L"", 1, false), 0.0f);
}
