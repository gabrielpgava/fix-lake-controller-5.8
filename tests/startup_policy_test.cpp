#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <string>
#include <vector>
#include "../src/component_contract.h"

// One test-owned source builds the controller, a pre-entry TLS child, and
// inert component DLLs. No proprietary image or existing fixture is modified.
#if defined(LAKE_STARTUP_CHILD)
namespace {
void NTAPI BeforeFixtureEntry(void*, DWORD reason, void*) {
    if (reason != DLL_PROCESS_ATTACH) return;
#ifdef LAKE_STARTUP_ALWAYS_EARLY_EXIT
    ExitProcess(0xc0000005);
#endif
    const wchar_t* value = wcsstr(GetCommandLineW(), L"--early-exit=");
    if (value) {
        value += wcslen(L"--early-exit=");
        DWORD code = 0;
        while (*value >= L'0' && *value <= L'9') code = code * 10 + static_cast<DWORD>(*value++ - L'0');
        ExitProcess(code);
    }
    if (wcsstr(GetCommandLineW(), L"--early-sleep")) Sleep(5000);
}
}
extern "C" {
#if defined(_MSC_VER)
#pragma section(".CRT$XLB", long, read)
__declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK startupTls = BeforeFixtureEntry;
#pragma comment(linker, "/INCLUDE:_startupTls")
#pragma comment(linker, "/INCLUDE:__tls_used")
#else
PIMAGE_TLS_CALLBACK startupTls __attribute__((section(".CRT$XLB"), used)) = BeforeFixtureEntry;
#endif
}
int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--hold-event") == 0) {
            HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, argv[++i]);
            if (!event) return 91;
            const DWORD wait = WaitForSingleObject(event, 10000);
            CloseHandle(event);
            return wait == WAIT_OBJECT_0 ? 0 : 92;
        }
        if (wcscmp(argv[i], L"--exit-code") == 0) return static_cast<int>(wcstoul(argv[++i], nullptr, 10));
    }
    return 0;
}
#elif defined(LAKE_STARTUP_COMPONENT)
namespace { bool queried = false; }
#ifndef LAKE_STARTUP_MISSING_QUERY
extern "C" __declspec(dllexport) DWORD WINAPI LakeWindowedQueryContract(void* output) {
    auto* contract = static_cast<lakefix::ComponentContract*>(output);
    if (!contract || contract->size != sizeof(*contract)) return ERROR_REVISION_MISMATCH;
    *contract = lakefix::ExpectedComponentContract();
#ifdef LAKE_STARTUP_BAD_CONTRACT
    // The main test changes one field at a time too; this variant proves the
    // actual remote handshake refuses a build mismatch BEFORE init executes.
    contract->buildId[0] ^= 1;
#endif
    queried = true;
    return ERROR_SUCCESS;
}
#endif
extern "C" __declspec(dllexport) DWORD WINAPI LakeWindowedInitialize(void* input) {
    auto* request = static_cast<lakefix::InitializeRequest*>(input);
    if (!queried || !request || request->size != sizeof(*request) || request->abi != lakefix::kComponentAbi ||
        !lakefix::ComponentContractMatches(request->component)) return ERROR_REVISION_MISMATCH;
    if (!request->contourPath[0] || request->contourPath[32767]) return ERROR_INVALID_NAME;
    HANDLE file = CreateFileW(reinterpret_cast<const wchar_t*>(request->contourPath), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    char text[3]{};
    DWORD bytes{};
    const bool read = GetFileInformationByHandle(file, &info) && ReadFile(file, text, sizeof(text), &bytes, nullptr);
    CloseHandle(file);
    const lakefix::ComponentFileIdentity actual{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
        info.nFileSizeHigh, info.nFileSizeLow, info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime};
    constexpr BYTE abcHash[] = {0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    if (!read || bytes != 3 || memcmp(text, "abc", 3) || memcmp(&actual, &request->contourIdentity, sizeof(actual)) ||
        memcmp(abcHash, request->contourSha256, sizeof(abcHash))) return ERROR_FILE_INVALID;
    wchar_t name[256]{};
    if (!GetEnvironmentVariableW(L"LAKE_STARTUP_INIT_EVENT", name, ARRAYSIZE(name))) return ERROR_INVALID_NAME;
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!event) return GetLastError();
    SetEvent(event);
    CloseHandle(event);
    return ERROR_SUCCESS;
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, void*) { return TRUE; }
#else
// Test the very same production helpers and policy paths. Build all test
// artifacts with LAKE_FIX_TESTING so their component identity carries the
// mandatory fixture suffix. No test-specific hash policy exists in the helpers.
#define wmain LauncherMain
#include "../src/launcher.cpp"
#undef wmain

namespace {
int checks = 0, failures = 0;
void Check(bool value, const char* text) {
    ++checks;
    printf("%s: %s\n", value ? "PASS" : "FAIL", text);
    fflush(stdout);
    if (!value) ++failures;
}

std::wstring Quote(const std::wstring& value) { return L"\"" + value + L"\""; }
std::wstring Self() {
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return path;
}

struct Child {
    PROCESS_INFORMATION process{};
    ~Child() {
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
                TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED); // Only OUR CreateProcess handle.
                DebugActiveProcessStop(process.dwProcessId);
                WaitForSingleObject(process.hProcess, 5000);
            }
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
    }
    bool Start(const std::wstring& path, const std::wstring& arguments, DWORD flags = 0,
               const std::wstring& directory = L"") {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        std::wstring command = Quote(path) + L" " + arguments;
        return CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr,
                              directory.empty() ? nullptr : directory.c_str(), &startup, &process) != FALSE;
    }
    bool Exit(DWORD expected, DWORD timeout = 7000) {
        DWORD code{};
        return WaitForSingleObject(process.hProcess, timeout) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(process.hProcess, &code) && code == expected;
    }
};

