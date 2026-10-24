#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include "../src/policy.h"

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

void Check(bool condition, const char* name) {
    printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
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
    return DefWindowProcA(window, message, wparam, lparam);
}
void Pump() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
} // namespace

int wmain() {
    Check(sizeof(void*) == 4, "x86 fixture");
    Check(lakefix::HostStyle(WS_POPUP) & WS_CAPTION, "host caption policy");
    Check((lakefix::ClientStyle(WS_POPUP | WS_CAPTION) & (WS_POPUP | WS_CAPTION)) == 0, "child removes popup/caption");
    Check(!lakefix::HasPixels(SIZE_MINIMIZED, MAKELPARAM(800, 600)), "minimize policy");
    Check(!lakefix::HasPixels(SIZE_RESTORED, 0), "zero-size policy");
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
    Check(frame != nullptr, "client has standard host");
    Check((GetWindowLongW(lake, GWL_STYLE) & WS_CHILD) != 0, "native renderer is a child");
    Check((GetWindowLongW(frame, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW, "resizable host style");
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
    Check(startupNotifications == 1 && resizes >= 1,
          "native startup notification initializes host without EXE message loop");
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
    Check(resizes >= 1 && actual.right >= 800 && actual.bottom >= 600, "deferred native initialization and min area");

    const int before = resizes;
    SendMessageW(frame, WM_ENTERSIZEMOVE, 0, 0);
    SendMessageW(frame, WM_SIZE, SIZE_RESTORED, MAKELPARAM(900, 650));
    SendMessageW(frame, WM_SIZE, SIZE_RESTORED, MAKELPARAM(950, 700));
    Check(resizes == before, "no native rebuild during drag");
    SendMessageW(frame, WM_EXITSIZEMOVE, 0, 0);
    Check(resizes == before + 1, "one native rebuild after drag");
    SendMessageW(frame, WM_SIZE, SIZE_MINIMIZED, 0);
    SendMessageW(frame, WM_SIZE, SIZE_RESTORED, 0);
    Check(resizes == before + 1, "minimized and empty sizes not delivered");
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
    Check(child.right == actual.right && child.bottom == actual.bottom, "SetWindowPos keeps child in client area");

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
    SendMessageW(frame, WM_CLOSE, 0, 0);
    Pump();
    Check(!IsWindow(frame) && !IsWindow(lake) && closeRequests == 2, "frame close uses native cleanup then destroys host");
    lake = nullptr;
    Pump();
    // W entry point and cleanup/recreation coverage.
    lake = CreateWindowExW(0, L"Lake Controller", L"Unicode fixture", WS_POPUP,
                           0, 0, 1024, 768, nullptr, nullptr, instance, nullptr);
    Check(lake && GetParent(lake), "Unicode creation path and recreation");
    if (lake) {
        frame = GetParent(lake);
        const int beforeFallback = resizes;
        ShowWindow(lake, SW_SHOW); // No 0x8032: exercise the outer-loop fallback.
        for (int i = 0; i < 32 && resizes == beforeFallback; ++i) {
            if (GetMessageA(&start, nullptr, 0, 0) <= 0) break;
            TranslateMessage(&start);
            DispatchMessageA(&start);
        }
        Check(resizes > beforeFallback && IsWindowVisible(frame), "outer GetMessage remains a working fallback");
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
    if (lake) DestroyWindow(GetParent(lake));
    lake = nullptr;
    Pump();
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
            Check(resizes > beforeLateStart, "plugin-owned client-filtered PeekMessage loop initializes host");
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
