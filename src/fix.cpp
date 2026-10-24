#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <cstring>
#include <cwchar>
#include <atomic>
#include "policy.h"
#include "log_lock.h"
#include "scale_geometry.h"
#include "ddraw_scale.h"
#ifdef _MSC_VER
#include <intrin.h>
#define LAKE_CALLER_ADDRESS() _ReturnAddress()
#else
#define LAKE_CALLER_ADDRESS() __builtin_return_address(0)
#endif

static_assert(sizeof(void*) == 4, "Build the compatibility DLL for x86");

// No detours, executable-page writes, background polling or work in DllMain.
// Imports are redirected only in the EXE/Contour. The scaled presenter owns
// complete forwarding COM proxies, never changes a native object's vtable,
// and seeds two guarded writable layout dimensions once at graphics startup.
namespace {
HMODULE self;
HMODULE application;
HWND host;
HWND client;
SIZE originalClientSize;
UINT readyMessage;
UINT clientGoneMessage;
bool sizing;
bool pendingSize;
bool inResize;
bool ready;
bool readyQueued;
bool nativeResizeEnabled;
bool scaleRenderingEnabled = true;
SIZE renderSize{1088, 612};
SIZE initialViewSize{1024, 576};
bool canvasLayoutSeeded;
bool closeAfterStartup;
std::atomic<DWORD> presentationSize{0};
WPARAM pendingType;
LPARAM pendingDimensions;
LONG initialization;
SRWLOCK importsLock = SRWLOCK_INIT;
SRWLOCK logLock = SRWLOCK_INIT;
HANDLE logFile = INVALID_HANDLE_VALUE;
LONG observingException;
ULONG_PTR applicationBytes;
HMODULE renderer;
ULONG_PTR rendererBytes;
std::atomic<DWORD> uiThreadId{0};
std::atomic<bool> nativeUiQuitPending{false};
std::atomic<int> nativeUiQuitCode{0};
constexpr UINT_PTR kSubclass = 0x4c574658;
// Build 007's plugin vtable+0x24 posts this before ShowWindow(main, 1).
constexpr UINT kNativeStartMessage = WM_APP + 0x32;
constexpr char kScaledHostClass[] = "LakeWindowedFix.ScaledHost.v1";
using DrawCreateEx = HRESULT (WINAPI*)(GUID*, void**, const GUID&, void*);
std::atomic<DrawCreateEx> originalDrawCreateEx{nullptr};

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

void Log(const wchar_t* text, bool nonBlocking = false) {
    const DWORD error = GetLastError();
    wchar_t line[768]{};
    swprintf_s(line, L"[LakeWindowedFix 0.4] tick=%llu pid=%lu tid=%lu %ls\r\n",
               GetTickCount64(), GetCurrentProcessId(), GetCurrentThreadId(), text);
    // Exit/exception observers must not wait for a debugger notification.
    if (!nonBlocking) OutputDebugStringW(line);
    if (logFile != INVALID_HANDLE_VALUE) {
        char encoded[3072]{};
        const int length = WideCharToMultiByte(CP_UTF8, 0, line, -1, encoded, sizeof(encoded), nullptr, nullptr);
        if (length > 1) {
            const bool acquired = lakefix::AcquireDiagnosticLock(logLock, nonBlocking);
            if (acquired) {
                DWORD written{};
                WriteFile(logFile, encoded, static_cast<DWORD>(length - 1), &written, nullptr);
                ReleaseSRWLockExclusive(&logLock);
            }
        }
    }
    SetLastError(error);
}

ULONG_PTR ImageBytes(HMODULE module) {
    if (!module) return 0;
    const auto* base = reinterpret_cast<const BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

void LogSite(const wchar_t* event, DWORD code, const void* address, bool nonBlocking = false) {
    const DWORD error = GetLastError();
    const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(address);
    const ULONG_PTR appBase = reinterpret_cast<ULONG_PTR>(application);
    const ULONG_PTR rendererBase = reinterpret_cast<ULONG_PTR>(renderer);
    const wchar_t* name = L"outside-cached-target-images";
    ULONG_PTR rva = 0;
    if (application && value >= appBase && value - appBase < applicationBytes) {
        name = L"main-executable";
        rva = value - appBase;
    } else if (renderer && value >= rendererBase && value - rendererBase < rendererBytes) {
        name = L"Contour.dll";
        rva = value - rendererBase;
    }
    wchar_t text[384]{};
    swprintf_s(text, L"%ls code=%lu (0x%08lx) address=0x%08lx cached-image=%ls rva=0x%08lx",
               event, code, code, static_cast<unsigned long>(value), name, static_cast<unsigned long>(rva));
    Log(text, nonBlocking);
    SetLastError(error);
}

LONG CALLBACK ObserveException(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // First chance can be handled by the original app. Never swallow it, and
    // don't try to allocate/log on an exhausted stack or on normal C++ throws.
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
         code == EXCEPTION_INT_DIVIDE_BY_ZERO) &&
        InterlockedCompareExchange(&observingException, 1, 0) == 0) {
        LogSite(L"First-chance exception (may be handled by application)", code,
                info->ExceptionRecord->ExceptionAddress, true);
        InterlockedExchange(&observingException, 0);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void LoadResizePolicy() {
    wchar_t ini[1024]{};
    const DWORD length = GetModuleFileNameW(self, ini, ARRAYSIZE(ini));
    if (length && length < ARRAYSIZE(ini)) {
        wchar_t* slash = wcsrchr(ini, L'\\');
        if (slash && wcscpy_s(slash + 1, ARRAYSIZE(ini) - (slash + 1 - ini), L"LakeWindowedFix.ini") == 0) {
            nativeResizeEnabled = GetPrivateProfileIntW(L"compatibility", L"NativeResize", 0, ini) == 1;
            scaleRenderingEnabled = !nativeResizeEnabled &&
                GetPrivateProfileIntW(L"compatibility", L"ScaleRendering", 1, ini) == 1;
            initialViewSize.cx = GetPrivateProfileIntW(L"window", L"Width", 1024, ini);
            initialViewSize.cy = GetPrivateProfileIntW(L"window", L"Height", 576, ini);
            renderSize.cx = GetPrivateProfileIntW(L"render", L"Width", 1088, ini);
            renderSize.cy = GetPrivateProfileIntW(L"render", L"Height", 612, ini);
        }
    }
    // Missing INI uses the new safe scaling mode, not native resource rebuild.
    if (!length || length >= ARRAYSIZE(ini) || !wcsrchr(ini, L'\\')) scaleRenderingEnabled = !nativeResizeEnabled;
    if (initialViewSize.cx < 320 || initialViewSize.cx > 8192) initialViewSize.cx = 1024;
    if (initialViewSize.cy < 180 || initialViewSize.cy > 8192) initialViewSize.cy = 576;
    if (renderSize.cx < 1024 || renderSize.cx > 4096) renderSize.cx = 1088;
    if (renderSize.cy < 600 || renderSize.cy > 4096) renderSize.cy = 612;
    if (scaleRenderingEnabled) {
        Log(L"ScaleRendering=1: resizable scaled presentation; fixed virtual canvas; no native graphics rebuild");
        return;
    }
    Log(nativeResizeEnabled ? L"NativeResize=1: EXPERIMENTAL graphics teardown/rebuild enabled"
                            : L"NativeResize=0: original framebuffer size retained; no native graphics rebuild");
}

DWORD FrameStyle(DWORD style) {
    DWORD result = lakefix::HostStyle(style);
    // The DirectDraw clipper is bound to this host so it can upscale beyond
    // the fixed child. WS_CLIPCHILDREN would make its visible region EMPTY
    // where the canvas child covers the host (confirmed with real GetClipList).
    if (scaleRenderingEnabled) result &= ~WS_CLIPCHILDREN;
    if (!nativeResizeEnabled && !scaleRenderingEnabled) result &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    return result;
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
    AdjustWindowRectEx(&r, FrameStyle(0), FALSE, 0);
    return r;
}

SIZE CurrentSourceSize() {
    const DWORD packed = presentationSize.load();
    return packed ? SIZE{static_cast<LONG>(LOWORD(packed)), static_cast<LONG>(HIWORD(packed))}
                  : originalClientSize;
}

bool LocalViewport(SIZE source, RECT& area, RECT& viewport) {
    if (!scaleRenderingEnabled || !host || !client || IsIconic(host) ||
        source.cx <= 0 || source.cy <= 0 || !GetClientRect(host, &area) ||
        area.right <= 0 || area.bottom <= 0) return false;
    viewport = lakefix::FitViewport(area.right, area.bottom, source.cx, source.cy);
    return viewport.right > viewport.left && viewport.bottom > viewport.top;
}

bool ScaledViewport(HWND window, LONG width, LONG height, RECT* full, RECT* fitted) noexcept {
    if (window != client || width <= 0 || height <= 0 || width > 16384 || height > 16384) return false;
    const DWORD saved = GetLastError();
    RECT area{}, view{};
    POINT origin{};
    const bool valid = LocalViewport({width, height}, area, view) && ClientToScreen(host, &origin);
    if (valid) {
        OffsetRect(&area, origin.x, origin.y);
        OffsetRect(&view, origin.x, origin.y);
        *full = area;
        *fitted = view;
        presentationSize.store(MAKELONG(width, height));
    }
    SetLastError(saved);
    return valid;
}

void PresenterLog(const wchar_t* message) noexcept { Log(message, true); }

HWND ClipWindow(HWND window) noexcept {
    // Clip to the visible host, not the fixed-size child. Otherwise enlarging
    // the presentation beyond the canvas HWND would crop the right/bottom.
    return scaleRenderingEnabled && window == client ? host : nullptr;
}

bool PaintLetterbox(HWND window, const RECT* strips, UINT count) noexcept {
    const DWORD saved = GetLastError();
    RECT area{};
    POINT origin{};
    if (window != client || !host || !strips || count > 4 || !GetClientRect(host, &area) ||
        !ClientToScreen(host, &origin)) { SetLastError(saved); return false; }
    RECT local[4]{};
    for (UINT i = 0; i < count; ++i) {
        local[i] = strips[i];
        OffsetRect(&local[i], -origin.x, -origin.y);
        if (local[i].left < 0 || local[i].top < 0 || local[i].right > area.right || local[i].bottom > area.bottom) {
            SetLastError(saved); return false;
        }
    }
    HDC dc = GetDC(host);
    bool result = dc != nullptr;
    if (dc) {
        for (UINT i = 0; i < count; ++i)
            if (local[i].right > local[i].left && local[i].bottom > local[i].top &&
                !FillRect(dc, &local[i], reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)))) result = false;
        if (!GdiFlush()) result = false;
        ReleaseDC(host, dc);
    }
    SetLastError(saved);
    return result;
}