struct Sandbox {
    std::wstring path;
    std::vector<std::wstring> files;
    std::vector<std::wstring> directories;
    explicit Sandbox(const std::wstring& parent) {
        const std::wstring prefix = parent + L"\\startup-sandbox-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        for (int i = 0; i < 100; ++i) {
            const auto candidate = prefix + L"-" + std::to_wstring(i);
            if (CreateDirectoryW(candidate.c_str(), nullptr)) { path = candidate; break; }
            if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        }
    }
    ~Sandbox() {
        for (auto it = files.rbegin(); it != files.rend(); ++it) DeleteFileW(it->c_str());
        // All directories below were CREATE_NEW-equivalent test-owned. Never
        // recurse outside them or delete any shared TEMP fallback log.
        for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
            WIN32_FIND_DATAW data{};
            HANDLE search = FindFirstFileW((*it + L"\\*").c_str(), &data);
            if (search != INVALID_HANDLE_VALUE) {
                do {
                    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((*it + L"\\" + data.cFileName).c_str());
                } while (FindNextFileW(search, &data));
                FindClose(search);
            }
            RemoveDirectoryW(it->c_str());
        }
        if (!path.empty()) RemoveDirectoryW(path.c_str());
    }
    bool Directory(const std::wstring& name) {
        const auto full = path + L"\\" + name;
        if (!CreateDirectoryW(full.c_str(), nullptr)) return false;
        directories.push_back(full);
        return true;
    }
    bool Copy(const std::wstring& from, const std::wstring& name) {
        const auto full = path + L"\\" + name;
        if (!CopyFileW(from.c_str(), full.c_str(), TRUE)) return false;
        files.push_back(full);
        return true;
    }
    bool Write(const std::wstring& name, const char* text) {
        const auto full = path + L"\\" + name;
        Handle file(CreateFileW(full.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.valid()) return false;
        files.push_back(full);
        DWORD written{};
        return WriteFile(file.get(), text, static_cast<DWORD>(strlen(text)), &written, nullptr) && written == strlen(text);
    }
};

std::string ReadText(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    char text[8192]{};
    DWORD bytes{};
    if (!file.valid() || !ReadFile(file.get(), text, sizeof(text) - 1, &bytes, nullptr)) return {};
    return std::string(text, bytes);
}

