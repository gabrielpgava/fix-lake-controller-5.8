#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Synthetic Contour.dll, built only for tests. NOT a replacement for the Lake
// DLL and never installed into dist/ or the real application directory.
extern "C" __declspec(dllexport) HWND __cdecl FixtureCreateWindow() {
    return CreateWindowExW(0, L"Lake Controller", L"Late loaded fixture", WS_POPUP,
                           0, 0, 1024, 768, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

extern "C" __declspec(dllexport) LONG __cdecl FixtureDisplayChange() {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    mode.dmPelsWidth = 123;
    mode.dmPelsHeight = 123;
    return ChangeDisplaySettingsExW(L"LAKE_FIX_NONEXISTENT_DEVICE", &mode, nullptr, CDS_FULLSCREEN, nullptr);
}

extern "C" __declspec(dllexport) void __cdecl FixtureStart(HWND window) {
    // Same order as the verified native plugin's show method.
    PostMessageA(window, 0x8032, 0, 0);
    ShowWindow(window, SW_SHOWNORMAL);
}

extern "C" __declspec(dllexport) void __cdecl FixturePump(HWND window) {
    MSG message;
    while (PeekMessageA(&message, window, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) continue;
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
}