bool MapClientPoint(POINT real, POINT& mapped) {
    RECT area{}, view{};
    const SIZE source = CurrentSourceSize();
    if (!LocalViewport(source, area, view)) return false;
    mapped = lakefix::ViewToSource(view, source, real);
    return true;
}

bool MapScreenPoint(POINT real, POINT& mapped) {
    POINT origin{};
    if (!client || !ClientToScreen(client, &origin)) return false;
    POINT source{};
    if (!MapClientPoint({real.x - origin.x, real.y - origin.y}, source)) return false;
    mapped = {origin.x + source.x, origin.y + source.y};
    return true;
}

bool RendererCallsite(const void* caller, ULONG_PTR rva) {
    return renderer && reinterpret_cast<ULONG_PTR>(caller) == reinterpret_cast<ULONG_PTR>(renderer) + rva;
}

struct CursorRoundTrip {
    bool armed = false;
    HWND window = nullptr;
    POINT real{};
    POINT virtualPoint{};
    RECT viewport{};
    SIZE source{};
};
thread_local CursorRoundTrip cursorRoundTrip;
thread_local bool deliveringClientWheel;

BOOL WINAPI HookCursorPosition(LPPOINT point) {
    const void* caller = LAKE_CALLER_ADDRESS();
    cursorRoundTrip.armed = false;
    const BOOL result = GetCursorPos(point);
    const DWORD saved = GetLastError();
    if (result && point && scaleRenderingEnabled && client && host) {
        const POINT real = *point;
        const HWND under = WindowFromPoint(*point);
        RECT hostArea{};
        POINT hostOrigin{};
        const bool inHostClient = under == host && GetClientRect(host, &hostArea) &&
            ClientToScreen(host, &hostOrigin) && point->x >= hostOrigin.x && point->y >= hostOrigin.y &&
            point->x < hostOrigin.x + hostArea.right && point->y < hostOrigin.y + hostArea.bottom;
        const HWND capture = GetCapture();
        if (under == client || IsChild(client, under) || inHostClient || capture == client || IsChild(client, capture)) {
            POINT origin{};
            POINT local = *point;
            if (ClientToScreen(client, &origin)) {
                local.x -= origin.x;
                local.y -= origin.y;
                POINT mapped{};
                if (MapClientPoint(local, mapped)) {
                    point->x = origin.x + mapped.x;
                    point->y = origin.y + mapped.y;
                    // Verified build-007 getter -> offset -> setter pair.
                    // Other setters (notably SetCursorPos(0,0) during rebuild)
                    // must retain physical/screen semantics, not be inverted.
                    if (RendererCallsite(caller, 0x36fc43)) {
                        RECT area{}, view{};
                        const SIZE source = CurrentSourceSize();
                        if (LocalViewport(source, area, view))
                            cursorRoundTrip = {true, client, real, *point, view, source};
                    }
                }
            }
        }
    }
    SetLastError(saved);
    return result;
}