DWORD snapshotCalls, snapshotFailures, snapshotError;
HANDLE WINAPI SyntheticSnapshot(DWORD, DWORD) {
    ++snapshotCalls;
    if (snapshotCalls <= snapshotFailures) { SetLastError(snapshotError); return INVALID_HANDLE_VALUE; }
    return CreateEventW(nullptr, FALSE, FALSE, nullptr);
}
BOOL WINAPI FailedDebugWait(LPDEBUG_EVENT, DWORD) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
DEBUG_EVENT queuedExit{};
DWORD replayWaitMilliseconds = INFINITE;
BOOL WINAPI ReplayQueuedExit(LPDEBUG_EVENT event, DWORD milliseconds) {
    replayWaitMilliseconds = milliseconds;
    *event = queuedExit;
    return TRUE;
}

void TestPurePolicies() {
    Check(lakefix::startup::DebugWaitFailure(ERROR_ACCESS_DENIED).state == State::waitError &&
          lakefix::startup::DebugWaitFailure(ERROR_ACCESS_DENIED).error == ERROR_ACCESS_DENIED, "real wait error is not a timeout");
    Check(lakefix::startup::DebugWaitFailure(ERROR_SEM_TIMEOUT).state == State::timeout &&
          lakefix::startup::DebugWaitFailure(ERROR_TIMEOUT).error == ERROR_TIMEOUT, "both documented Win32 timeout forms are classified");
    for (DWORD code : {0ul, 23ul, 259ul, 0xc0000005ul}) {
        const auto result = lakefix::startup::Exited(code);
        Check(result.state == State::childExited && result.hasExitCode && result.exitCode == code && !result.error,
              "early exit retains exact code, including zero and STILL_ACTIVE");
    }
    Check(lakefix::startup::MutexAcquired(WAIT_OBJECT_0) && lakefix::startup::MutexAcquired(WAIT_ABANDONED) &&
          !lakefix::startup::MutexAcquired(WAIT_TIMEOUT), "mutex abandonment owns gate, timeout does not");
    auto contract = lakefix::ExpectedComponentContract();
    Check(lakefix::ComponentIdentityConfigured() && lakefix::ComponentContractMatches(contract), "central product/build identity matches itself");
    for (size_t offset = 0; offset < sizeof(contract); ++offset) {
        auto changed = contract;
        reinterpret_cast<BYTE*>(&changed)[offset] ^= 1;
        if (lakefix::ComponentContractMatches(changed)) {
            Check(false, "all ABI, architecture, version, build, reserved bytes must match");
            return;
        }
    }
    Check(true, "all ABI, architecture, version, build, reserved bytes must match");
    Check(!lakefix::ComponentContractMatches(contract, lakefix::MakeComponentContract("", "")), "unconfigured component identity cannot match");
    const auto production = lakefix::MakeComponentContract(LAKE_FIX_VERSION, LAKE_FIX_BUILD_ID);
    const auto fixture = lakefix::MakeComponentContract(LAKE_FIX_VERSION, LAKE_FIX_BUILD_ID "-fixture");
    Check(!lakefix::ComponentContractMatches(fixture, production), "production build contract rejects same-stamp fixture DLL identity");
    {
        Handle temporary(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        SetLastError(ERROR_ACCESS_DENIED);
    }
    Check(GetLastError() == ERROR_ACCESS_DENIED, "RAII CloseHandle preserves primary Win32 failure");
}

void TestSnapshots() {
    snapshotCalls = 0; snapshotFailures = 2; snapshotError = ERROR_BAD_LENGTH;
    Handle success(CreateSnapshot(TH32CS_SNAPMODULE, 123, SyntheticSnapshot, 0));
    Check(success.valid() && snapshotCalls == 3, "snapshot retries ERROR_BAD_LENGTH then succeeds");
    snapshotCalls = 0; snapshotFailures = 100;
    Handle exhausted(CreateSnapshot(TH32CS_SNAPMODULE, 123, SyntheticSnapshot, 0));
    const DWORD exhaustedError = GetLastError();
    Check(!exhausted.valid() && snapshotCalls == lakefix::startup::kSnapshotAttempts && exhaustedError == ERROR_BAD_LENGTH,
          "snapshot retries are bounded and retain ERROR_BAD_LENGTH");
    snapshotCalls = 0; snapshotError = ERROR_ACCESS_DENIED;
    Handle denied(CreateSnapshot(TH32CS_SNAPPROCESS, 0, SyntheticSnapshot, 0));
    Check(!denied.valid() && snapshotCalls == 1 && GetLastError() == ERROR_ACCESS_DENIED, "non-transient snapshot error is never retried");
    DWORD self{};
    const auto name = Self().substr(Self().find_last_of(L'\\') + 1);
    Check(FindRunningImage(name.c_str(), self) && self != 0, "production process discovery is available to tests");
}

void TestRetention(Sandbox& sandbox) {
    Check(sandbox.Directory(L"pinned") && sandbox.Write(L"pinned\\hash.bin", "abc"), "test owns hash file and directory");
    VerifiedFile file;
    const auto path = sandbox.path + L"\\pinned\\hash.bin";
    const bool opened = file.Open(path);
    Check(opened && file.Matches(L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "production retained-handle SHA-256 matches known data");
    Check(opened && !file.Matches(kContourHash) && GetLastError() == ERROR_INVALID_DATA, "production allowlist is not bypassed for fixture content");
    Handle writer(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(!writer.valid() && GetLastError() == ERROR_SHARING_VIOLATION, "retained verified file rejects new writers");
    Check(!DeleteFileW(path.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION, "retained verified file rejects deletion");
    Check(!MoveFileW(path.c_str(), (path + L".moved").c_str()), "retained verified file rejects replacement by rename");
    Check(!MoveFileW((sandbox.path + L"\\pinned").c_str(), (sandbox.path + L"\\pinned-moved").c_str()), "canonical parent pin prevents directory substitution");
    Check(file.IsSameFile(path), "retained identity still agrees with pathname after substitution attempts");
    file.Release();
    Handle after(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(after.valid(), "writer is permitted after explicit startup retention release");
}

void TestBreakpoint() {
    BYTE* code = static_cast<BYTE*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(code != nullptr, "allocate own synthetic executable page");
    if (!code) return;
    code[0] = 0x90;
    DWORD ignored{};
    VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &ignored);
    EntryBreakpoint patch;
    MEMORY_BASIC_INFORMATION armed{}, restored{};
    const bool set = patch.Arm(GetCurrentProcess(), code);
    VirtualQuery(code, &armed, sizeof(armed));
    Check(set && code[0] == 0xcc && armed.Protect == PAGE_EXECUTE_READ, "INT3 arm restores original page protection immediately");
    const bool reset = patch.Restore();
    VirtualQuery(code, &restored, sizeof(restored));
    Check(reset && code[0] == 0x90 && restored.Protect == PAGE_EXECUTE_READ, "INT3 restore preserves original byte and original protection");
    VirtualFree(code, 0, MEM_RELEASE);
}

void TestDebugOutcomes(const std::wstring& childPath, Sandbox& sandbox) {
    for (DWORD code : {0ul, 23ul, 259ul, 0xc0000005ul}) {
        Child child;
        const bool created = child.Start(childPath, L"--early-exit=" + std::to_wstring(code), DEBUG_ONLY_THIS_PROCESS);
        Check(created, "create owned TLS early-exit fixture");
        if (!created) continue;
        auto result = StopBeforeEntry(child.process, 3000);
        Check(result.state == State::childExited && result.hasExitCode && result.exitCode == code && !result.error,
              "real EXIT_PROCESS_DEBUG_EVENT is not converted into timeout");
        const bool terminated = CleanupOwnedChild(child.process, result);
        Check(!terminated && child.Exit(code), "already exited child is never terminated and exact code remains observable");
        const auto report = WriteStartupReport(sandbox.path, child.process.dwProcessId, "before-entry", result, terminated);
        const auto text = ReadText(report);
        const auto decimal = "code_decimal=" + std::to_string(code);
        Check(!report.empty() && text.find("status=child-exited") != std::string::npos && text.find(decimal) != std::string::npos,
              "startup report records early-exit code without --wait");
    }
    {
        Child child;
        bool captured = child.Start(childPath, L"--early-exit=23", DEBUG_ONLY_THIS_PROCESS);
        bool foundExit = false;
        const ULONGLONG deadline = GetTickCount64() + 3000;
        while (captured && GetTickCount64() < deadline) {
            DEBUG_EVENT event{};
            if (!WaitForDebugEvent(&event, 1000)) break;
            if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) { queuedExit = event; foundExit = true; break; }
            if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT && event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
            if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE)) break;
        }
        Check(foundExit, "capture real pending EXIT_PROCESS event at synthetic expired deadline");
        if (foundExit) {
            const auto result = StopBeforeEntry(child.process, 0, ReplayQueuedExit);
            Check(result.state == State::childExited && result.exitCode == 23 && replayWaitMilliseconds == 0 && child.Exit(23),
                  "already queued real exit outranks expired startup clock");
        }
    }
    {
        Child child;
        Check(child.Start(childPath, L"--early-sleep", DEBUG_ONLY_THIS_PROCESS), "create owned TLS timeout fixture");
        if (child.process.hProcess) {
            const ULONGLONG start = GetTickCount64();
            auto result = StopBeforeEntry(child.process, 200);
            Check(result.state == State::timeout && result.error == ERROR_TIMEOUT && !result.hasExitCode && GetTickCount64() - start < 2500,
                  "actual pre-entry wait timeout remains bounded and has no invented exit code");
            Check(CleanupOwnedChild(child.process, result) && child.Exit(ERROR_PROCESS_ABORTED), "only owned timed-out child is cleaned up");
        }
    }
    {
        Child child;
        Check(child.Start(childPath, L"", DEBUG_ONLY_THIS_PROCESS), "create owned debug API failure fixture");
        if (child.process.hProcess) {
            auto result = StopBeforeEntry(child.process, 1000, FailedDebugWait);
            Check(result.state == State::waitError && result.error == ERROR_ACCESS_DENIED && !result.hasExitCode,
                  "Win32 wait failure survives suspend/restore/detach cleanup");
            CleanupOwnedChild(child.process, result);
            Check(child.Exit(ERROR_PROCESS_ABORTED), "owned debug-error child cleanup completes");
        }
    }
    {
        Child child;
        Check(child.Start(childPath, L"--exit-code 23", DEBUG_ONLY_THIS_PROCESS), "create owned normal-entry fixture");
        if (child.process.hProcess) {
            const auto result = StopBeforeEntry(child.process, 3000);
            CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
            MEMORY_BASIC_INFORMATION region{};
            BYTE byte = 0xcc; SIZE_T bytes{};
            const bool stopped = result.state == State::ready && GetThreadContext(child.process.hThread, &context) &&
                ReadProcessMemory(child.process.hProcess, reinterpret_cast<void*>(context.Eip), &byte, 1, &bytes) && bytes == 1 &&
                VirtualQueryEx(child.process.hProcess, reinterpret_cast<void*>(context.Eip), &region, sizeof(region));
            Check(stopped && byte != 0xcc && !(region.Protect & PAGE_EXECUTE_READWRITE), "real entry stop rewinds EIP to original byte with RX protection");
            CONTEXT after{}; after.ContextFlags = CONTEXT_CONTROL;
            const bool again = stopped && RewindEntry(child.process.hThread, reinterpret_cast<void*>(context.Eip)) &&
                GetThreadContext(child.process.hThread, &after);
            Check(again && context.Eip == after.Eip && context.EFlags == after.EFlags && context.Esp == after.Esp &&
                  context.Ebp == after.Ebp && context.SegCs == after.SegCs && context.SegSs == after.SegSs,
                  "rewind preserves complete original x86 control context except intended EIP change");
            Check(result.state == State::ready && ResumeThread(child.process.hThread) == 1 && child.Exit(23),
                  "restored child executes original entry after exactly one resume");
        }
    }
}

DWORD WINAPI AbandonGate(void* input) {
    return WaitForSingleObject(static_cast<HANDLE>(input), 2000) == WAIT_OBJECT_0 ? 0 : 1;
}

int GateWorker(int argc, wchar_t** argv) {
    if (argc != 8) return 90;
    StartupMutex gate;
    if (!gate.Acquire(argv[2], 5000)) return 91;
    DWORD pid{};
    if (!FindRunningImage(argv[4], pid)) return 92;
    if (lakefix::startup::ExistingImageBlocks(pid)) return 4;
    Child child;
    if (!child.Start(argv[3], L"--hold-event " + Quote(argv[7]))) return 93;
    Handle started(OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[5]));
    Handle release(OpenEventW(SYNCHRONIZE, FALSE, argv[6]));
    if (!started.valid() || !release.valid()) return 94;
    SetEvent(started.get());
    if (WaitForSingleObject(release.get(), 5000) != WAIT_OBJECT_0) return 95;
    gate.Release();
    return child.Exit(0, 7000) ? 0 : 96;
}

