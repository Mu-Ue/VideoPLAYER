#pragma once

// A small test harness, with no dependencies, for the player's own sources.
//
// Every test runs in a child process of the runner, so a crash, an AddressSanitizer report or a hang fails that
// test and the run goes on. Unit tests share one child, one after another; core tests, which start Media
// Foundation, the GPU and threads that live for the whole process, each get one of their own. In each child,
// HKEY_CURRENT_USER is a scratch key, so no test reads or writes the user's settings or saved positions.

#include <windows.h>

#include <cmath>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace t {

enum Tier { kUnit, kCore };

struct Case {
    const char* name;
    Tier tier;
    void (*fn)();
    int timeoutMs;
};
std::vector<Case>& Cases();

struct Register {
    Register(const char* name, Tier tier, void (*fn)(), int timeoutMs) { Cases().push_back({name, tier, fn, timeoutMs}); }
};

struct Abort {};  // a REQUIRE failed: the test stops there
struct Skip {
    std::string why;
};

std::string Utf8(std::wstring_view w);
// UTF-8 with control characters escaped, for messages.
std::string Escaped(std::wstring_view w);
std::string Escaped(std::string_view s);

template <class T>
std::string Show(const T& v) {
    using U = std::decay_t<T>;
    if constexpr (std::is_same_v<U, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_same_v<U, std::wstring> || std::is_same_v<U, std::wstring_view> ||
                         std::is_same_v<U, const wchar_t*> || std::is_same_v<U, wchar_t*>) {
        return "L\"" + Escaped(std::wstring_view(v)) + "\"";
    } else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view> ||
                         std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
        return "\"" + Escaped(std::string_view(v)) + "\"";
    } else if constexpr (std::is_enum_v<U>) {
        return std::to_string(static_cast<long long>(v));
    } else if constexpr (std::is_floating_point_v<U>) {
        char s[64];
        sprintf_s(s, "%.9g", double(v));
        return s;
    } else if constexpr (std::is_arithmetic_v<U>) {
        return std::to_string(v);
    } else if constexpr (std::is_pointer_v<U>) {
        char s[32];
        sprintf_s(s, "%p", static_cast<const void*>(v));
        return s;
    } else {
        return "(value)";
    }
}

// Records a failure of the running test; it goes on unless the caller throws Abort.
void Fail(const char* file, int line, const std::string& what);

// Marks the running test as showing a bug not yet fixed: its failure is reported as XFAIL and doesn't fail the
// run, while passing does (XPASS), so the mark goes once the bug does.
void KnownBug(const char* what);

// Context printed with each failure while it is in scope, such as the seed of a fuzz case.
class Note {
  public:
    explicit Note(std::string text);
    ~Note();
    Note(const Note&) = delete;
    Note& operator=(const Note&) = delete;
};

template <class A, class B>
bool CheckEq(const char* file, int line, const char* ta, const char* tb, const A& a, const B& b) {
    if (a == b) return true;
    Fail(file, line, std::string("CHECK_EQ(") + ta + ", " + tb + "): " + Show(a) + " != " + Show(b));
    return false;
}

inline bool CheckNear(const char* file, int line, const char* ta, const char* tb, double a, double b, double tol) {
    if (std::abs(a - b) <= tol) return true;
    Fail(file, line,
         std::string("CHECK_NEAR(") + ta + ", " + tb + "): " + Show(a) + " vs " + Show(b) + ", off by " +
             Show(std::abs(a - b)) + " > " + Show(tol));
    return false;
}

// Paths: the test folder with its media, and a scratch folder of this process, emptied when it exits.
std::wstring Media(const std::wstring& name);
std::wstring TempDir();
std::wstring WriteTemp(const std::wstring& name, const std::string& bytes);

// Seconds on the performance counter.
double Now();

// Pumps this thread's messages until done() or ms pass; returns done().
bool PumpUntil(const std::function<bool()>& done, int ms);
void Pump(int ms);

// The texts of the message boxes shown since the last call, each closed at once with OK. A test that doesn't take
// one fails, as the player showed an error it didn't expect.
std::vector<std::wstring> TakeDialogs();
// Waits for a message box to show, pumping messages, and returns its text, or an empty string.
std::wstring WaitDialog(int ms);

// Empties the scratch HKEY_CURRENT_USER.
void ResetRegistry();

// Whether this machine can run tests that need a GPU with a hardware video decoder, an audio output, or a desktop
// to capture; tests skip without.
bool HasAudioOutput();

}  // namespace t

#define T_CAT2(a, b) a##b
#define T_CAT(a, b) T_CAT2(a, b)

// A unit test: pure logic, or files and the registry; shares a process with the others.
#define TEST(name)                                                                     \
    static void name();                                                               \
    static const t::Register T_CAT(name, _registered)(#name, t::kUnit, name, 20000); \
    static void name()
// A unit test with a time limit of its own, in ms.
#define TEST_TIMED(name, ms)                                                        \
    static void name();                                                            \
    static const t::Register T_CAT(name, _registered)(#name, t::kUnit, name, ms); \
    static void name()
// A test of the running player: Media Foundation, the GPU, the audio output or the window. Runs in a process of its
// own, which it may leave in any state, killed after ms.
#define CORE_TEST(name, ms)                                                         \
    static void name();                                                            \
    static const t::Register T_CAT(name, _registered)(#name, t::kCore, name, ms); \
    static void name()

#define CHECK(c)                                          \
    do {                                                  \
        if (!(c)) t::Fail(__FILE__, __LINE__, "CHECK(" #c ")"); \
    } while (0)
#define REQUIRE(c)                                              \
    do {                                                        \
        if (!(c)) {                                             \
            t::Fail(__FILE__, __LINE__, "REQUIRE(" #c ")");     \
            throw t::Abort{};                                   \
        }                                                       \
    } while (0)
#define CHECK_EQ(a, b) t::CheckEq(__FILE__, __LINE__, #a, #b, (a), (b))
#define REQUIRE_EQ(a, b)                                                   \
    do {                                                                   \
        if (!t::CheckEq(__FILE__, __LINE__, #a, #b, (a), (b))) throw t::Abort{}; \
    } while (0)
#define CHECK_NEAR(a, b, tol) t::CheckNear(__FILE__, __LINE__, #a, #b, double(a), double(b), double(tol))
#define REQUIRE_NEAR(a, b, tol)                                                               \
    do {                                                                                      \
        if (!t::CheckNear(__FILE__, __LINE__, #a, #b, double(a), double(b), double(tol))) throw t::Abort{}; \
    } while (0)
#define SKIP(why) throw t::Skip{why}
#define KNOWN_BUG(what) t::KnownBug(what)
#define NOTE(text) const t::Note T_CAT(note_, __LINE__)(text)
