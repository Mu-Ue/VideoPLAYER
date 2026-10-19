#include "harness.h"

#include <mmdeviceapi.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "paths.h"  // TEST_DIR, made by CMake

namespace t {

namespace {

struct Running {
    const Case* test = nullptr;
    int failures = 0;
    std::vector<std::string> notes;
    std::string knownBug;
} running;

std::wstring tempDir;
HKEY sandbox = nullptr;

void Print(const std::string& s) {
    fwrite(s.data(), 1, s.size(), stdout);
    fflush(stdout);
}

}  // namespace

std::vector<Case>& Cases() {
    static std::vector<Case> cases;
    return cases;
}

std::string Utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Escaped(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c == '\0') out += "\\0";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

std::string Escaped(std::wstring_view w) { return Escaped(std::string_view(Utf8(w))); }

void Fail(const char* file, int line, const std::string& what) {
    ++running.failures;
    const char* name = strrchr(file, '\\');
    std::string text = "  " + std::string(name ? name + 1 : file) + "(" + std::to_string(line) + "): " + what + "\n";
    for (const std::string& n : running.notes) text += "    with " + n + "\n";
    Print(text);
}

void KnownBug(const char* what) { running.knownBug = what; }

Note::Note(std::string text) { running.notes.push_back(std::move(text)); }
Note::~Note() { running.notes.pop_back(); }

std::wstring Media(const std::wstring& name) { return std::wstring(TEST_DIR) + L"\\media\\" + name; }

std::wstring TempDir() {
    if (tempDir.empty()) {
        wchar_t base[MAX_PATH];
        GetTempPathW(MAX_PATH, base);
        tempDir = std::wstring(base) + L"VideoPLAYER-tests\\" + std::to_wstring(GetCurrentProcessId());
        std::filesystem::create_directories(tempDir);
    }
    return tempDir;
}

std::wstring WriteTemp(const std::wstring& name, const std::string& bytes) {
    const std::wstring path = TempDir() + L"\\" + name;
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Fail(__FILE__, __LINE__, "can't write " + Utf8(path));
        throw Abort{};
    }
    DWORD written = 0;
    WriteFile(f, bytes.data(), DWORD(bytes.size()), &written, nullptr);
    CloseHandle(f);
    return path;
}

double Now() {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / frequency;
}

bool PumpUntil(const std::function<bool()>& done, int ms) {
    const double end = Now() + ms / 1000.0;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (done()) return true;
        const double left = end - Now();
        if (left <= 0) return false;
        MsgWaitForMultipleObjectsEx(0, nullptr, DWORD(std::min(left * 1000, 5.0)) + 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

void Pump(int ms) { PumpUntil([] { return false; }, ms); }

void ResetRegistry() {
    if (sandbox) RegDeleteTreeW(sandbox, nullptr);
}

bool HasAudioOutput() {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> devices;
    Microsoft::WRL::ComPtr<IMMDevice> device;
    return SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&devices))) &&
           SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device));
}

namespace {

constexpr wchar_t kSandboxRoot[] = L"Software\\VideoPLAYER-Tests";
constexpr wchar_t kRealKey[] = L"Software\\VideoPLAYER";

// --- The child: runs tests and reports each on stdout ---

// HKEY_CURRENT_USER becomes a key of this process's own for the rest of its life.
void SandboxRegistry() {
    const std::wstring path = std::wstring(kSandboxRoot) + L"\\" + std::to_wstring(GetCurrentProcessId());
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &sandbox, nullptr) !=
            ERROR_SUCCESS ||
        RegOverridePredefKey(HKEY_CURRENT_USER, sandbox) != ERROR_SUCCESS) {
        Print("@@FATAL can't sandbox HKEY_CURRENT_USER\n");
        ExitProcess(4);
    }
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* e) {
    char s[160];
    sprintf_s(s, "\n@@CRASH exception 0x%08lX at %p\n", e->ExceptionRecord->ExceptionCode,
              e->ExceptionRecord->ExceptionAddress);
    Print(s);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Message boxes the player shows: closed at once, so no test waits on a click, and kept for the test to check.
std::mutex dialogLock;
std::vector<std::wstring> dialogs;
std::atomic<bool> watching = true;

void WatchDialogs() {
    while (watching) {
        Sleep(30);
        EnumWindows(
            [](HWND h, LPARAM) -> BOOL {
                DWORD pid = 0;
                wchar_t cls[16];
                GetWindowThreadProcessId(h, &pid);
                if (pid != GetCurrentProcessId() || !GetClassNameW(h, cls, 16) || wcscmp(cls, L"#32770") != 0 ||
                    !IsWindowVisible(h)) {
                    return TRUE;
                }
                // Kept once, though it may take a few rounds to close (a busy or slow machine).
                if (!GetPropW(h, L"VideoPLAYER-Tests-seen")) {
                    SetPropW(h, L"VideoPLAYER-Tests-seen", HANDLE(1));
                    wchar_t text[1024] = L"";
                    GetDlgItemTextW(h, 0xFFFF, text, 1024);  // a message box's message
                    std::lock_guard l(dialogLock);
                    dialogs.push_back(text);
                }
                // Its button, clicked; a message box takes no WM_COMMAND posted to itself.
                if (const HWND button = FindWindowExW(h, nullptr, L"Button", nullptr)) {
                    PostMessageW(button, BM_CLICK, 0, 0);
                } else {
                    PostMessageW(h, WM_CLOSE, 0, 0);
                }
                return TRUE;
            },
            0);
    }
}

}  // namespace