void TestGate(const std::wstring& childPath, Sandbox& sandbox) {
    const auto name = StartupMutexName();
    Check(!name.empty() && name == StartupMutexName() && name.find(L"Local\\") == 0, "startup gate is stable and user/session scoped");
    const auto gateName = name + L".fixture." + std::to_wstring(GetCurrentProcessId());
    Handle raw(CreateMutexExW(nullptr, gateName.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE));
    Handle thread(CreateThread(nullptr, 0, AbandonGate, raw.get(), 0, nullptr));
    Check(raw.valid() && thread.valid() && WaitForSingleObject(thread.get(), 3000) == WAIT_OBJECT_0, "synthetic worker abandons test-owned mutex");
    StartupMutex reclaimed;
    Check(reclaimed.Acquire(gateName, 1000), "abandoned named gate is reclaimed");
    reclaimed.Release();

    const auto childName = L"StartupGateChild-" + std::to_wstring(GetCurrentProcessId()) + L".exe";
    Check(sandbox.Copy(childPath, childName), "test owns distinct child name for external-process discovery");
    const auto startedName = gateName + L".started", releaseName = gateName + L".release", exitName = gateName + L".exit";
    Handle started(CreateEventW(nullptr, TRUE, FALSE, startedName.c_str()));
    Handle release(CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str()));
    Handle exit(CreateEventW(nullptr, TRUE, FALSE, exitName.c_str()));
    const auto arguments = L"--gate-worker " + Quote(gateName) + L" " + Quote(sandbox.path + L"\\" + childName) + L" " +
        Quote(childName) + L" " + Quote(startedName) + L" " + Quote(releaseName) + L" " + Quote(exitName);
    Child first, second;
    const bool firstCreated = first.Start(Self(), arguments);
    const bool initializing = firstCreated && WaitForSingleObject(started.get(), 3000) == WAIT_OBJECT_0;
    Check(initializing, "first launcher holds gate through synthetic initialization");
    const bool secondCreated = initializing && second.Start(Self(), arguments);
    Check(secondCreated && WaitForSingleObject(second.process.hProcess, 150) == WAIT_TIMEOUT, "concurrent launcher cannot enumerate/create during first initialization");
    SetEvent(release.get());
    Check(secondCreated && second.Exit(4), "second launcher re-enumerates after gate and refuses existing child");
    DWORD pid{};
    Check(FindRunningImage(childName.c_str(), pid) && pid != 0, "process outside current launcher is detected without opening memory");
    SetEvent(exit.get());
    Check(firstCreated && first.Exit(0), "fixture exits normally; gate is not held for process lifetime");
}

