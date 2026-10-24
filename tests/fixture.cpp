#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <new>
#include "../src/policy.h"
#include "../src/log_lock.h"

// Entirely synthetic application. No Lake code, audio, network or DirectDraw.
// Run via LakeFixInjectionTest, which uses the same loader/injection source as
// the production launcher, but can only start this fixed fixture filename.
namespace {
HWND lake;
int resizes;
int focusLosses;
int activations;
int closeRequests;
int startupNotifications;
bool vetoClose;
int failures;
int handledTestExceptions;
int destroyQuitCode = -1;
int lastPumpedQuit = -1;

LONG CALLBACK HandleSyntheticException(EXCEPTION_POINTERS* info) {
    if (info && info->ExceptionRecord && info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters == 1 &&
        info->ExceptionRecord->ExceptionInformation[0] == 0x4c465854) {
        ++handledTestExceptions;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool LogContains(const char* expected) {
    wchar_t path[1200]{};
    if (!GetModuleFileNameW(nullptr, path, ARRAYSIZE(path))) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    swprintf_s(slash + 1, ARRAYSIZE(path) - (slash + 1 - path), L"logs\\LakeWindowedFix-%lu.log", GetCurrentProcessId());
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char data[32768]{};
    DWORD bytes{};
    const bool read = ReadFile(file, data, sizeof(data) - 1, &bytes, nullptr) != FALSE;
    CloseHandle(file);
    return read && strstr(data, expected);
}

void Check(bool condition, const char* name) {
    printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

struct DiagnosticLockProbe {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE ready = nullptr;
    HANDLE release = nullptr;
    LONG owners = 1;
};

void ReleaseDiagnosticLockProbe(DiagnosticLockProbe* probe) {
    if (InterlockedDecrement(&probe->owners) == 0) {
        if (probe->ready) CloseHandle(probe->ready);
        if (probe->release) CloseHandle(probe->release);
        delete probe;
    }
}

DWORD WINAPI HoldDiagnosticLock(void* context) {
    auto* probe = static_cast<DiagnosticLockProbe*>(context);
    AcquireSRWLockExclusive(&probe->lock);
    SetEvent(probe->ready);
    // Bound the hold itself: a regressed blocking best-effort acquisition in
    // the UI thread must return after this timeout, not hang the fixture.
    WaitForSingleObject(probe->release, 2000);
    ReleaseSRWLockExclusive(&probe->lock);
    ReleaseDiagnosticLockProbe(probe);
    return 0;
}

void TestBestEffortDiagnosticLock() {
    auto* probe = new (std::nothrow) DiagnosticLockProbe;
    Check(probe != nullptr, "diagnostic lock probe allocation");
    if (!probe) return;
    probe->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    probe->release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(probe->ready && probe->release, "diagnostic lock probe owns ready/release events");
    if (!probe->ready || !probe->release) {
        ReleaseDiagnosticLockProbe(probe);
        return;
    }
    InterlockedIncrement(&probe->owners);
    HANDLE worker = CreateThread(nullptr, 0, HoldDiagnosticLock, probe, 0, nullptr);
    Check(worker != nullptr, "diagnostic lock holder thread created");
    if (!worker) {
        ReleaseDiagnosticLockProbe(probe); // Unused worker ownership.
        ReleaseDiagnosticLockProbe(probe);
        return;
    }
    const bool held = WaitForSingleObject(probe->ready, 2000) == WAIT_OBJECT_0;
    Check(held, "diagnostic lock held by synthetic worker");
    if (held) {
        const ULONGLONG begin = GetTickCount64();
        const bool acquired = lakefix::AcquireDiagnosticLock(probe->lock, true);
        const ULONGLONG elapsed = GetTickCount64() - begin;
        if (acquired) ReleaseSRWLockExclusive(&probe->lock);
        Check(!acquired, "best-effort diagnostic lock rejects a contended lock");
        Check(elapsed < 1000, "best-effort diagnostic lock returns within 1000 ms");
    }
    SetEvent(probe->release);
    Check(WaitForSingleObject(worker, 2000) == WAIT_OBJECT_0, "diagnostic lock holder thread finishes");
    CloseHandle(worker);
    // A timed-out worker retains its own reference, so neither its lock nor
    // its event handles can be freed while it is still using them.
    ReleaseDiagnosticLockProbe(probe);
}

DWORD WINAPI PostSyntheticWorkerQuit(void*) {
    PostQuitMessage(99);
    return 0;
}

LRESULT CALLBACK ParentProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SIZE && lake && wparam != SIZE_MINIMIZED && LOWORD(lparam) && HIWORD(lparam)) {
        ++resizes;
        MoveWindow(lake, 0, 0, LOWORD(lparam), HIWORD(lparam), FALSE);
    }
    return DefWindowProcA(window, message, wparam, lparam);
}
LRESULT CALLBACK LakeProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == 0x8032) ++startupNotifications;
    if (message == WM_KILLFOCUS) ++focusLosses;
    if (message == WM_ACTIVATEAPP) ++activations;
    if (message == WM_CLOSE) {
        ++closeRequests;
        if (vetoClose) return 0;
    }
    if (message == WM_DESTROY && destroyQuitCode >= 0) PostQuitMessage(destroyQuitCode);
    return DefWindowProcA(window, message, wparam, lparam);
}
void Pump() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            lastPumpedQuit = static_cast<int>(message.wParam);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--exit-code") == 0) {
            // Intentional ordinary exit, not a crash or foreign-process kill.
            ExitProcess(static_cast<UINT>(wcstoul(argv[i + 1], nullptr, 10)));
        }
    }
    wchar_t configuration[1024]{};
    GetModuleFileNameW(nullptr, configuration, ARRAYSIZE(configuration));
    wchar_t* slash = wcsrchr(configuration, L'\\');
    if (slash) wcscpy_s(slash + 1, ARRAYSIZE(configuration) - (slash + 1 - configuration), L"LakeWindowedFix.ini");
    const bool nativeResize = GetPrivateProfileIntW(L"compatibility", L"NativeResize", 0, configuration) == 1;
    printf("Fixture NativeResize=%d\n", nativeResize ? 1 : 0);
    Check(sizeof(void*) == 4, "x86 fixture");
    Check(lakefix::HostStyle(WS_POPUP) & WS_CAPTION, "host caption policy");
    Check((lakefix::ClientStyle(WS_POPUP | WS_CAPTION) & (WS_POPUP | WS_CAPTION)) == 0, "child removes popup/caption");
    Check(!lakefix::HasPixels(SIZE_MINIMIZED, MAKELPARAM(800, 600)), "minimize policy");
    Check(!lakefix::HasPixels(SIZE_RESTORED, 0), "zero-size policy");
    TestBestEffortDiagnosticLock(); // No windows or resize counters involved.
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXA cls{};
    cls.cbSize = sizeof(cls);
    cls.hInstance = instance;
    cls.lpfnWndProc = ParentProc;
    cls.lpszClassName = "ContourParent";
    Check(RegisterClassExA(&cls) != 0, "register native parent fixture");
    cls.lpfnWndProc = LakeProc;
    cls.lpszClassName = "Lake Controller";
    Check(RegisterClassExA(&cls) != 0, "register native client fixture");
    cls.lpszClassName = "LakeFix.Unrelated";
    Check(RegisterClassExA(&cls) != 0, "register unrelated window");
    HWND unrelated = CreateWindowExA(0, cls.lpszClassName, "Unrelated", WS_POPUP,
                                    0, 0, 100, 100, nullptr, nullptr, instance, nullptr);
    Check(unrelated && (GetWindowLongW(unrelated, GWL_STYLE) & WS_POPUP), "unrelated window unchanged");
    // Exercise the ANSI path used by the shipped target.
    lake = CreateWindowExA(WS_EX_TOPMOST, "Lake Controller", "Lake Controller", WS_POPUP | WS_SYSMENU,
                           0, 0, 1024, 768, nullptr, nullptr, instance, nullptr);
    Check(lake != nullptr, "create hooked native client");
    if (!lake) return 1;
    HWND frame = GetParent(lake);
    RECT originalClient{};
    GetClientRect(lake, &originalClient);
    Check(frame != nullptr, "client has standard host");
    Check((GetWindowLongW(lake, GWL_STYLE) & WS_CHILD) != 0, "native renderer is a child");
    const DWORD expectedStyle = nativeResize ? WS_OVERLAPPEDWINDOW : WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    Check((static_cast<DWORD>(GetWindowLongW(frame, GWL_STYLE)) & expectedStyle) == expectedStyle, "host style matches selected resize policy");
    if (!nativeResize) Check(!(GetWindowLongW(frame, GWL_STYLE) & (WS_THICKFRAME | WS_MAXIMIZEBOX)),
                             "diagnostic mode has no resize/maximize controls");
    Check(!(GetWindowLongW(lake, GWL_EXSTYLE) & WS_EX_TOPMOST), "no topmost client");
    MINMAXINFO limits{};
    SendMessageW(frame, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
    Check(limits.ptMinTrackSize.x > 800 && limits.ptMinTrackSize.y > 600, "host minimum includes non-client borders");
    Pump(); // Simulate a nested PeekMessage loop DURING plugin initialization.
    Check(!IsWindowVisible(frame) && resizes == 0, "startup pumping cannot trigger premature rebuild");
    // Match the native plugin's vtable+0x24 method: post 0x8032, then
    // ShowWindow(main, SW_SHOWNORMAL), BEFORE the EXE's first GetMessage.
    PostMessageW(lake, 0x8032, 0, 0);
    ShowWindow(lake, SW_SHOWNORMAL);
    Check(IsWindowVisible(frame) && IsWindowVisible(lake) && resizes == 0,
          "native ShowWindow reveals host without premature graphics rebuild");
    Pump(); // A plugin-owned loop, not the EXE's GetMessage wrapper.
    Check(startupNotifications == 1 && GetPropW(frame, L"LakeWindowedFix.HostSetupCompleted") &&
          (nativeResize ? resizes >= 1 : resizes == 0),
          "native startup initializes host and obeys graphics rebuild policy");
    MSG start{};
    // The observed EXE's first outer GetMessage call is the readiness milestone.
    for (int i = 0; i < 32 && !IsWindowVisible(frame); ++i) {
        if (GetMessageA(&start, nullptr, 0, 0) <= 0) break;
        TranslateMessage(&start);
        DispatchMessageA(&start);
    }
    Pump();
    RECT actual{};
    GetClientRect(frame, &actual);
    Check(GetPropW(frame, L"LakeWindowedFix.HostSetupCompleted") && actual.right >= 800 && actual.bottom >= 600,
          "deferred host setup and minimum area");

    // A software-raised, explicitly handled test exception, NOT a crash or
    // invalid memory access. The observer must log and continue searching.
    PVOID handler = AddVectoredExceptionHandler(0, HandleSyntheticException);
    if (handler) {
        const ULONG_PTR marker = 0x4c465854;
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 1, &marker);
        RemoveVectoredExceptionHandler(handler);
    }
    Check(handler && handledTestExceptions == 1, "exception observer does not swallow application-handled exceptions");
    Check(LogContains("First-chance exception (may be handled by application)"), "first-chance diagnostic is logged without claiming a crash");
    PostQuitMessage(7);
    BOOL quitResult = 1;
    for (int i = 0; i < 32 && quitResult > 0; ++i) {
        quitResult = GetMessageA(&start, nullptr, 0, 0);
        if (quitResult > 0) { TranslateMessage(&start); DispatchMessageA(&start); }
    }
    Check(quitResult == 0 && start.wParam == 7, "PostQuitMessage and WM_QUIT retain original exit code");
    Check(LogContains("Application PostQuitMessage") && LogContains("EXE GetMessageA returned WM_QUIT"),
          "quit request and message-loop termination are logged");

    const int before = resizes;
    SendMessageW(frame, WM_ENTERSIZEMOVE, 0, 0);
    SendMessageW(frame, WM_SIZE, SIZE_RESTORED, MAKELPARAM(900, 650));
    SendMessageW(frame, WM_SIZE, SIZE_RESTORED, MAKELPARAM(950, 700));
    Check(resizes == before, "no native rebuild during drag");
    SendMessageW(frame, WM_EXITSIZEMOVE, 0, 0);
    Check(resizes == before + (nativeResize ? 1 : 0), "drag completion obeys graphics rebuild policy");
    SendMessageW(frame, WM_SIZE, SIZE_MINIMIZED, 0);
    SendMessageW(frame, WM_SIZE, SIZE_RESTORED, 0);
    Check(resizes == before + (nativeResize ? 1 : 0), "minimized and empty sizes not delivered");
    int lost = focusLosses;
    SendMessageW(lake, WM_KILLFOCUS, reinterpret_cast<WPARAM>(unrelated), 0);
    Check(focusLosses == lost, "external focus loss bypasses legacy SetFocus bounce");
    int active = activations;
    SendMessageW(lake, WM_ACTIVATEAPP, FALSE, 0);
    Check(activations == active + 1, "activation message still delivered");

    SetWindowLongW(lake, GWL_STYLE, WS_POPUP | WS_CAPTION);
    Check((GetWindowLongW(lake, GWL_STYLE) & WS_CHILD) && !(GetWindowLongW(lake, GWL_STYLE) & WS_POPUP), "SetWindowLong style guard");
    SetWindowPos(lake, HWND_TOPMOST, 20, 30, 100, 100, SWP_NOACTIVATE);
    RECT child{};
    GetClientRect(lake, &child);
    GetClientRect(frame, &actual);
    Check(nativeResize ? child.right == actual.right && child.bottom == actual.bottom
                       : child.right == originalClient.right && child.bottom == originalClient.bottom,
          "SetWindowPos obeys framebuffer size policy");
    if (!nativeResize) {
        MoveWindow(lake, 0, 0, 320, 200, FALSE);
        GetClientRect(lake, &child);
        Check(child.right == originalClient.right && child.bottom == originalClient.bottom,
              "diagnostic mode preserves original framebuffer dimensions on MoveWindow");
    }

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto create = reinterpret_cast<decltype(&CreateWindowExA)>(reinterpret_cast<void*>(GetProcAddress(user32, "CreateWindowExA")));
    Check(create != nullptr, "GetProcAddress hook is callable");
    auto change = reinterpret_cast<decltype(&ChangeDisplaySettingsExW)>(reinterpret_cast<void*>(GetProcAddress(user32, "ChangeDisplaySettingsExW")));
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    mode.dmPelsWidth = 123;
    mode.dmPelsHeight = 123;
    // Invalid device intentionally: even if the hook regresses, this cannot
    // change a real display mode.
    Check(change && change(L"LAKE_FIX_NONEXISTENT_DEVICE", &mode, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL,
          "dynamic display change is neutralized");
    Check(change && change(L"LAKE_FIX_NONEXISTENT_DEVICE", &mode, nullptr, CDS_TEST, nullptr) != DISP_CHANGE_SUCCESSFUL,
          "CDS_TEST is forwarded, not faked");

    vetoClose = true;
    SendMessageW(frame, WM_CLOSE, 0, 0);
    Check(IsWindow(lake) && IsWindow(frame) && closeRequests == 1, "frame close respects native veto");
    vetoClose = false;
    destroyQuitCode = 23;
    lastPumpedQuit = -1;
    SendMessageW(frame, WM_CLOSE, 0, 0);
    Pump();
    Check(!IsWindow(frame) && !IsWindow(lake) && closeRequests == 2, "frame close uses native cleanup then destroys host");
    lake = nullptr;
    Pump();
    Check(lastPumpedQuit == 23, "host cleanup preserves native WM_DESTROY quit code 23");
    destroyQuitCode = -1;
    // W entry point and cleanup/recreation coverage.
    lake = CreateWindowExW(0, L"Lake Controller", L"Unicode fixture", WS_POPUP,
                           0, 0, 1024, 768, nullptr, nullptr, instance, nullptr);
    Check(lake && GetParent(lake), "Unicode creation path and recreation");
    if (lake) {
        frame = GetParent(lake);
        const int beforeFallback = resizes;
        ShowWindow(lake, SW_SHOW); // No 0x8032: exercise the outer-loop fallback.
        for (int i = 0; i < 32 && !GetPropW(frame, L"LakeWindowedFix.HostSetupCompleted"); ++i) {
            if (GetMessageA(&start, nullptr, 0, 0) <= 0) break;
            TranslateMessage(&start);
            DispatchMessageA(&start);
        }
        Check(GetPropW(frame, L"LakeWindowedFix.HostSetupCompleted") && IsWindowVisible(frame) &&
              (nativeResize ? resizes > beforeFallback : resizes == beforeFallback),
              "outer GetMessage fallback obeys graphics rebuild policy");
        ShowWindow(lake, SW_HIDE);
        Check(!IsWindowVisible(frame), "native hide hides the frame");
        ShowWindow(lake, SW_SHOW);
        Check(IsWindowVisible(frame) && IsWindowVisible(lake), "native show restores frame and child");
        ShowWindow(lake, SW_MINIMIZE);
        Check(IsIconic(frame) && !IsIconic(lake), "native minimize targets frame, not renderer child");
        ShowWindow(lake, SW_RESTORE);
        Check(!IsIconic(frame) && IsWindowVisible(lake), "native restore restores visible renderer");
        SetWindowPos(lake, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
        SetWindowPos(lake, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        Check(IsWindowVisible(frame) && IsWindowVisible(lake), "SetWindowPos SHOWWINDOW also reveals frame");
    }
    HANDLE quitWorker = CreateThread(nullptr, 0, PostSyntheticWorkerQuit, nullptr, 0, nullptr);
    Check(quitWorker != nullptr, "unrelated synthetic quit worker created");
    const DWORD quitWorkerWait = quitWorker ? WaitForSingleObject(quitWorker, 2000) : WAIT_FAILED;
    Check(quitWorkerWait == WAIT_OBJECT_0, "unrelated synthetic quit worker finishes after posting 99");
    if (quitWorker) CloseHandle(quitWorker);
    lastPumpedQuit = -1;
    if (lake) DestroyWindow(GetParent(lake));
    lake = nullptr;
    Pump();
    Check(lastPumpedQuit == 0, "UI host posts quit zero after consumed quit 23 and unrelated worker quit 99");
    HMODULE renderer = LoadLibraryW(L"Contour.dll");
    Check(renderer != nullptr, "late synthetic Contour.dll loads");
    if (renderer) {
        // C export decoration differs across MSVC/MinGW; these are test-only.
        FARPROC factory = GetProcAddress(renderer, "FixtureCreateWindow");
        if (!factory) factory = GetProcAddress(renderer, "_FixtureCreateWindow");
        FARPROC display = GetProcAddress(renderer, "FixtureDisplayChange");
        if (!display) display = GetProcAddress(renderer, "_FixtureDisplayChange");
        FARPROC begin = GetProcAddress(renderer, "FixtureStart");
        if (!begin) begin = GetProcAddress(renderer, "_FixtureStart");
        FARPROC pump = GetProcAddress(renderer, "FixturePump");
        if (!pump) pump = GetProcAddress(renderer, "_FixturePump");
        auto make = reinterpret_cast<HWND (__cdecl*)()>(reinterpret_cast<void*>(factory));
        auto changeLate = reinterpret_cast<LONG (__cdecl*)()>(reinterpret_cast<void*>(display));
        auto startLate = reinterpret_cast<void (__cdecl*)(HWND)>(reinterpret_cast<void*>(begin));
        auto pumpLate = reinterpret_cast<void (__cdecl*)(HWND)>(reinterpret_cast<void*>(pump));
        lake = make ? make() : nullptr;
        Check(lake && GetParent(lake), "late DLL CreateWindowEx import patched before factory returns");
        if (lake && startLate && pumpLate) {
            const int beforeLateStart = resizes;
            startLate(lake);
            Check(IsWindowVisible(GetParent(lake)) && resizes == beforeLateStart,
                  "late DLL native show reveals host without rebuild");
            pumpLate(lake);
            Check(GetPropW(GetParent(lake), L"LakeWindowedFix.HostSetupCompleted") &&
                  (nativeResize ? resizes > beforeLateStart : resizes == beforeLateStart),
                  "client-filtered plugin loop initializes host with selected rebuild policy");
        } else Check(false, "late DLL native start exports available");
        Check(changeLate && changeLate() == DISP_CHANGE_SUCCESSFUL, "late DLL display import patched");
        if (lake) DestroyWindow(GetParent(lake));
        lake = nullptr;
        FreeLibrary(renderer);
        Pump();
    }
    Check(!LoadLibraryA("LakeFix.IntentionallyMissing.dll") && GetLastError() == ERROR_MOD_NOT_FOUND,
          "failed LoadLibrary preserves API result/error");
    DestroyWindow(unrelated);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