BOOL WINAPI HookSetCursorPosition(int x, int y) {
    const void* caller = LAKE_CALLER_ADDRESS();
    const CursorRoundTrip previous = cursorRoundTrip;
    cursorRoundTrip.armed = false;
    if (previous.armed && client == previous.window && scaleRenderingEnabled &&
        RendererCallsite(caller, 0x36fc55)) {
        const SIZE view{previous.viewport.right - previous.viewport.left, previous.viewport.bottom - previous.viewport.top};
        const POINT real = lakefix::TranslateCursorDelta(previous.real, previous.virtualPoint, {x, y}, view, previous.source);
        x = real.x;
        y = real.y;
    }
    return SetCursorPos(x, y);
}

void MapMessagePoint(MSG* message) {
    if (!message || !scaleRenderingEnabled || !client ||
        message->message < WM_MOUSEFIRST || message->message > WM_MOUSELAST ||
        (message->hwnd != client && message->hwnd != host && !IsChild(client, message->hwnd))) return;
    const bool wheel = message->message == WM_MOUSEWHEEL || message->message == WM_MOUSEHWHEEL;
    if (wheel) message->hwnd = client; // Original main wheel router scales it once.
    else {
        POINT raw{static_cast<short>(LOWORD(message->lParam)), static_cast<short>(HIWORD(message->lParam))};
        MapWindowPoints(message->hwnd, client, &raw, 1);
        POINT source{};
        if (!MapClientPoint(raw, source)) return;
        HWND target = GetCapture();
        if (target != client && !IsChild(client, target)) {
            target = client;
            if (source.x >= 0 && source.y >= 0) {
                // Native HWND hit regions are in virtual canvas coordinates.
                // At the picture's ButtonBar position the OS can hit WorkWindow.
                for (int depth = 0; depth < 16; ++depth) {
                    POINT local = source;
                    MapWindowPoints(client, target, &local, 1);
                    const HWND child = ChildWindowFromPointEx(target, local,
                        CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
                    if (!child || child == target || !IsChild(client, child)) break;
                    target = child;
                }
            }
        }
        if (target == client) message->lParam = MAKELPARAM(raw.x, raw.y);
        else {
            MapWindowPoints(client, target, &source, 1);
            message->lParam = MAKELPARAM(source.x, source.y);
        }
        message->hwnd = target;
    }
    POINT mapped{};
    // Contour's private loop also consumes MSG.pt directly.
    // Main lParam is mapped at dispatch; native child local lParam was mapped
    // above, since native child WndProcs are deliberately not subclassed.
    if (MapScreenPoint(message->pt, mapped)) message->pt = mapped;
}

BOOL WINAPI HookClientRectangle(HWND window, LPRECT rectangle) {
    const void* caller = LAKE_CALLER_ADDRESS();
    const BOOL result = GetClientRect(window, rectangle);
    const DWORD error = GetLastError();
    if (result && rectangle && window == client && scaleRenderingEnabled && !canvasLayoutSeeded &&
        RendererCallsite(caller, 0x369005) && rectangle->right == renderSize.cx && rectangle->bottom == renderSize.cy) {
        // Build 007 uses display dimensions both to choose an adapter mode and
        // to seed its layout. Leave capability/mode enumeration truthful, then
        // seed ONLY the two writable layout dimensions after mode selection,
        // at the original windowed-surface GetClientRect query. No code patch,
        // GPU reset, display-mode spoof, or periodic resize writes are involved.
        constexpr ULONG_PTR dimensionsRva = 0xc8aa34;
        auto* dimensions = reinterpret_cast<LONG*>(reinterpret_cast<BYTE*>(renderer) + dimensionsRva);
        MEMORY_BASIC_INFORMATION memory{};
        const bool valid = dimensionsRva + 2 * sizeof(LONG) <= rendererBytes &&
            VirtualQuery(dimensions, &memory, sizeof(memory)) == sizeof(memory) &&
            memory.State == MEM_COMMIT && (memory.Protect & 0xff) == PAGE_READWRITE &&
            reinterpret_cast<ULONG_PTR>(dimensions + 2) <= reinterpret_cast<ULONG_PTR>(memory.BaseAddress) + memory.RegionSize &&
            dimensions[0] > 0 && dimensions[1] > 0 && dimensions[0] <= 32768 && dimensions[1] <= 32768;
        if (valid) {
            dimensions[0] = renderSize.cx;
            dimensions[1] = renderSize.cy;
            canvasLayoutSeeded = true;
            Log(L"Virtual canvas layout seeded after native adapter/mode selection", true);
        } else Log(L"Virtual canvas layout unavailable: writable build-007 dimension guard failed", true);
    }
    SetLastError(error);
    return result;
}

bool ResolveDrawCreateEx() {
    if (!scaleRenderingEnabled) return true;
    const HMODULE draw = GetModuleHandleW(L"DDRAW.dll");
    if (!draw) return true; // Loaded later as a dependency of Contour.dll.
    const auto original = reinterpret_cast<DrawCreateEx>(reinterpret_cast<void*>(GetProcAddress(draw, "DirectDrawCreateEx")));
    if (!original) return false;
    originalDrawCreateEx.store(original);
    return true;
}

HRESULT WINAPI HookDrawCreateEx(GUID* guid, void** object, const GUID& iid, void* outer) {
    const auto original = originalDrawCreateEx.load();
    if (!original) return static_cast<HRESULT>(0x80004005L);
    const HRESULT result = original(guid, object, iid, outer);
    const DWORD saved = GetLastError();
    static constexpr GUID draw7{0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0, 0x60, 0x97, 0x97, 0xea, 0x5b}};
    if (SUCCEEDED(result) && object && *object && memcmp(&iid, &draw7, sizeof(iid)) == 0 &&
        !lakefix::WrapDirectDraw7(object))
        Log(L"Scaled presentation unavailable: could not wrap the DirectDraw7 object", true);
    SetLastError(saved);
    return result;
}