void TestHandshake(const std::wstring& directory, const std::wstring& childPath, Sandbox& sandbox) {
    VerifiedFile contour;
    Check(sandbox.Write(L"contour-input.bin", "abc") && contour.Open(sandbox.path + L"\\contour-input.bin") && contour.Hash(), "retain synthetic Contour identity for init request");
    const auto eventName = StartupMutexName() + L".init-test." + std::to_wstring(GetCurrentProcessId());
    Handle initialized(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()));
    SetEnvironmentVariableW(L"LAKE_STARTUP_INIT_EVENT", eventName.c_str());
    for (const wchar_t* variant : {L"StartupComponentGood.dll", L"StartupComponentMismatch.dll", L"StartupComponentMissing.dll"}) {
        ResetEvent(initialized.get());
        VerifiedFile dll;
        ComponentExports exports{};
        const bool opened = dll.Open(directory + L"\\" + variant);
        const bool inspected = opened && InspectComponent(dll, exports);
        const bool missing = wcscmp(variant, L"StartupComponentMissing.dll") == 0;
        const bool good = wcscmp(variant, L"StartupComponentGood.dll") == 0;
        Check(missing ? !inspected && GetLastError() == ERROR_PROC_NOT_FOUND : inspected, "preflight requires both query and init exports");
        if (!inspected) continue;
        Child child;
        Check(child.Start(childPath, L"--exit-code 23", DEBUG_ONLY_THIS_PROCESS), "create owned remote-handshake fixture");
        if (!child.process.hProcess) continue;
        auto result = StopBeforeEntry(child.process, 3000);
        const bool injected = result.state == State::ready && Inject(child.process, dll, contour, exports);
        const DWORD error = GetLastError();
        if (good) {
            Check(injected && WaitForSingleObject(initialized.get(), 0) == WAIT_OBJECT_0, "remote query succeeds before init; init receives exact retained path/file ID/hash");
            Check(injected && ResumeThread(child.process.hThread) == 1 && child.Exit(23), "successful handshake releases original child entry");
        } else {
            Check(!injected && error == ERROR_REVISION_MISMATCH && WaitForSingleObject(initialized.get(), 0) == WAIT_TIMEOUT,
                  "actual remote build mismatch is rejected without executing initialize");
            result = lakefix::startup::Failure(error);
            CleanupOwnedChild(child.process, result);
            Check(child.Exit(ERROR_PROCESS_ABORTED), "mismatched component failure terminates only owned child");
        }
    }
    SetEnvironmentVariableW(L"LAKE_STARTUP_INIT_EVENT", nullptr);
}

