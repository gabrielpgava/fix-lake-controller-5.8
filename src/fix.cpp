#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <cstring>
#include <cwchar>
#include "policy.h"

static_assert(sizeof(void*) == 4, "Build the compatibility DLL for x86");

// No detours, executable-page writes, background polling or work in DllMain.
// Only the main executable and Contour.dll's import slots are redirected.
namespace {
HMODULE self;
HMODULE application;
HWND host;
HWND client;
UINT readyMessage;
UINT clientGoneMessage;
bool sizing;
bool pendingSize;
bool inResize;
bool ready;
bool readyQueued;
WPARAM pendingType;
LPARAM pendingDimensions;
LONG initialization;
SRWLOCK importsLock = SRWLOCK_INIT;
SRWLOCK logLock = SRWLOCK_INIT;
HANDLE logFile = INVALID_HANDLE_VALUE;
constexpr UINT_PTR kSubclass = 0x4c574658;
// Build 007's plugin vtable+0x24 posts this before ShowWindow(main, 1).
constexpr UINT kNativeStartMessage = WM_APP + 0x32;

bool OpenLogIn(const wchar_t* directory) {
    wchar_t file[1200]{};
    if (swprintf_s(file, L"%ls\\LakeWindowedFix-%lu.log", directory, GetCurrentProcessId()) < 0)
        return false;
    if (!CreateDirectoryW(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    logFile = CreateFileW(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return logFile != INVALID_HANDLE_VALUE;
}

void InitializeLog() {
    wchar_t directory[1024]{};
    const DWORD length = GetModuleFileNameW(self, directory, ARRAYSIZE(directory));
    if (length && length < ARRAYSIZE(directory)) {
        wchar_t* slash = wcsrchr(directory, L'\\');
        if (slash && wcscpy_s(slash + 1, ARRAYSIZE(directory) - (slash + 1 - directory), L"logs") == 0 &&
            OpenLogIn(directory)) return;
    }
    const DWORD temporaryLength = GetTempPathW(ARRAYSIZE(directory), directory);
    if (temporaryLength && temporaryLength < ARRAYSIZE(directory) - 32) {
        wcscat_s(directory, L"LakeWindowedFix");
        OpenLogIn(directory);
    }
}

void Log(const wchar_t* text) {
    const DWORD error = GetLastError();
    wchar_t line[768]{};
    swprintf_s(line, L"[LakeWindowedFix 0.2] tick=%llu pid=%lu tid=%lu %ls\r\n",
               GetTickCount64(), GetCurrentProcessId(), GetCurrentThreadId(), text);
    OutputDebugStringW(line);
    if (logFile != INVALID_HANDLE_VALUE) {
        char encoded[3072]{};
        const int length = WideCharToMultiByte(CP_UTF8, 0, line, -1, encoded, sizeof(encoded), nullptr, nullptr);
        if (length > 1) {
            AcquireSRWLockExclusive(&logLock);
            DWORD written{};
            WriteFile(logFile, encoded, static_cast<DWORD>(length - 1), &written, nullptr);
            ReleaseSRWLockExclusive(&logLock);
        }
    }
    SetLastError(error);
}

bool ClientRequestsVisibility() {
    // IsWindowVisible(client) includes the ancestor's state. Using it here
    // would recreate the hidden-parent/hidden-child circular dependency.
    return client && (static_cast<DWORD>(GetWindowLongW(client, GWL_STYLE)) & WS_VISIBLE);
}

void QueueReady(const wchar_t* reason) {
    if (host && ClientRequestsVisibility() && !ready && !readyQueued &&
        GetCurrentThreadId() == GetWindowThreadProcessId(host, nullptr)) {
        // Route through the native client: legacy pumps can filter by HWND and
        // never retrieve a message posted only to the new parent frame.
        readyQueued = PostMessageW(client, readyMessage, 0, 0) != FALSE;
        if (readyQueued) Log(reason);
    }
}

bool IsName(const char* name, const char* expected) {
    return reinterpret_cast<ULONG_PTR>(name) > 0xffff &&
           _stricmp(name, expected) == 0;
}

bool IsName(const wchar_t* name, const wchar_t* expected) {
    return reinterpret_cast<ULONG_PTR>(name) > 0xffff &&
           _wcsicmp(name, expected) == 0;
}

RECT OuterSize(int width, int height) {
    RECT r{0, 0, width, height};
    AdjustWindowRectEx(&r, lakefix::HostStyle(0), FALSE, 0);
    return r;
}

void InitialPlacement(HWND window) {
    Log(L"Initial placement begin");
    wchar_t ini[32768]{};
    GetModuleFileNameW(self, ini, ARRAYSIZE(ini));
    wchar_t* slash = wcsrchr(ini, L'\\');
    if (slash) wcscpy_s(slash + 1, ARRAYSIZE(ini) - (slash + 1 - ini), L"LakeWindowedFix.ini");
    // Bound values before arithmetic (GetPrivateProfileInt also accepts negatives).
    UINT width = GetPrivateProfileIntW(L"window", L"Width", 1024, ini);
    UINT height = GetPrivateProfileIntW(L"window", L"Height", 768, ini);
    if (width < 800 || width > 8192) width = 1024;
    if (height < 600 || height > 8192) height = 768;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &monitor.rcWork, 0);
    const RECT border = OuterSize(0, 0);
    const int workWidth = monitor.rcWork.right - monitor.rcWork.left;
    const int workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
    int outerWidth = static_cast<int>(width) + border.right - border.left;
    int outerHeight = static_cast<int>(height) + border.bottom - border.top;
    if (outerWidth > workWidth && workWidth >= 800 + border.right - border.left) outerWidth = workWidth;
    if (outerHeight > workHeight && workHeight >= 600 + border.bottom - border.top) outerHeight = workHeight;
    sizing = true;
    SetWindowPos(window, HWND_NOTOPMOST,
                 monitor.rcWork.left + (workWidth - outerWidth) / 2,
                 monitor.rcWork.top + (workHeight - outerHeight) / 2,
                 outerWidth, outerHeight, SWP_NOACTIVATE | SWP_FRAMECHANGED);
    sizing = false;
    pendingSize = false;
    // This is queued after plugin initialization. One explicit WM_SIZE is also
    // needed when the configured dimensions happen to equal the initial size.
    RECT r{};
    GetClientRect(window, &r);
    SendMessageW(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
    // The native show request already controls visibility. Do not undo a
    // minimize/hide requested after the asynchronous initialization was queued.
    if (!IsWindowVisible(window) && ClientRequestsVisibility()) ShowWindow(window, SW_SHOWNORMAL);
    Log(L"Initial placement complete");
}

LRESULT CALLBACK HostProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                          UINT_PTR, DWORD_PTR) {
    if (message == readyMessage) {
        if (!ClientRequestsVisibility()) { readyQueued = false; return 0; }
        if (!ready) {
            ready = true;
            InitialPlacement(window);
            Log(L"Native windowed host ready");
        }
        return 0;
    }
    if (message == clientGoneMessage) {
        if (!client) DestroyWindow(window);
        return 0;
    }
    switch (message) {
    case WM_GETMINMAXINFO: {
        const RECT r = OuterSize(lakefix::kMinWidth, lakefix::kMinHeight);
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        limits->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return result;
    }
    case WM_ENTERSIZEMOVE:
        sizing = true;
        pendingSize = false;
        break;
    case WM_SIZE:
        // Keep zero/minimized buffers out of the native surface rebuild path.
        if (!ready || !lakefix::HasPixels(wparam, lparam) || inResize) return 0;
        if (sizing) {
            pendingSize = true;
            pendingType = wparam;
            pendingDimensions = lparam;
            return 0;
        } else {
            // ContourParent performs the native graphics/layout rebuild. Never
            // hold our import lock while calling it, and don't recurse on SIZE.
            inResize = true;
            Log(L"Native resize begin");
            LRESULT result = DefSubclassProc(window, message, wparam, lparam);
            Log(L"Native resize complete");
            inResize = false;
            InvalidateRect(client, nullptr, FALSE);
            return result;
        }
    case WM_EXITSIZEMOVE: {
        sizing = false;
        const bool deliver = pendingSize;
        pendingSize = false;
        LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        if (deliver) SendMessageW(window, WM_SIZE, pendingType, pendingDimensions);
        return result;
    }
    case WM_SETFOCUS:
        if (client) SetFocus(client);
        return 0;
    case WM_CLOSE:
        // The real main WndProc owns confirmations/cleanup. A frame close must
        // not destroy its child behind its back or ignore an application veto.
        if (client) SendMessageW(client, WM_CLOSE, wparam, lparam);
        else DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, HostProc, kSubclass);
        host = nullptr;
        ready = false;
        readyQueued = false;
        sizing = pendingSize = false;
        break;
    }
    // WM_ACTIVATE/WM_ACTIVATEAPP are not forged or globally swallowed.
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK ClientProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                            UINT_PTR, DWORD_PTR) {
    if (message == readyMessage) {
        if (window == client && host) return SendMessageW(host, message, wparam, lparam);
        return 0;
    }
    if (message == kNativeStartMessage) {
        // Preserve the original handler FIRST. This marker belongs to the
        // verified plugin show method, not an arbitrary startup PeekMessage.
        LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        if (window == client) QueueReady(L"Ready queued after native 0x8032 notification");
        return result;
    }
    if (message == WM_KILLFOCUS && reinterpret_cast<HWND>(wparam) != host &&
        !IsChild(host, reinterpret_cast<HWND>(wparam))) {
        // The observed legacy handler calls SetFocus(main) on BOTH SETFOCUS and
        // KILLFOCUS. Don't bounce focus back when it actually left this host.
        return IsWindowUnicode(window) ? DefWindowProcW(window, message, wparam, lparam)
                                       : DefWindowProcA(window, message, wparam, lparam);
    }
    if (message == WM_SYSCOMMAND) {
        const UINT command = static_cast<UINT>(wparam & 0xfff0);
        if (command == SC_MINIMIZE || command == SC_MAXIMIZE || command == SC_RESTORE ||
            command == SC_MOVE || command == SC_SIZE || command == SC_CLOSE) {
            return SendMessageW(host, message, wparam, lparam);
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, ClientProc, kSubclass);
        client = nullptr;
        // Don't recursively destroy a parent while its child is in teardown.
        if (host) PostMessageW(host, clientGoneMessage, 0, 0);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

HWND MakeHost(HINSTANCE instance, int width, int height) {
    WNDCLASSEXA cls{};
    cls.cbSize = sizeof(cls);
    if (host || !GetClassInfoExA(instance, "ContourParent", &cls)) {
        Log(L"Refusing fullscreen fallback: native ContourParent unavailable");
        SetLastError(ERROR_CLASS_DOES_NOT_EXIST);
        return nullptr;
    }
    const RECT r = OuterSize(width > 0 ? width : 1024, height > 0 ? height : 768);
    // Hidden until the native client asks to be shown. Visibility and graphics
    // rebuild readiness are deliberately separate (the 0.1 regression).
    host = CreateWindowExA(0, "ContourParent", "Lake Controller - Windowed",
                           lakefix::HostStyle(0), CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           nullptr, nullptr, instance, nullptr);
    if (!host) return nullptr;
    Log(L"Standard host created (hidden until native show request)");
    if (!SetWindowSubclass(host, HostProc, kSubclass, 0)) {
        DestroyWindow(host);
        host = nullptr;
    }
    return host;
}

bool AttachClient(HWND window) {
    if (!window || !SetWindowSubclass(window, ClientProc, kSubclass, 0)) return false;
    client = window;
    Log(L"Lake client hosted; native resize procedure retained");
    return true;
}

void QueueReadyAtMainLoop() {
    // A fallback, not the only way to reveal/initialize the host. Some startup
    // paths run a plugin-owned loop before the EXE reaches GetMessage.
    QueueReady(L"Ready queued by EXE GetMessage fallback");
}
BOOL WINAPI HookMessageA(LPMSG message, HWND window, UINT first, UINT last) {
    QueueReadyAtMainLoop();
    return GetMessageA(message, window, first, last);
}
BOOL WINAPI HookMessageW(LPMSG message, HWND window, UINT first, UINT last) {
    QueueReadyAtMainLoop();
    return GetMessageW(message, window, first, last);
}

HWND WINAPI HookCreateA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style,
                       int x, int y, int w, int h, HWND parent, HMENU menu,
                       HINSTANCE instance, LPVOID param) {
    if (parent || (style & WS_CHILD) || !IsName(cls, "Lake Controller"))
        return CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, instance, param);
    HWND frame = MakeHost(instance, w, h);
    if (!frame) return nullptr;
    HWND window = CreateWindowExA(lakefix::WindowExStyle(ex), cls, title,
                                 lakefix::ClientStyle(style), 0, 0, w, h,
                                 frame, menu, instance, param);
    if (AttachClient(window)) return window;
    DestroyWindow(frame);
    return nullptr;
}

HWND WINAPI HookCreateW(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style,
                       int x, int y, int w, int h, HWND parent, HMENU menu,
                       HINSTANCE instance, LPVOID param) {
    if (parent || (style & WS_CHILD) || !IsName(cls, L"Lake Controller"))
        return CreateWindowExW(ex, cls, title, style, x, y, w, h, parent, menu, instance, param);
    HWND frame = MakeHost(instance, w, h);
    if (!frame) return nullptr;
    HWND window = CreateWindowExW(lakefix::WindowExStyle(ex), cls, title,
                                 lakefix::ClientStyle(style), 0, 0, w, h,
                                 frame, menu, instance, param);
    if (AttachClient(window)) return window;
    DestroyWindow(frame);
    return nullptr;
}

LONG FilterLong(HWND window, int index, LONG value) {
    if (window == client || window == host) {
        if (index == GWL_STYLE) return static_cast<LONG>(window == host
            ? lakefix::HostStyle(static_cast<DWORD>(value))
            : lakefix::ClientStyle(static_cast<DWORD>(value)));
        if (index == GWL_EXSTYLE) return static_cast<LONG>(lakefix::WindowExStyle(static_cast<DWORD>(value)));
    }
    return value;
}

LONG WINAPI HookLongA(HWND w, int i, LONG v) { return SetWindowLongA(w, i, FilterLong(w, i, v)); }
LONG WINAPI HookLongW(HWND w, int i, LONG v) { return SetWindowLongW(w, i, FilterLong(w, i, v)); }

BOOL WINAPI HookPosition(HWND window, HWND after, int x, int y, int w, int h, UINT flags) {
    const bool isClient = window == client;
    if (window == client) {
        RECT r{};
        GetClientRect(host, &r);
        after = nullptr;
        x = y = 0;
        w = r.right;
        h = r.bottom;
        flags |= SWP_NOZORDER;
    } else if (window == host && after == HWND_TOPMOST) {
        after = HWND_NOTOPMOST;
    }
    const BOOL result = SetWindowPos(window, after, x, y, w, h, flags);
    const DWORD error = GetLastError();
    if (result && isClient && host) {
        if ((flags & SWP_SHOWWINDOW) && ClientRequestsVisibility()) {
            ShowWindow(host, SW_SHOWNOACTIVATE);
            Log(L"Host visibility mirrored from native SetWindowPos(SHOWWINDOW)");
        } else if (flags & SWP_HIDEWINDOW) ShowWindow(host, SW_HIDE);
    }
    SetLastError(error);
    return result;
}

BOOL WINAPI HookMove(HWND window, int x, int y, int w, int h, BOOL paint) {
    if (window == client) {
        RECT r{};
        GetClientRect(host, &r);
        x = y = 0;
        w = r.right;
        h = r.bottom;
    }
    return MoveWindow(window, x, y, w, h, paint);
}

BOOL WINAPI HookShow(HWND window, int command) {
    if (window != client || !host) return ShowWindow(window, command);
    const bool hostCommand = command == SW_MINIMIZE || command == SW_SHOWMINIMIZED ||
        command == SW_SHOWMINNOACTIVE || command == SW_MAXIMIZE || command == SW_RESTORE ||
        command == SW_FORCEMINIMIZE;
    // The child stays a normal child; minimize/maximize apply to the frame.
    const BOOL previous = ShowWindow(window, hostCommand ? SW_SHOWNA : command);
    const DWORD error = GetLastError();
    if (command == SW_HIDE) ShowWindow(host, SW_HIDE);
    else if (ClientRequestsVisibility()) ShowWindow(host, command);
    wchar_t line[160]{};
    swprintf_s(line, L"Native ShowWindow cmd=%d; client-visible-bit=%d host-visible=%d",
               command, ClientRequestsVisibility() ? 1 : 0, IsWindowVisible(host) ? 1 : 0);
    Log(line);
    // Visibility is mirrored synchronously, WITHOUT resize/rebuild here. The
    // 0x8032 notification or outer loop separately schedules initial placement.
    SetLastError(error);
    return previous;
}

// These APIs aren't in the observed normal imports; cover dynamic resolution
// too. CDS_TEST remains a real capability query, not an invented success.
LONG WINAPI HookDisplayA(DEVMODEA* mode, DWORD flags) {
    if (flags & CDS_TEST) return ChangeDisplaySettingsA(mode, flags);
    Log(L"Blocked ChangeDisplaySettingsA");
    return DISP_CHANGE_SUCCESSFUL;
}
LONG WINAPI HookDisplayW(DEVMODEW* mode, DWORD flags) {
    if (flags & CDS_TEST) return ChangeDisplaySettingsW(mode, flags);
    Log(L"Blocked ChangeDisplaySettingsW");
    return DISP_CHANGE_SUCCESSFUL;
}
LONG WINAPI HookDisplayExA(LPCSTR name, DEVMODEA* mode, HWND window, DWORD flags, LPVOID data) {
    if (flags & CDS_TEST) return ChangeDisplaySettingsExA(name, mode, window, flags, data);
    Log(L"Blocked ChangeDisplaySettingsExA");
    return DISP_CHANGE_SUCCESSFUL;
}
LONG WINAPI HookDisplayExW(LPCWSTR name, DEVMODEW* mode, HWND window, DWORD flags, LPVOID data) {
    if (flags & CDS_TEST) return ChangeDisplaySettingsExW(name, mode, window, flags, data);
    Log(L"Blocked ChangeDisplaySettingsExW");
    return DISP_CHANGE_SUCCESSFUL;
}

bool PatchModule(HMODULE module);

HMODULE WINAPI HookLoadA(LPCSTR name) {
    HMODULE module = LoadLibraryA(name);
    const DWORD error = GetLastError();
    if (module && module == GetModuleHandleW(L"Contour.dll") && !PatchModule(module)) {
        // Don't let the caller initialize a partially hooked graphics module.
        FreeLibrary(module);
        SetLastError(ERROR_INVALID_FUNCTION);
        return nullptr;
    }
    if (module && module == GetModuleHandleW(L"Contour.dll")) Log(L"Contour imports patched after LoadLibraryA");
    SetLastError(error);
    return module;
}
HMODULE WINAPI HookLoadW(LPCWSTR name) {
    HMODULE module = LoadLibraryW(name);
    const DWORD error = GetLastError();
    if (module && module == GetModuleHandleW(L"Contour.dll") && !PatchModule(module)) {
        FreeLibrary(module);
        SetLastError(ERROR_INVALID_FUNCTION);
        return nullptr;
    }
    if (module && module == GetModuleHandleW(L"Contour.dll")) Log(L"Contour imports patched after LoadLibraryW");
    SetLastError(error);
    return module;
}

struct Hook {
    const char* dll;
    const char* name;
    PVOID replacement;
    bool applicationOnly;
    HMODULE provider;
    PVOID original;
};
FARPROC WINAPI HookGetProc(HMODULE module, LPCSTR name);
#define HOOK(dll, name, function) {dll, name, reinterpret_cast<PVOID>(function), false, nullptr, nullptr}
Hook hooks[] = {
    HOOK("USER32.dll", "CreateWindowExA", HookCreateA),
    HOOK("USER32.dll", "CreateWindowExW", HookCreateW),
    // x86 SetWindowLongPtr is a header alias of SetWindowLong, not an export.
    HOOK("USER32.dll", "SetWindowLongA", HookLongA),
    HOOK("USER32.dll", "SetWindowLongW", HookLongW),
    HOOK("USER32.dll", "SetWindowPos", HookPosition),
    HOOK("USER32.dll", "MoveWindow", HookMove),
    HOOK("USER32.dll", "ShowWindow", HookShow),
    HOOK("USER32.dll", "ChangeDisplaySettingsA", HookDisplayA),
    HOOK("USER32.dll", "ChangeDisplaySettingsW", HookDisplayW),
    HOOK("USER32.dll", "ChangeDisplaySettingsExA", HookDisplayExA),
    HOOK("USER32.dll", "ChangeDisplaySettingsExW", HookDisplayExW),
    HOOK("KERNEL32.dll", "LoadLibraryA", HookLoadA),
    HOOK("KERNEL32.dll", "LoadLibraryW", HookLoadW),
    HOOK("KERNEL32.dll", "GetProcAddress", HookGetProc),
    {"USER32.dll", "GetMessageA", reinterpret_cast<PVOID>(HookMessageA), true, nullptr, nullptr},
    {"USER32.dll", "GetMessageW", reinterpret_cast<PVOID>(HookMessageW), true, nullptr, nullptr},
};
#undef HOOK

FARPROC WINAPI HookGetProc(HMODULE module, LPCSTR name) {
    FARPROC original = GetProcAddress(module, name);
    if (!original || reinterpret_cast<ULONG_PTR>(name) <= 0xffff) return original;
    for (const auto& hook : hooks) {
        // Compare both module identity and name; never replace another DLL's
        // coincidentally named export. Forwarders are resolved by GetProcAddress.
        if (!hook.applicationOnly && module == hook.provider && strcmp(name, hook.name) == 0)
            return reinterpret_cast<FARPROC>(hook.replacement);
    }
    return original;
}

bool PatchModule(HMODULE module) {
    if (!module || module == self) return true;
    AcquireSRWLockExclusive(&importsLock);
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    bool success = dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE &&
                   nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    if (success) {
        const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.VirtualAddress && dir.Size >= sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            auto* descriptors = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
            for (size_t d = 0; d < dir.Size / sizeof(*descriptors) && descriptors[d].Name; ++d) {
                const auto& descriptor = descriptors[d];
                const char* dll = reinterpret_cast<char*>(base + descriptor.Name);
                if (!descriptor.OriginalFirstThunk) { success = false; break; }
                auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor.OriginalFirstThunk);
                auto* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor.FirstThunk);
                for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
                    if (IMAGE_SNAP_BY_ORDINAL32(names[i].u1.Ordinal)) continue;
                    auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
                    for (const auto& hook : hooks) {
                        if (hook.applicationOnly && module != application) continue;
                        if (_stricmp(dll, hook.dll) || strcmp(imported->Name, hook.name)) continue;
                        auto* slot = reinterpret_cast<PVOID volatile*>(&slots[i].u1.Function);
                        PVOID replacement = hook.replacement;
                        if (*slot == replacement) continue;
                        if (*slot != hook.original) {
                            Log(L"Refusing to overwrite a third-party IAT hook");
                            success = false;
                            continue;
                        }
                        DWORD protection;
                        if (!VirtualProtect(const_cast<PVOID*>(slot), sizeof(PVOID), PAGE_READWRITE, &protection)) {
                            success = false;
                            continue;
                        }
                        PVOID previous = InterlockedCompareExchangePointer(slot, replacement, hook.original);
                        if (previous != hook.original && previous != replacement) {
                            Log(L"IAT changed concurrently; third-party hook preserved");
                            success = false;
                        }
                        DWORD ignored;
                        if (!VirtualProtect(const_cast<PVOID*>(slot), sizeof(PVOID), protection, &ignored)) success = false;
                    }
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&importsLock);
    return success;
}
} // namespace

