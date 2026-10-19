// What the player keeps between runs: the volume and where each video was left. The harness makes
// HKEY_CURRENT_USER a scratch key, emptied before each test.

#include <cmath>
#include <limits>
#include <string>

#include "config.h"
#include "harness.h"
#include "platform/settings.h"

namespace {

const wchar_t kKey[] = L"Software\\VideoPLAYER";

void SetRaw(const wchar_t* name, DWORD type, const void* data, DWORD size) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, type, data, size);
}

std::wstring Video(int i) { return L"C:\\Videos\\film " + std::to_wstring(i) + L".mkv"; }

}  // namespace

TEST(settings_default_to_full_volume_unmuted) {
    const Settings s = LoadSettings();
    CHECK_EQ(s.volume, 1.0);
    CHECK(!s.muted);
}

TEST(settings_round_trip) {
    for (const double v : {0.0, 0.05, 0.37, 1.0, 1.55, 2.0}) {
        NOTE("volume " + std::to_string(v));
        SaveSettings({v, true});
        const Settings s = LoadSettings();
        CHECK_NEAR(s.volume, v, 1e-9);
        CHECK(s.muted);
        SaveSettings({v, false});
        CHECK(!LoadSettings().muted);
    }
}

TEST(settings_volume_is_kept_in_range) {
    SaveSettings({7.5, false});
    CHECK_EQ(LoadSettings().volume, kMaxVolume);
    SaveSettings({-1, false});
    CHECK_EQ(LoadSettings().volume, 0.0);
    SaveSettings({0.123, false});
    CHECK_NEAR(LoadSettings().volume, 0.12, 1e-9);  // to the percent
}

TEST(settings_tampered_values_are_safe) {
    const DWORD huge = 0xFFFFFFFF;
    SetRaw(L"Volume", REG_DWORD, &huge, sizeof huge);
    CHECK_EQ(LoadSettings().volume, kMaxVolume);
    SetRaw(L"Volume", REG_SZ, L"50", 6);  // of another type: ignored
    CHECK_EQ(LoadSettings().volume, 1.0);
    const DWORD two = 2;
    SetRaw(L"Muted", REG_DWORD, &two, sizeof two);
    CHECK(LoadSettings().muted);
}

TEST(positions_round_trip) {
    CHECK_EQ(LoadPosition(Video(1)), 0.0);
    SavePosition(Video(1), 125.75);
    SavePosition(Video(2), 3);
    CHECK_EQ(LoadPosition(Video(1)), 125.75);
    CHECK_EQ(LoadPosition(Video(2)), 3.0);
    CHECK_EQ(LoadPosition(Video(3)), 0.0);
    SavePosition(Video(1), 200);  // replaces it
    CHECK_EQ(LoadPosition(Video(1)), 200.0);
}

TEST(positions_zero_or_less_forget) {
    SavePosition(Video(1), 10);
    SavePosition(Video(1), 0);
    CHECK_EQ(LoadPosition(Video(1)), 0.0);
    SavePosition(Video(1), 10);
    SavePosition(Video(1), -5);
    CHECK_EQ(LoadPosition(Video(1)), 0.0);
}

TEST(positions_match_the_same_file_however_named) {
    SavePosition(L"C:\\Videos\\Film.MKV", 42);
    CHECK_EQ(LoadPosition(L"c:\\videos\\film.mkv"), 42.0);
    CHECK_EQ(LoadPosition(L"C:/Videos/Film.mkv"), 42.0);
    CHECK_EQ(LoadPosition(L"C:\\Videos\\.\\sub\\..\\Film.mkv"), 42.0);
    // A relative path is the full path it names.
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    SavePosition(std::wstring(cwd) + L"\\here.mp4", 7);
    CHECK_EQ(LoadPosition(L"here.mp4"), 7.0);
    CHECK_EQ(LoadPosition(L"C:\\Videos\\Film2.mkv"), 0.0);
}

