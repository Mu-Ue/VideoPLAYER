#pragma once

// What tests of the running player share: a hidden window that records what the player posts, starting the player
// for a file as the window does, waiting for its events, and reading the frame number a test video's picture
// carries.

#include <windows.h>

#include <string>
#include <vector>

#include "harness.h"

namespace fx {

struct Message {
    UINT msg;
    WPARAM wp;
    LPARAM lp;
    double at;  // t::Now()
};
// Everything posted to EventWindow(), in order. Cues are deleted once recorded.
std::vector<Message>& Messages();
HWND EventWindow();

// Starts the player for a first file, as the window does, with the sound silent; fails the test if it can't.
void Start(const std::wstring& path);
// Opens another file in the running player.
void Open(const std::wstring& path);

// Waits for the media engine event, or the message, to be posted after Messages()[from]; returns its index or -1.
int WaitMedia(DWORD event, int ms, size_t from = 0);
int WaitMessage(UINT msg, int ms, size_t from = 0);
// Waits for the file's metadata and first frame, and for it to play.
void WaitPlaying(size_t from = 0);
// Seeks and waits for the engine to say it landed.
void SeekAndWait(double seconds, bool fast);
// Waits until the frame on screen, as the player tells it, is at least at `seconds`.
bool WaitFrameAt(double seconds, int ms);

// The process's sound session at no volume: the player's sound still plays, and can be recorded, but isn't heard.
void Silence();

// The frame number a picture of the test media carries in its two rows of bits, or -1 if they don't read as one
// (a blend of two frames, or not one of the test videos). bgra is h rows of pitch bytes.
int FrameNumber(const BYTE* bgra, UINT pitch, UINT w, UINT h);
// Whether white text, a subtitle, shows in the picture's lower third, apart from the flash in the middle.
bool HasWhiteText(const BYTE* bgra, UINT pitch, UINT w, UINT h);

// What the screen shows, through Desktop Duplication: as composed, with the player's video and all over it.
class Screen {
  public:
    Screen();
    ~Screen();
    // Duplicates the output of the monitor the window is on; false if it can't (a locked or remote desktop).
    bool Open(HWND window);
    // The pixels of a rect of the screen, in screen coordinates, as last composed, after waiting ms for what was
    // asked of the window to show. BGRA, rows of 4 * width bytes.
    bool Read(const RECT& rect, std::vector<BYTE>& pixels, int ms = 300);

  private:
    struct Kit;
    Kit* kit_;
};

}  // namespace fx