extern "C" DWORD WINAPI LakeWindowedInitialize(void*) {
    if (InterlockedCompareExchange(&initialization, 1, 0) != 0) return ERROR_ALREADY_INITIALIZED;
    InitializeLog();
    Log(L"Initialize begin");
    readyMessage = RegisterWindowMessageW(L"LakeWindowedFix.Ready.v1");
    clientGoneMessage = RegisterWindowMessageW(L"LakeWindowedFix.ClientGone.v1");
    if (!readyMessage || !clientGoneMessage) return GetLastError();
    // Check the launcher-provided native flags. Without them, border changes
    // alone cannot remove DirectDraw exclusive cooperative mode.
    const wchar_t* command = GetCommandLineW();
    if (!wcsstr(command, L"windowed") || !wcsstr(command, L"nonfullscreen")) {
        Log(L"Required native flags missing; no hooks installed");
        return ERROR_INVALID_PARAMETER;
    }
    // Resolve outside importsLock: never invert it against the loader lock.
    for (auto& hook : hooks) {
        hook.provider = GetModuleHandleA(hook.dll);
        hook.original = reinterpret_cast<PVOID>(GetProcAddress(hook.provider, hook.name));
        if (!hook.original) return ERROR_PROC_NOT_FOUND;
    }
    application = GetModuleHandleW(nullptr);
    if (!PatchModule(application) || !PatchModule(GetModuleHandleW(L"Contour.dll")))
        return ERROR_INVALID_FUNCTION;
    // Imports point into this DLL until process exit. Unloading is unsupported.
    HMODULE pinned;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&LakeWindowedInitialize), &pinned))
        return GetLastError();
    Log(L"Initialized outside loader lock; native flags present");
    return ERROR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) self = instance;
    return TRUE;
}