void TestNoWaitReport(const std::wstring& directory, const std::wstring& childPath, Sandbox& sandbox) {
    Check(sandbox.Directory(L"runner") && sandbox.Directory(L"runner\\logs"), "create isolated no-wait launcher directory");
    Check(sandbox.Copy(directory + L"\\LakeFixInjectionTest.exe", L"runner\\LakeFixInjectionTest.exe") &&
          sandbox.Copy(directory + L"\\StartupComponentMismatch.dll", L"runner\\LakeWindowedFix.dll") &&
          sandbox.Copy(childPath, L"runner\\LakeFixFixture.exe") && sandbox.Write(L"runner\\Contour.dll", "abc"), "isolate mismatched test DLL beside compile-time fixture launcher");
    Child launcher;
    const auto runner = sandbox.path + L"\\runner";
    Check(launcher.Start(runner + L"\\LakeFixInjectionTest.exe", L"--app-dir " + Quote(runner)) && launcher.Exit(1),
          "real test launcher reports handshake failure without --wait");
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW((runner + L"\\logs\\*-startup.log").c_str(), &data);
    std::string text;
    if (search != INVALID_HANDLE_VALUE) { text = ReadText(runner + L"\\logs\\" + data.cFileName); FindClose(search); }
    Check(text.find("stage=component-load-handshake-init") != std::string::npos && text.find("win32_decimal=1306") != std::string::npos &&
          text.find("exit_code_available=0") != std::string::npos && text.find("launcher_terminated_child=1") != std::string::npos,
          "no-wait startup report preserves mismatch and labels owned-child cleanup");

    Check(sandbox.Directory(L"early") && sandbox.Directory(L"early\\logs") &&
          sandbox.Copy(directory + L"\\LakeFixInjectionTest.exe", L"early\\LakeFixInjectionTest.exe") &&
          sandbox.Copy(directory + L"\\StartupComponentGood.dll", L"early\\LakeWindowedFix.dll") &&
          sandbox.Copy(directory + L"\\StartupEarlyChild.exe", L"early\\LakeFixFixture.exe") &&
          sandbox.Write(L"early\\Contour.dll", "abc"), "isolate always-early-exit fixture beside test launcher");
    Child earlyLauncher;
    const auto early = sandbox.path + L"\\early";
    Check(earlyLauncher.Start(early + L"\\LakeFixInjectionTest.exe", L"--app-dir " + Quote(early)) && earlyLauncher.Exit(1),
          "real test launcher reports pre-entry exit without --wait");
    search = FindFirstFileW((early + L"\\logs\\*-startup.log").c_str(), &data);
    text.clear();
    if (search != INVALID_HANDLE_VALUE) { text = ReadText(early + L"\\logs\\" + data.cFileName); FindClose(search); }
    Check(text.find("stage=before-entry status=child-exited") != std::string::npos &&
          text.find("code_decimal=3221225477 code_hex=0xc0000005") != std::string::npos &&
          text.find("launcher_terminated_child=0") != std::string::npos,
          "no-wait early-exit report preserves original code and does not claim termination");
}