TEST(positions_of_long_paths) {
    const std::wstring path = L"C:\\" + std::wstring(300, L'd') + L"\\" + std::wstring(200, L'f') + L".mp4";
    const std::wstring other = L"C:\\" + std::wstring(300, L'd') + L"\\" + std::wstring(200, L'f') + L".mkv";
    SavePosition(path, 99);
    CHECK_EQ(LoadPosition(path), 99.0);
    CHECK_EQ(LoadPosition(other), 0.0);
}

TEST(positions_keep_the_most_recent_50) {
    for (int i = 0; i < 60; ++i) SavePosition(Video(i), i + 1.0);
    for (int i = 0; i < 10; ++i) CHECK_EQ(LoadPosition(Video(i)), 0.0);
    for (int i = 10; i < 60; ++i) CHECK_EQ(LoadPosition(Video(i)), i + 1.0);
    // Saving an old one again makes it the most recent: the next new one pushes out the oldest other.
    SavePosition(Video(10), 500);
    SavePosition(Video(100), 1);
    CHECK_EQ(LoadPosition(Video(10)), 500.0);
    CHECK_EQ(LoadPosition(Video(11)), 0.0);
    CHECK_EQ(LoadPosition(Video(12)), 13.0);
}

TEST(positions_hold_no_file_names) {
    SavePosition(L"C:\\Secret\\holiday.mp4", 5);
    BYTE data[4096];
    DWORD size = sizeof data, type = 0;
    REQUIRE(RegGetValueW(HKEY_CURRENT_USER, kKey, L"Positions", RRF_RT_ANY, &type, data, &size) == ERROR_SUCCESS);
    CHECK_EQ(type, DWORD(REG_BINARY));
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    CHECK(bytes.find("holiday") == std::string::npos);
    CHECK(std::wstring(reinterpret_cast<const wchar_t*>(data), size / 2).find(L"holiday") == std::wstring::npos);
}

TEST(positions_tampered_values_are_safe) {
    // Each entry is a hash and a time; bad times read as 0, odd sizes and oversized values as nothing kept.
    SavePosition(Video(1), 10);
    BYTE entry[16];
    DWORD size = sizeof entry;
    REQUIRE(RegGetValueW(HKEY_CURRENT_USER, kKey, L"Positions", RRF_RT_REG_BINARY, nullptr, entry, &size) == ERROR_SUCCESS);
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -50.0}) {
        memcpy(entry + 8, &bad, 8);
        SetRaw(L"Positions", REG_BINARY, entry, sizeof entry);
        CHECK_EQ(LoadPosition(Video(1)), 0.0);
    }
    SetRaw(L"Positions", REG_BINARY, entry, 15);  // cut short
    CHECK_EQ(LoadPosition(Video(1)), 0.0);
    SavePosition(Video(2), 3);  // and still works after
    CHECK_EQ(LoadPosition(Video(2)), 3.0);
    std::string big(16 * 80, '\x55');
    SetRaw(L"Positions", REG_BINARY, big.data(), DWORD(big.size()));
    CHECK_EQ(LoadPosition(Video(2)), 0.0);
    SavePosition(Video(3), 4);
    CHECK_EQ(LoadPosition(Video(3)), 4.0);
    SetRaw(L"Positions", REG_SZ, L"x", 4);
    CHECK_EQ(LoadPosition(Video(3)), 0.0);
}

TEST(the_registry_is_sandboxed) {
    // Writes go to the scratch key the harness made HKEY_CURRENT_USER, not the user's.
    SaveSettings({0.5, true});
    HKEY real;  // the user's own key, which the override doesn't reach
    REQUIRE(RegOpenCurrentUser(KEY_READ, &real) == ERROR_SUCCESS);
    const std::wstring sandboxed =
        L"Software\\VideoPLAYER-Tests\\" + std::to_wstring(GetCurrentProcessId()) + L"\\Software\\VideoPLAYER";
    DWORD value = 0, size = sizeof value;
    const LONG got = RegGetValueW(real, sandboxed.c_str(), L"Volume", RRF_RT_REG_DWORD, nullptr, &value, &size);
    RegCloseKey(real);
    CHECK_EQ(got, LONG(ERROR_SUCCESS));
    CHECK_EQ(value, DWORD(50));
    CHECK_EQ(LoadSettings().volume, 0.5);
}