std::vector<std::wstring> TakeDialogs() {
    std::lock_guard l(dialogLock);
    return std::exchange(dialogs, {});
}

std::wstring WaitDialog(int ms) {
    std::wstring text;
    PumpUntil(
        [&] {
            std::lock_guard l(dialogLock);
            if (dialogs.empty()) return false;
            text = dialogs.front();
            dialogs.erase(dialogs.begin());
            return true;
        },
        ms);
    return text;
}

namespace {

void RunOne(const Case& c) {
    Print(std::string("@@RUN ") + c.name + "\n");
    running = {&c};
    ResetRegistry();
    const double start = Now();
    std::string verdict = "PASS", why;
    try {
        c.fn();
    } catch (const Abort&) {
    } catch (const Skip& s) {
        verdict = "SKIP";
        why = s.why;
    } catch (const std::exception& e) {
        Fail(__FILE__, __LINE__, std::string("exception: ") + e.what());
    } catch (...) {
        Fail(__FILE__, __LINE__, "unknown exception");
    }
    {
        std::lock_guard l(dialogLock);
        for (const std::wstring& d : dialogs) Fail(__FILE__, __LINE__, "unexpected message box: " + Utf8(d));
        dialogs.clear();
    }
    if (running.failures) verdict = "FAIL";
    if (!running.knownBug.empty() && verdict != "SKIP") {
        why = running.knownBug;
        if (verdict == "FAIL") {
            verdict = "XFAIL";
        } else {
            verdict = "XPASS";
            Print("  passes now: the bug seems fixed, so take its KNOWN_BUG mark off\n");
        }
    }
    char s[64];
    sprintf_s(s, " %.0f", (Now() - start) * 1000);
    Print("@@END " + std::string(c.name) + " " + verdict + s + (why.empty() ? "" : " " + why) + "\n");
}

int ChildMain(const std::vector<std::string>& names) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(OnCrash);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, [](int) {
        Print("\n@@CRASH abort()\n");
        TerminateProcess(GetCurrentProcess(), 3);
    });
    std::set_terminate([] {
        Print("\n@@CRASH std::terminate\n");
        TerminateProcess(GetCurrentProcess(), 3);
    });
    SandboxRegistry();
    // As the player's main thread does.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 4;
    std::thread watcher(WatchDialogs);
    for (const std::string& name : names) {
        const auto it = std::find_if(Cases().begin(), Cases().end(), [&](const Case& c) { return name == c.name; });
        if (it != Cases().end()) RunOne(*it);
    }
    watching = false;
    watcher.join();
    // Core tests leave the player's threads as they are, as the player exits without stopping them either.
    fflush(stdout);
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    std::error_code ignored;
    std::filesystem::remove_all(TempDir(), ignored);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

// --- The runner ---

struct Result {
    std::string verdict;  // PASS, FAIL, SKIP, CRASH or HANG
    double ms = 0;
    std::string why, output;
};

// The values under a key, to tell whether a run changed them.
std::map<std::wstring, std::pair<DWORD, std::string>> Snapshot(const wchar_t* key) {
    std::map<std::wstring, std::pair<DWORD, std::string>> values;
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return values;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD nameLen = 256, type = 0, size = 1 << 16;
        std::string data(size, '\0');
        if (RegEnumValueW(h, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &size) !=
            ERROR_SUCCESS) {
            break;
        }
        data.resize(size);
        values[name] = {type, data};
    }
    RegCloseKey(h);
    return values;
}

void Restore(const wchar_t* key, const std::map<std::wstring, std::pair<DWORD, std::string>>& values) {
    RegDeleteTreeW(HKEY_CURRENT_USER, key);
    for (const auto& [name, v] : values) {
        RegSetKeyValueW(HKEY_CURRENT_USER, key, name.c_str(), v.first, v.second.data(), DWORD(v.second.size()));
    }
}