void TestStartupReportFallback(Sandbox& sandbox) {
    const DWORD pid = GetCurrentProcessId();
    const auto name = L"\\LakeWindowedFix-" + std::to_wstring(pid) + L"-startup.log";
    const auto local = sandbox.path + L"\\logs" + name;
    Handle blocked(CreateFileW(local.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                               nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    constexpr char marker[] = "preserve test-owned startup report\r\n";
    DWORD written{};
    const bool marked = blocked.valid() && WriteFile(blocked.get(), marker, sizeof(marker) - 1, &written, nullptr) && written == sizeof(marker) - 1;
    wchar_t temp[32768]{};
    const DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
    const auto expected = std::wstring(temp) + L"LakeWindowedFix" + name;
    LARGE_INTEGER before{};
    {
        Handle old(CreateFileW(expected.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (old.valid()) GetFileSizeEx(old.get(), &before);
    }
    const auto report = WriteStartupReport(sandbox.path, pid, "synthetic-wait", lakefix::startup::DebugWaitFailure(ERROR_ACCESS_DENIED));
    Handle fallback(CreateFileW(report.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    char appended[1024]{}; DWORD bytes{};
    const bool read = fallback.valid() && SetFilePointerEx(fallback.get(), before, nullptr, FILE_BEGIN) &&
        ReadFile(fallback.get(), appended, sizeof(appended) - 1, &bytes, nullptr);
    LARGE_INTEGER size{}, zero{};
    char original[sizeof(marker)]{}; DWORD originalBytes{};
    const bool preserved = marked && GetFileSizeEx(blocked.get(), &size) && size.QuadPart == sizeof(marker) - 1 &&
        SetFilePointerEx(blocked.get(), zero, nullptr, FILE_BEGIN) && ReadFile(blocked.get(), original, sizeof(marker) - 1, &originalBytes, nullptr) &&
        originalBytes == sizeof(marker) - 1 && memcmp(original, marker, sizeof(marker) - 1) == 0;
    Check(length && length < ARRAYSIZE(temp) && report == expected && read && strstr(appended, "status=wait-error win32_decimal=5") && preserved,
          "startup report falls back after local sharing failure, appending unchanged error and preserving old local bytes");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && wcscmp(argv[1], L"--gate-worker") == 0) return GateWorker(argc, argv);
    const auto directory = Parent(Self());
    Sandbox sandbox(directory);
    Check(!sandbox.path.empty() && sandbox.Directory(L"logs"), "create unique owned test sandbox");
    if (sandbox.path.empty()) return 1;
    const auto childPath = directory + L"\\StartupChild.exe";
    TestPurePolicies();
    TestSnapshots();
    TestRetention(sandbox);
    TestBreakpoint();
    TestDebugOutcomes(childPath, sandbox);
    TestGate(childPath, sandbox);
    TestHandshake(directory, childPath, sandbox);
    TestNoWaitReport(directory, childPath, sandbox);
    TestStartupReportFallback(sandbox);
    printf("Startup reliability: %d checks, %d failures (synthetic fixtures only).\n", checks, failures);
    return failures ? 1 : 0;
}
#endif