void InitialPlacement(HWND window) {
    Log(L"Initial placement begin");
    if (!nativeResizeEnabled) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
            RECT rectangle{};
            GetWindowRect(window, &rectangle);
            const int x = scaleRenderingEnabled ? monitor.rcWork.left +
                ((monitor.rcWork.right - monitor.rcWork.left) - (rectangle.right - rectangle.left)) / 2 : monitor.rcWork.left;
            const int y = scaleRenderingEnabled ? monitor.rcWork.top +
                ((monitor.rcWork.bottom - monitor.rcWork.top) - (rectangle.bottom - rectangle.top)) / 2 : monitor.rcWork.top;
            SetWindowPos(window, HWND_NOTOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        Log(L"Initial placement complete WITHOUT WM_SIZE or framebuffer size change");
        return;
    }
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
    const bool wheel = message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
    // DefWindowProc propagates an unconsumed wheel from the child to its
    // parent. Never bounce that delivery back to the child or scale it twice.
    if (scaleRenderingEnabled && client && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST &&
        !(wheel && deliveringClientWheel))
        return SendMessageW(client, message, wparam, lparam);
    if (message == readyMessage) {
        if (!ClientRequestsVisibility()) { readyQueued = false; return 0; }
        if (!ready) {
            ready = true;
            InitialPlacement(window);
            if (window == host && IsWindow(window) && IsWindow(client)) {
                SetPropW(window, L"LakeWindowedFix.HostSetupCompleted", reinterpret_cast<HANDLE>(1));
                Log(L"Host setup complete (not a renderer health check)");
                if (closeAfterStartup) {
                    closeAfterStartup = false;
                    PostMessageW(window, WM_CLOSE, 0, 0);
                }
            } else Log(L"Host/client destroyed during initial placement; setup not marked complete");
        }
        return 0;
    }
    if (message == clientGoneMessage) {
        if (!client) DestroyWindow(window);
        return 0;
    }
    switch (message) {
    case WM_ERASEBKGND:
        // The presenter owns the image and letterbox (including its GDI fill
        // fallback). Do not erase them during host WM_PAINT/resize.
        if (scaleRenderingEnabled) return 1;
        break;
    case WM_SYSCOMMAND:
        if (!nativeResizeEnabled && !scaleRenderingEnabled && ((wparam & 0xfff0) == SC_SIZE || (wparam & 0xfff0) == SC_MAXIMIZE)) {
            Log(L"Resize/maximize ignored while NativeResize=0");
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        const RECT r = OuterSize(scaleRenderingEnabled ? 320 : lakefix::kMinWidth,
                                 scaleRenderingEnabled ? 180 : lakefix::kMinHeight);
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
        if (!nativeResizeEnabled || !ready || !lakefix::HasPixels(wparam, lparam) || inResize) return 0;
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
        if (scaleRenderingEnabled && !ready && client) {
            // Native startup modals pump only a limited message range and can
            // consume WM_QUIT without terminating their loop. Do not tear down
            // graphics/windows underneath them; honor this request once ready.
            closeAfterStartup = true;
            Log(L"Host close deferred until native startup dialogs complete", true);
            return 0;
        }
        Log(L"Host WM_CLOSE: forwarding to original client", true);
        // The real main WndProc owns confirmations/cleanup. A frame close must
        // not destroy its child behind its back or ignore an application veto.
        if (client) SendMessageW(client, WM_CLOSE, wparam, lparam);
        else DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (GetCurrentThreadId() == uiThreadId.load() && nativeUiQuitPending.load()) {
            wchar_t text[160]{};
            swprintf_s(text, L"Host WM_DESTROY: preserving pending native UI quit code=%d", nativeUiQuitCode.load());
            Log(text, true);
        } else {
            Log(L"Host WM_DESTROY: no native UI quit pending; shim calls PostQuitMessage(0)", true);
            PostQuitMessage(0);
        }
        break;
    case WM_NCDESTROY:
        Log(L"Host WM_NCDESTROY", true);
        RemovePropW(window, L"LakeWindowedFix.HostSetupCompleted");
        RemoveWindowSubclass(window, HostProc, kSubclass);
        host = nullptr;
        ready = false;
        readyQueued = false;
        closeAfterStartup = false;
        sizing = pendingSize = false;
        break;
    }
    // WM_ACTIVATE/WM_ACTIVATEAPP are not forged or globally swallowed.
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK ClientProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                            UINT_PTR, DWORD_PTR) {
    if (scaleRenderingEnabled && deliveringClientWheel && (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)) {
        // A native child can propagate an unconsumed, already-scaled wheel
        // back to main. Continue default propagation, never re-enter its router.
        return IsWindowUnicode(window) ? DefWindowProcW(window, message, wparam, lparam)
                                       : DefWindowProcA(window, message, wparam, lparam);
    }
    struct WheelScope {
        bool previous = deliveringClientWheel;
        explicit WheelScope(bool active) { if (active) deliveringClientWheel = true; }
        ~WheelScope() { deliveringClientWheel = previous; }
    } wheelScope(scaleRenderingEnabled && (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL));
    if (scaleRenderingEnabled && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST &&
        message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL) {
        const POINT position{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        POINT mapped{};
        if (MapClientPoint(position, mapped)) {
            const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN ||
                message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK ||
                message == WM_MBUTTONDBLCLK || message == WM_XBUTTONDBLCLK;
            if (down && (mapped.x < 0 || mapped.y < 0)) return 0;
            lparam = MAKELPARAM(mapped.x, mapped.y);
        }
    }
    if (scaleRenderingEnabled && (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)) {
        POINT origin{};
        if (ClientToScreen(window, &origin)) {
            const POINT position{static_cast<short>(LOWORD(lparam)) - origin.x,
                                 static_cast<short>(HIWORD(lparam)) - origin.y};
            POINT mapped{};
            if (MapClientPoint(position, mapped)) lparam = MAKELPARAM(origin.x + mapped.x, origin.y + mapped.y);
        }
    }
    if (message == WM_CLOSE) Log(L"Client WM_CLOSE: original handler follows", true);
    if (message == WM_DESTROY) Log(L"Client WM_DESTROY: original handler follows", true);
    if (message == WM_DISPLAYCHANGE) {
        wchar_t text[192]{};
        swprintf_s(text, L"Client WM_DISPLAYCHANGE bits=%lu width=%u height=%u (forwarded unchanged)",
                   static_cast<unsigned long>(wparam), LOWORD(lparam), HIWORD(lparam));
        Log(text, true);
    }
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
        Log(L"Client WM_NCDESTROY", true);
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
    const DWORD current = GetCurrentThreadId();
    if (uiThreadId.exchange(current) != current) {
        nativeUiQuitPending.store(false);
        nativeUiQuitCode.store(0);
    }
    RECT r = OuterSize(width > 0 ? width : 1024, height > 0 ? height : 768);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    LONG viewWidth = initialViewSize.cx, viewHeight = initialViewSize.cy;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    const char* className = "ContourParent";
    HINSTANCE frameInstance = instance;
    if (scaleRenderingEnabled) {
        // A private host never enters ContourParent's destructive WM_SIZE,
        // including messages sent during CreateWindow before subclassing.
        WNDCLASSEXA existing{};
        existing.cbSize = sizeof(existing);
        if (!GetClassInfoExA(self, kScaledHostClass, &existing)) {
            cls.hInstance = self;
            cls.lpfnWndProc = DefWindowProcA;
            cls.lpszClassName = kScaledHostClass;
            cls.lpszMenuName = nullptr;
            cls.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            if (!RegisterClassExA(&cls)) return nullptr;
        } else if (existing.lpfnWndProc != DefWindowProcA) {
            SetLastError(ERROR_CLASS_ALREADY_EXISTS);
            return nullptr;
        }
        className = kScaledHostClass;
        frameInstance = self;
        if (!GetMonitorInfoW(MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY), &monitor)) return nullptr;
        const RECT borders = OuterSize(0, 0);
        const LONG bw = borders.right - borders.left, bh = borders.bottom - borders.top;
        const LONG workWidth = monitor.rcWork.right - monitor.rcWork.left;
        const LONG workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
        if (viewWidth + bw > workWidth) viewWidth = workWidth - bw;
        if (viewHeight + bh > workHeight) viewHeight = workHeight - bh;
        if (viewWidth <= 0 || viewHeight <= 0) return nullptr;
        r = OuterSize(viewWidth, viewHeight);
        x = monitor.rcWork.left + (workWidth - (r.right - r.left)) / 2;
        y = monitor.rcWork.top + (workHeight - (r.bottom - r.top)) / 2;
    }
    // Hidden until the native client asks to be shown. Visibility and graphics
    // rebuild readiness are deliberately separate (the 0.1 regression).
    host = CreateWindowExA(0, className, scaleRenderingEnabled ? "Lake Controller - Windowed (scaled)"
                               : nativeResizeEnabled ? "Lake Controller - Windowed" : "Lake Controller - Windowed (diagnostic)",
                            FrameStyle(0), x, y,
                            r.right - r.left, r.bottom - r.top,
                            nullptr, nullptr, frameInstance, nullptr);
    if (!host) return nullptr;
    Log(L"Standard host created (hidden until native show request)");
    if (!SetWindowSubclass(host, HostProc, kSubclass, 0)) {
        DestroyWindow(host);
        host = nullptr;
    }
    if (host && scaleRenderingEnabled) {
        // Correct nonclient/DPI rounding using actual window measurements,
        // while still hidden and before creating/initializing the renderer.
        RECT outer{}, area{};
        if (GetWindowRect(host, &outer) && GetClientRect(host, &area)) {
            LONG w = (outer.right - outer.left) + viewWidth - area.right;
            LONG h = (outer.bottom - outer.top) + viewHeight - area.bottom;
            const LONG ww = monitor.rcWork.right - monitor.rcWork.left;
            const LONG wh = monitor.rcWork.bottom - monitor.rcWork.top;
            if (w > ww) w = ww;
            if (h > wh) h = wh;
            SetWindowPos(host, HWND_NOTOPMOST, monitor.rcWork.left + (ww - w) / 2,
                         monitor.rcWork.top + (wh - h) / 2, w, h, SWP_NOACTIVATE);
        }
    }
    return host;
}

bool AttachClient(HWND window) {
    if (!window || !SetWindowSubclass(window, ClientProc, kSubclass, 0)) return false;
    RECT rectangle{};
    if (!GetClientRect(window, &rectangle)) return false;
    originalClientSize = {rectangle.right - rectangle.left, rectangle.bottom - rectangle.top};
    presentationSize.store(0);
    canvasLayoutSeeded = false;
    client = window;
    Log(scaleRenderingEnabled ? L"Lake client hosted on fixed virtual canvas; no native resize procedure used"
                              : L"Lake client hosted; native resize procedure retained");
    wchar_t size[128]{};
    swprintf_s(size, L"Original client size=%ldx%ld", originalClientSize.cx, originalClientSize.cy);
    Log(size);
    return true;
}

void QueueReadyAtMainLoop() {
    // A fallback, not the only way to reveal/initialize the host. Some startup
    // paths run a plugin-owned loop before the EXE reaches GetMessage.
    QueueReady(L"Ready queued by EXE GetMessage fallback");
}
bool CalledFromApplication(const void* caller) {
    const ULONG_PTR address = reinterpret_cast<ULONG_PTR>(caller);
    const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(application);
    return application && address >= base && address - base < applicationBytes;
}

void ObserveQuitRemoval(const MSG* message) {
    if (message && message->message == WM_QUIT && GetCurrentThreadId() == uiThreadId.load())
        nativeUiQuitPending.store(false);
}

BOOL WINAPI HookMessageA(LPMSG message, HWND window, UINT first, UINT last) {
    const void* caller = LAKE_CALLER_ADDRESS();
    const bool fromApplication = CalledFromApplication(caller);
    if (fromApplication) QueueReadyAtMainLoop();
    const BOOL result = GetMessageA(message, window, first, last);
    const DWORD error = GetLastError();
    if (result == 0) {
        ObserveQuitRemoval(message);
        LogSite(fromApplication ? L"EXE GetMessageA returned WM_QUIT" : L"Module GetMessageA returned WM_QUIT",
                message ? static_cast<DWORD>(message->wParam) : 0, caller, true);
    } else if (result == -1) LogSite(L"GetMessageA failed", error, caller, true);
    if (result > 0) MapMessagePoint(message);
    SetLastError(error);
    return result;
}
BOOL WINAPI HookMessageW(LPMSG message, HWND window, UINT first, UINT last) {
    const void* caller = LAKE_CALLER_ADDRESS();
    const bool fromApplication = CalledFromApplication(caller);
    if (fromApplication) QueueReadyAtMainLoop();
    const BOOL result = GetMessageW(message, window, first, last);
    const DWORD error = GetLastError();
    if (result == 0) {
        ObserveQuitRemoval(message);
        LogSite(fromApplication ? L"EXE GetMessageW returned WM_QUIT" : L"Module GetMessageW returned WM_QUIT",
                message ? static_cast<DWORD>(message->wParam) : 0, caller, true);
    } else if (result == -1) LogSite(L"GetMessageW failed", error, caller, true);
    if (result > 0) MapMessagePoint(message);
    SetLastError(error);
    return result;
}

BOOL WINAPI HookPeekA(LPMSG message, HWND window, UINT first, UINT last, UINT flags) {
    const BOOL result = PeekMessageA(message, window, first, last, flags);
    const DWORD error = GetLastError();
    if (result) MapMessagePoint(message);
    if (result && (flags & PM_REMOVE) && message && message->message == WM_QUIT) {
        ObserveQuitRemoval(message);
        LogSite(L"PeekMessageA removed WM_QUIT", static_cast<DWORD>(message->wParam), LAKE_CALLER_ADDRESS(), true);
    }
    SetLastError(error);
    return result;
}

BOOL WINAPI HookPeekW(LPMSG message, HWND window, UINT first, UINT last, UINT flags) {
    const BOOL result = PeekMessageW(message, window, first, last, flags);
    const DWORD error = GetLastError();
    if (result) MapMessagePoint(message);
    if (result && (flags & PM_REMOVE) && message && message->message == WM_QUIT) {
        ObserveQuitRemoval(message);
        LogSite(L"PeekMessageW removed WM_QUIT", static_cast<DWORD>(message->wParam), LAKE_CALLER_ADDRESS(), true);
    }
    SetLastError(error);
    return result;
}

HWND WINAPI HookCreateA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style,
                       int x, int y, int w, int h, HWND parent, HMENU menu,
                       HINSTANCE instance, LPVOID param) {
    if (parent || (style & WS_CHILD) || !IsName(cls, "Lake Controller"))
        return CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, instance, param);
    if (scaleRenderingEnabled) { w = renderSize.cx; h = renderSize.cy; }
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
    if (scaleRenderingEnabled) { w = renderSize.cx; h = renderSize.cy; }
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
            ? FrameStyle(static_cast<DWORD>(value))
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
        if (!nativeResizeEnabled) flags |= SWP_NOSIZE;
    } else if (window == host && after == HWND_TOPMOST) {
        after = HWND_NOTOPMOST;
    }
    if (window == host && !nativeResizeEnabled && !scaleRenderingEnabled) flags |= SWP_NOSIZE;
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
        w = nativeResizeEnabled ? r.right : originalClientSize.cx;
        h = nativeResizeEnabled ? r.bottom : originalClientSize.cy;
    } else if (window == host && !nativeResizeEnabled && !scaleRenderingEnabled) {
        RECT r{};
        if (GetWindowRect(window, &r)) { w = r.right - r.left; h = r.bottom - r.top; }
    }
    return MoveWindow(window, x, y, w, h, paint);
}