// Runs tests in a child, one after another, killing it if one takes longer than its time limit. Returns the results
// of those that ran; one that crashed or hung ends the child, and the caller starts another for the rest.
class Child {
  public:
    explicit Child(const std::vector<const Case*>& tests, double timeoutScale) : tests_(tests), scale_(timeoutScale) {}

    // Runs the tests from `from` on, returning how far it got.
    size_t Run(size_t from, std::map<std::string, Result>& results) {
        std::wstring cmd = L"\"" + Exe() + L"\" --child";
        for (size_t i = from; i < tests_.size(); ++i) cmd += L" " + std::wstring(tests_[i]->name, tests_[i]->name + strlen(tests_[i]->name));
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE readPipe, writePipe;
        CreatePipe(&readPipe, &writePipe, &sa, 1 << 16);
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = writePipe;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        const HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
            fprintf(stderr, "can't start a test process\n");
            ExitProcess(4);
        }
        AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        CloseHandle(writePipe);

        std::thread reader([&] {
            std::string pending;
            char buf[4096];
            DWORD n;
            while (ReadFile(readPipe, buf, sizeof buf, &n, nullptr) && n) {
                pending.append(buf, n);
                for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                    Push(pending.substr(0, nl + 1));
                    pending.erase(0, nl + 1);
                }
            }
            if (!pending.empty()) Push(pending + "\n");
            Push({});  // the end
        });

        size_t at = from;
        std::string current, output;
        double deadline = Now() + 30;  // until the first test starts
        bool ended = false;
        while (!ended) {
            std::string line;
            if (!Pop(line, deadline)) {  // timed out
                TerminateJobObject(job, 5);
                if (!current.empty()) {
                    results[current] = {"HANG", 0, "", output + "  killed after its time limit\n"};
                    ++at;
                }
                break;
            }
            if (line.empty()) {
                ended = true;
                break;
            }
            if (line.rfind("@@RUN ", 0) == 0) {
                current = Trim(line.substr(6));
                output.clear();
                deadline = Now() + Timeout(current);
            } else if (line.rfind("@@END ", 0) == 0) {
                // @@END name VERDICT ms [why]
                const std::string rest = Trim(line.substr(6));
                const size_t a = rest.find(' '), b = rest.find(' ', a + 1), c = rest.find(' ', b + 1);
                Result r{rest.substr(a + 1, b - a - 1), atof(rest.substr(b + 1, c - b - 1).c_str()),
                         c == std::string::npos ? "" : rest.substr(c + 1), output};
                results[current] = r;
                current.clear();
                ++at;
                deadline = Now() + 30;
            } else {
                output += line;
            }
        }
        if (ended) WaitForSingleObject(pi.hProcess, 10000);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        if (!current.empty() && !results.count(current)) {  // died within a test
            char s[64];
            sprintf_s(s, "  the process died with exit code 0x%08lX\n", code);
            results[current] = {"CRASH", 0, "", output + s};
            ++at;
        }
        TerminateJobObject(job, 5);
        reader.join();
        CloseHandle(readPipe);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        return at;
    }

  private:
    static std::wstring Exe() {
        wchar_t exe[MAX_PATH * 4];
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        return exe;
    }
    static std::string Trim(std::string s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        return s;
    }
    double Timeout(const std::string& name) const {
        for (const Case* c : tests_) {
            if (name == c->name) return c->timeoutMs * scale_ / 1000.0;
        }
        return 30;
    }
    void Push(std::string line) {
        std::lock_guard l(lock_);
        lines_.push_back(std::move(line));
        ready_.notify_one();
    }
    bool Pop(std::string& line, double deadline) {
        std::unique_lock l(lock_);
        while (lines_.empty()) {
            const double left = deadline - Now();
            if (left <= 0) return false;
            ready_.wait_for(l, std::chrono::milliseconds(int(left * 1000) + 1));
        }
        line = std::move(lines_.front());
        lines_.pop_front();
        return true;
    }

    std::vector<const Case*> tests_;
    double scale_;
    std::mutex lock_;
    std::condition_variable ready_;
    std::deque<std::string> lines_;
};

void Usage() {
    printf(
        "tests [filters...] [--unit | --core] [--audible] [--list] [--repeat N] [--timeout-scale X] [--verbose]\n"
        "  filters  run the tests whose names contain any of them; -text leaves out those that contain text\n"
        "  --unit   only the unit tests, which take a second or two\n"
        "  --core   only the tests of the running player: Media Foundation, the GPU, sound and the window\n"
        "  --repeat runs the selection N times over, to catch flaky ones\n"
        "  --audible also runs the tests that record the player's sound, which play tones out loud\n");
}

int RunnerMain(int argc, char** argv) {
    std::vector<std::string> include, exclude;
    bool unit = true, core = true, list = false, verbose = false;
    int repeat = 1;
    double scale = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--unit") core = false;
        else if (a == "--core") unit = false;
        else if (a == "--list") list = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--audible") SetEnvironmentVariableW(L"VIDEOPLAYER_TESTS_AUDIBLE", L"1");  // children inherit it
        else if (a == "--repeat" && i + 1 < argc) repeat = std::max(1, atoi(argv[++i]));
        else if (a == "--timeout-scale" && i + 1 < argc) scale = atof(argv[++i]);
        else if (a == "--help" || a == "-h") return Usage(), 0;
        else if (a[0] == '-' && a.size() > 1 && a[1] != '-') exclude.push_back(a.substr(1));
        else if (a.rfind("--", 0) == 0) return Usage(), 2;
        else include.push_back(a);
    }
    std::map<std::string, int> seen;
    for (const Case& c : Cases()) {
        if (++seen[c.name] > 1) {
            fprintf(stderr, "two tests are named %s\n", c.name);
            return 2;
        }
    }
    std::vector<const Case*> units, cores;
    for (const Case& c : Cases()) {
        const std::string name = c.name;
        const auto has = [&](const std::string& s) { return name.find(s) != std::string::npos; };
        if ((!include.empty() && std::none_of(include.begin(), include.end(), has)) ||
            std::any_of(exclude.begin(), exclude.end(), has)) {
            continue;
        }
        if (c.tier == kUnit && unit) units.push_back(&c);
        if (c.tier == kCore && core) cores.push_back(&c);
    }
    if (list) {
        for (const Case* c : units) printf("unit  %s\n", c->name);
        for (const Case* c : cores) printf("core  %s\n", c->name);
        return 0;
    }

    const auto real = Snapshot(kRealKey);
    const double start = Now();
    int passed = 0, failed = 0, skipped = 0, known = 0;
    std::vector<std::string> failures, bugs;
    for (int round = 0; round < repeat; ++round) {
        std::map<std::string, Result> results;
        // The unit tests in one process, then each core test in its own.
        std::vector<std::vector<const Case*>> batches;
        if (!units.empty()) batches.push_back(units);
        for (const Case* c : cores) batches.push_back({c});
        for (const auto& batch : batches) {
            Child child(batch, scale);
            for (size_t at = 0; at < batch.size();) {
                const size_t next = child.Run(at, results);
                // A test that crashed or hung without starting another is passed over.
                at = next > at ? next : at + 1;
            }
            for (const Case* c : batch) {
                auto it = results.find(c->name);
                Result r = it != results.end() ? it->second : Result{"CRASH", 0, "", "  never reported\n"};
                const bool bad = r.verdict != "PASS" && r.verdict != "SKIP" && r.verdict != "XFAIL";
                printf("[%-5s] %-58s %7.0f ms%s%s\n", r.verdict.c_str(), c->name, r.ms, r.why.empty() ? "" : "  ",
                       r.why.c_str());
                if (bad || verbose) printf("%s", r.output.c_str());
                fflush(stdout);
                if (r.verdict == "PASS") ++passed;
                else if (r.verdict == "SKIP") ++skipped;
                else if (r.verdict == "XFAIL") {
                    ++known;
                    bugs.push_back(std::string(c->name) + ": " + r.why);
                } else {
                    ++failed;
                    failures.push_back(r.verdict + " " + c->name);
                }
            }
        }
    }

    // No test may change the user's own settings; put them back if one did.
    RegDeleteTreeW(HKEY_CURRENT_USER, kSandboxRoot);
    if (Snapshot(kRealKey) != real) {
        Restore(kRealKey, real);
        ++failed;
        failures.push_back("FAIL the run changed HKCU\\Software\\VideoPLAYER (now restored)");
    }
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    std::error_code ignored;
    std::filesystem::remove_all(std::wstring(base) + L"VideoPLAYER-tests", ignored);

    printf("\n%d passed, %d failed, %d skipped, %d known bugs in %.1f s\n", passed, failed, skipped, known,
           Now() - start);
    for (const std::string& b : bugs) printf("  known bug  %s\n", b.c_str());
    for (const std::string& f : failures) printf("  %s\n", f.c_str());
    return failed ? 1 : 0;
}

}  // namespace

}  // namespace t

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "--child") == 0) {
        return t::ChildMain(std::vector<std::string>(argv + 2, argv + argc));
    }
    return t::RunnerMain(argc, argv);
}