BOOL WINAPI HookShow(HWND window, int command) {
    if (window != client || !host) return ShowWindow(window, command);
    if (!nativeResizeEnabled && !scaleRenderingEnabled && command == SW_MAXIMIZE) command = SW_SHOW;
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

void WINAPI HookQuit(int code) {
    LogSite(L"Application PostQuitMessage", static_cast<DWORD>(code), LAKE_CALLER_ADDRESS(), true);
    PostQuitMessage(code);
    if (GetCurrentThreadId() == uiThreadId.load()) {
        nativeUiQuitCode.store(code);
        nativeUiQuitPending.store(true);
    }
}

[[noreturn]] void WINAPI HookExit(UINT code) {
    LogSite(L"Application ExitProcess", code, LAKE_CALLER_ADDRESS(), true);
    ExitProcess(code);
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
    if (module && module == GetModuleHandleW(L"Contour.dll")) { renderer = module; rendererBytes = ImageBytes(module); }
    if (module && module == GetModuleHandleW(L"Contour.dll") && (!ResolveDrawCreateEx() || !PatchModule(module))) {
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
    if (module && module == GetModuleHandleW(L"Contour.dll")) { renderer = module; rendererBytes = ImageBytes(module); }
    if (module && module == GetModuleHandleW(L"Contour.dll") && (!ResolveDrawCreateEx() || !PatchModule(module))) {
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
    HOOK("USER32.dll", "GetCursorPos", HookCursorPosition),
    HOOK("USER32.dll", "SetCursorPos", HookSetCursorPosition),
    HOOK("USER32.dll", "GetClientRect", HookClientRectangle),
    HOOK("USER32.dll", "PostQuitMessage", HookQuit),
    HOOK("USER32.dll", "ChangeDisplaySettingsA", HookDisplayA),
    HOOK("USER32.dll", "ChangeDisplaySettingsW", HookDisplayW),
    HOOK("USER32.dll", "ChangeDisplaySettingsExA", HookDisplayExA),
    HOOK("USER32.dll", "ChangeDisplaySettingsExW", HookDisplayExW),
    HOOK("KERNEL32.dll", "LoadLibraryA", HookLoadA),
    HOOK("KERNEL32.dll", "LoadLibraryW", HookLoadW),
    HOOK("KERNEL32.dll", "GetProcAddress", HookGetProc),
    HOOK("KERNEL32.dll", "ExitProcess", HookExit),
    HOOK("USER32.dll", "GetMessageA", HookMessageA),
    HOOK("USER32.dll", "GetMessageW", HookMessageW),
    HOOK("USER32.dll", "PeekMessageA", HookPeekA),
    HOOK("USER32.dll", "PeekMessageW", HookPeekW),
};
#undef HOOK

FARPROC WINAPI HookGetProc(HMODULE module, LPCSTR name) {
    FARPROC original = GetProcAddress(module, name);
    if (!original || reinterpret_cast<ULONG_PTR>(name) <= 0xffff) return original;
    if (scaleRenderingEnabled && strcmp(name, "DirectDrawCreateEx") == 0 && module == GetModuleHandleW(L"DDRAW.dll")) {
        originalDrawCreateEx.store(reinterpret_cast<DrawCreateEx>(reinterpret_cast<void*>(original)));
        return reinterpret_cast<FARPROC>(reinterpret_cast<void*>(HookDrawCreateEx));
    }
    for (const auto& hook : hooks) {
        // Compare both module identity and name; never replace another DLL's
        // coincidentally named export. Forwarders are resolved by GetProcAddress.
        if (!hook.applicationOnly && module == hook.provider && strcmp(name, hook.name) == 0)
            return reinterpret_cast<FARPROC>(hook.replacement);
    }
    return original;
}

bool ReplaceImport(PVOID volatile* slot, PVOID replacement, PVOID expected) {
    if (*slot == replacement) return true;
    if (!expected || *slot != expected) {
        Log(L"Refusing to overwrite a third-party IAT hook", true);
        return false;
    }
    DWORD protection{};
    if (!VirtualProtect(const_cast<PVOID*>(slot), sizeof(PVOID), PAGE_READWRITE, &protection)) return false;
    const PVOID previous = InterlockedCompareExchangePointer(slot, replacement, expected);
    DWORD ignored{};
    const bool restored = VirtualProtect(const_cast<PVOID*>(slot), sizeof(PVOID), protection, &ignored) != FALSE;
    return restored && (previous == expected || previous == replacement);
}

bool PatchModule(HMODULE module) {
    if (!module || module == self) return true;
    const PVOID drawCreate = reinterpret_cast<PVOID>(originalDrawCreateEx.load());
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
                    if (scaleRenderingEnabled && _stricmp(dll, "DDRAW.dll") == 0 &&
                        strcmp(imported->Name, "DirectDrawCreateEx") == 0) {
                        auto* slot = reinterpret_cast<PVOID volatile*>(&slots[i].u1.Function);
                        if (!ReplaceImport(slot, reinterpret_cast<PVOID>(HookDrawCreateEx), drawCreate)) success = false;
                    }
                    for (const auto& hook : hooks) {
                        if (hook.applicationOnly && module != application) continue;
                        if (_stricmp(dll, hook.dll) || strcmp(imported->Name, hook.name)) continue;
                        auto* slot = reinterpret_cast<PVOID volatile*>(&slots[i].u1.Function);
                        if (!ReplaceImport(slot, hook.replacement, hook.original)) success = false;
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
    LoadResizePolicy();
    lakefix::ConfigureScalePresenter(ScaledViewport, PresenterLog, ClipWindow, PaintLetterbox);
    application = GetModuleHandleW(nullptr);
    applicationBytes = ImageBytes(application);
    renderer = GetModuleHandleW(L"Contour.dll");
    rendererBytes = ImageBytes(renderer);
    if (!AddVectoredExceptionHandler(1, ObserveException)) Log(L"Exception observer unavailable; exit status still available with --wait");
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
    if (!ResolveDrawCreateEx() || !PatchModule(application) || !PatchModule(GetModuleHandleW(L"Contour.dll")))
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
