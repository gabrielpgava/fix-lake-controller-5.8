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
#include "ui_quality.h"
#include "profile.h"
#include "session_config.h"
#include "lake_build_profile.h"
#include "component_contract.h"
#include "session_diagnostics.h"
#include "layout_policy.h"
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
UINT startupFailedMessage;
bool sizing;
bool pendingSize;
bool inResize;
bool ready;
bool readyQueued;
bool nativeResizeEnabled;
bool scaleRenderingEnabled = true;
SIZE renderSize{1088, 612};
SIZE initialViewSize{1024, 576};
SIZE minimumViewSize{lakefix::kScaledMinWidth, lakefix::kScaledMinHeight};
bool canvasLayoutSeeded;
bool closeAfterStartup;
DWORD fatalStartupCode;
bool fatalStartupNotified;
bool smoothFontsEnabled = true;
bool systemCursorEnabled = true;
lakefix::SessionConfig sessionConfig;
HCURSOR systemArrow;
bool smoothFontLogged;
bool systemCursorLogged;
std::atomic<DWORD> presentationSize{0};
WPARAM pendingType;
LPARAM pendingDimensions;
LONG initialization;
SRWLOCK importsLock = SRWLOCK_INIT;
HANDLE logFile = INVALID_HANDLE_VALUE;
LONG observingException;
ULONG_PTR applicationBytes;
// One coherent x86 (base,size) publication. Once admitted, Contour is pinned;
// its base cannot be reused and no unsynchronized pair of globals is read.
alignas(8) std::atomic<std::uint64_t> rendererImage{0};
struct RendererImage { HMODULE module; ULONG_PTR bytes; };
RendererImage VerifiedRenderer() noexcept {
    const auto packed = rendererImage.load(std::memory_order_acquire);
    return {reinterpret_cast<HMODULE>(static_cast<ULONG_PTR>(static_cast<DWORD>(packed))),
            static_cast<ULONG_PTR>(packed >> 32)};
}
HANDLE admittedContour = INVALID_HANDLE_VALUE; // Retained to process exit; never hot-unhook/unload.
BY_HANDLE_FILE_INFORMATION admittedContourIdentity{};
wchar_t admittedContourPath[32768]{};
std::atomic<DWORD> startupStages{0};
std::atomic<DWORD> startupFailure{0};
std::atomic<bool> configurationLoaded{false};
std::atomic<unsigned long> cursorPairCount{0}, cursorTruncatedDeltaCount{0}, cursorZeroDeltaCount{0};
std::atomic<bool> layoutAdmissionFailureLogged{false};
enum StartupStage : DWORD { CompatibleDll = 1, HooksInstalled = 2, HostCanvasPrepared = 4,
    LayoutAdmitted = 8, GraphicsProxyAvailable = 16, FirstPresentation = 32,
    ResponsivenessObserved = 64, ShutdownObserved = 128 };
std::atomic<DWORD> uiThreadId{0};
std::atomic<bool> nativeUiQuitPending{false};
std::atomic<int> nativeUiQuitCode{0};
constexpr UINT_PTR kSubclass = 0x4c574658;
constexpr UINT_PTR kChildCursorSubclass = 0x4c574643;
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
    // One writer is required by the bounded consumer. Readers may review logs;
    // a second writer cannot invalidate its byte accounting during the session.
    logFile = CreateFileW(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE,
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
    [[maybe_unused]] lakefix::profile::Scope timing(lakefix::profile::Metric::LogTotal);
    (void)nonBlocking; // All admission is now bounded, best-effort, nonblocking.
    lakefix::diagnostics::Write(text);
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
    const auto renderer = VerifiedRenderer();
    const ULONG_PTR rendererBase = reinterpret_cast<ULONG_PTR>(renderer.module);
    const wchar_t* name = L"outside-cached-target-images";
    ULONG_PTR rva = 0;
    if (application && value >= appBase && value - appBase < applicationBytes) {
        name = L"main-executable";
        rva = value - appBase;
    } else if (renderer.module && value >= rendererBase && value - rendererBase < renderer.bytes) {
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
        lakefix::diagnostics::RecordException(code, info->ExceptionRecord->ExceptionAddress);
        InterlockedExchange(&observingException, 0);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool LoadResizePolicy() {
    wchar_t ini[1024]{};
    const DWORD length = GetModuleFileNameW(self, ini, ARRAYSIZE(ini));
    wchar_t* slash = wcsrchr(ini, L'\\');
    if (!length || length >= ARRAYSIZE(ini) || !slash ||
        wcscpy_s(slash + 1, ARRAYSIZE(ini) - (slash + 1 - ini), L"LakeWindowedFix.ini") != 0) return false;
    const HANDLE held = CreateFileW(ini, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (held == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) return false;
    if (held != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION identity{};
        if (!GetFileInformationByHandle(held, &identity) || identity.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY) ||
            identity.nFileSizeHigh || identity.nFileSizeLow > LAKE_FIX_MAX_INI_BYTES) { CloseHandle(held); return false; }
        sessionConfig.adjacentIni = true;
    }
    const bool loaded = lakefix::ReadSessionConfig([&](const wchar_t* section, const wchar_t* name, wchar_t* text, DWORD capacity) noexcept {
        if (held == INVALID_HANDLE_VALUE) return lakefix::kConfigMissing;
        const DWORD count = GetPrivateProfileStringW(section, name, L"\x01", text, capacity, ini);
        return count == 1 && text[0] == L'\x01' ? lakefix::kConfigMissing : count;
    }, sessionConfig);
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    if (!loaded) return false;
    using lakefix::ConfigKey;
    nativeResizeEnabled = sessionConfig.Get(ConfigKey::NativeResize) != 0;
    scaleRenderingEnabled = sessionConfig.Get(ConfigKey::ScaleRendering) != 0;
    initialViewSize = {sessionConfig.Get(ConfigKey::WindowWidth), sessionConfig.Get(ConfigKey::WindowHeight)};
    minimumViewSize = {sessionConfig.Get(ConfigKey::MinWidth), sessionConfig.Get(ConfigKey::MinHeight)};
    renderSize = {sessionConfig.Get(ConfigKey::CanvasWidth), sessionConfig.Get(ConfigKey::CanvasHeight)};
    smoothFontsEnabled = sessionConfig.Get(ConfigKey::SmoothFonts) != 0;
    systemCursorEnabled = sessionConfig.Get(ConfigKey::SystemCursor) != 0;
    if (scaleRenderingEnabled && systemCursorEnabled) {
        systemArrow = LoadCursorW(nullptr, IDC_ARROW);
        if (!systemArrow) {
            auto& value = sessionConfig.values[static_cast<unsigned>(ConfigKey::SystemCursor)];
            value.applied = 0;
            value.resolution = lakefix::ConfigResolution::ResourceUnavailable;
            systemCursorEnabled = false;
            if (!lakefix::HashEffectiveConfig(sessionConfig)) return false;
            Log(L"SystemCursor unavailable: shared Windows arrow could not be loaded; legacy cursor retained", true);
        }
    }
    configurationLoaded.store(true);
    for (unsigned i = 0; i < static_cast<unsigned>(ConfigKey::Count); ++i) {
        const auto& definition = lakefix::configDefinitions[i];
        const auto& value = sessionConfig.values[i];
        const wchar_t* origin = value.origin == lakefix::ConfigOrigin::Requested ? L"requested-valid" :
            value.origin == lakefix::ConfigOrigin::InvalidDefault ? L"invalid-replaced-by-default" : L"missing-default";
        const wchar_t* resolution = value.resolution == lakefix::ConfigResolution::CompatibilityMode ? L"overridden-by-compatibility-mode" :
            value.resolution == lakefix::ConfigResolution::ResourceUnavailable ? L"resource-unavailable-fallback" : L"unchanged";
        wchar_t line[256]{};
        if (value.origin == lakefix::ConfigOrigin::Requested)
            swprintf_s(line, L"Config [%ls] %ls requested-numeric=%ld effective=%ld status=%ls resolution=%ls", definition.section, definition.name,
                       value.requested, value.applied, origin, resolution);
        else
            swprintf_s(line, L"Config [%ls] %ls requested=%ls effective=%ld status=%ls resolution=%ls", definition.section, definition.name,
                       value.origin == lakefix::ConfigOrigin::InvalidDefault ? L"invalid-not-logged" : L"absent", value.applied, origin, resolution);
        Log(line, true);
    }
    wchar_t fingerprint[192]{};
    swprintf_s(fingerprint, L"Config immutable schema=%d source=%ls effective-sha256=%hs; changes apply next launch", LAKE_FIX_CONFIG_SCHEMA,
               sessionConfig.adjacentIni ? L"adjacent-ini" : L"defaults", sessionConfig.effectiveSha256);
    Log(fingerprint, true);
    if (scaleRenderingEnabled) {
        Log(L"ScaleRendering=1: resizable scaled presentation; fixed virtual canvas; no native graphics rebuild");
        return true;
    }
    Log(nativeResizeEnabled ? L"NativeResize=1: EXPERIMENTAL graphics teardown/rebuild enabled"
                             : L"NativeResize=0: original framebuffer size retained; no native graphics rebuild");
    return true;
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

// Stack-only input geometry. Reuse it within one API call, never across queue
// peeks, dispatches or events. ClientProc's ordinary mapping needs no origin.
struct InputGeometry {
    SIZE source{};
    RECT area{};
    RECT viewport{};
    POINT origin{};
    bool hasOrigin = false;
};

bool CaptureInputGeometry(InputGeometry& geometry, bool screenCoordinates = false) {
    geometry.source = CurrentSourceSize();
    if (!LocalViewport(geometry.source, geometry.area, geometry.viewport)) return false;
    geometry.hasOrigin = screenCoordinates && ClientToScreen(client, &geometry.origin);
    return true;
}

bool ScaledViewport(HWND window, LONG width, LONG height, RECT* full, RECT* fitted) noexcept {
    if (window != client || width != renderSize.cx || height != renderSize.cy) return false;
#if !defined(LAKE_FIX_TESTING)
    if (!canvasLayoutSeeded) return false;
#endif
    const DWORD saved = GetLastError();
    RECT area{}, view{};
    POINT origin{};
    const bool valid = LocalViewport({width, height}, area, view) && ClientToScreen(host, &origin);
    if (valid) {
        OffsetRect(&area, origin.x, origin.y);
        OffsetRect(&view, origin.x, origin.y);
        *full = area;
        *fitted = view;
    }
    SetLastError(saved);
    return valid;
}

HRESULT AdmitScaledPresentation(HWND window) noexcept {
#if defined(LAKE_FIX_TESTING)
    // Synthetic window fixtures do not execute proprietary RVAs. The production
    // policy below is covered independently as logic and in the COM admission test.
    return lakefix::ScaledLayoutAdmission(scaleRenderingEnabled && window == client, true);
#else
    const HRESULT admission = lakefix::ScaledLayoutAdmission(scaleRenderingEnabled && window == client, canvasLayoutSeeded);
    if (FAILED(admission)) {
        startupFailure.store(static_cast<DWORD>(admission));
        if (!layoutAdmissionFailureLogged.exchange(true)) {
            Log(L"Startup failed stage=layout-admission reason=eligible-frame-before-admitted-layout action=close-normally-and-review-matching-Build-007; frame rejected, NEVER native scaled fallback", true);
            // Request native cleanup on its UI thread, after startup modals.
            // No arbitrary kill, synchronous UI call or I/O on the renderer.
            if (client && startupFailedMessage)
                PostMessageW(client, startupFailedMessage, static_cast<WPARAM>(static_cast<DWORD>(admission)), 0);
        }
    }
    return admission;
#endif
}

void CommitPresentation(HWND window, LONG width, LONG height, const RECT*, const RECT*) noexcept {
    if (window != client || width != renderSize.cx || height != renderSize.cy) return;
    presentationSize.store(MAKELONG(width, height));
    if (!(startupStages.fetch_or(FirstPresentation) & FirstPresentation))
        Log(L"Startup stage=first-presentation HRESULT=success; not prolonged stability, audio health or proof of pixels", true);
}

void PresenterLog(const wchar_t* message) noexcept { Log(message, true); }

bool DiagnosticStats(void*, lakefix::diagnostics::PresenterStats& output) noexcept {
    const auto stats = lakefix::GetScalePresenterStats();
    output = {stats.directDrawObjects, stats.primarySurfaces, stats.liveProxyObjects, stats.ownerContexts};
    wchar_t stages[320]{};
    swprintf_s(stages, L"Session stages=0x%08lx last-observed-failure=0x%08lx; no-presentation intervals/modals are not crash evidence; cursor-pairs=%lu zero=%lu truncated=%lu accumulation=disabled",
               startupStages.load(), startupFailure.load(), cursorPairCount.load(), cursorZeroDeltaCount.load(), cursorTruncatedDeltaCount.load());
    Log(stages, true);
    return true; // These proxy counts do not measure all native/GPU memory.
}

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
    bool hasPixels = false;
    for (UINT i = 0; i < count; ++i) {
        local[i] = strips[i];
        OffsetRect(&local[i], -origin.x, -origin.y);
        if (local[i].left < 0 || local[i].top < 0 || local[i].right > area.right || local[i].bottom > area.bottom) {
            SetLastError(saved); return false;
        }
        if (local[i].right > local[i].left && local[i].bottom > local[i].top) hasPixels = true;
    }
    if (!hasPixels) { SetLastError(saved); return true; }
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

bool CanvasOwnsCapture() {
    const HWND capture = GetCapture();
    return client && capture && (capture == client || IsChild(client, capture));
}

POINT TransformInputPoint(const InputGeometry& geometry, POINT point, bool captured) {
    return captured ? lakefix::ViewToSourceCaptured(geometry.viewport, geometry.source, point)
                    : lakefix::ViewToSource(geometry.viewport, geometry.source, point);
}

bool MapClientPoint(POINT real, POINT& mapped, bool allowCapture = true) {
    InputGeometry geometry;
    if (!CaptureInputGeometry(geometry)) return false;
    mapped = TransformInputPoint(geometry, real, allowCapture && CanvasOwnsCapture());
    return true;
}

bool MapScreenPoint(const InputGeometry& geometry, POINT real, POINT& mapped) {
    if (!geometry.hasOrigin) return false;
    const POINT source = TransformInputPoint(geometry,
        {real.x - geometry.origin.x, real.y - geometry.origin.y}, CanvasOwnsCapture());
    mapped = {geometry.origin.x + source.x, geometry.origin.y + source.y};
    return true;
}

bool RendererCallsite(const void* caller, ULONG_PTR rva) {
#if defined(LAKE_FIX_TESTING)
    (void)caller; (void)rva;
    return false; // Synthetic modules can NEVER admit proprietary Build 007 RVAs.
#else
    const auto renderer = VerifiedRenderer();
    return renderer.module && rva < renderer.bytes &&
        reinterpret_cast<ULONG_PTR>(caller) == reinterpret_cast<ULONG_PTR>(renderer.module) + rva;
#endif
}

bool RendererAddress(const void* caller) {
    const ULONG_PTR address = reinterpret_cast<ULONG_PTR>(caller);
    const auto renderer = VerifiedRenderer();
    const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(renderer.module);
    return renderer.module && address >= base && address - base < renderer.bytes;
}

bool UseSystemCursor() { return scaleRenderingEnabled && systemCursorEnabled && systemArrow; }

// ShowCursor uses a thread/input-queue counter, not a visibility Boolean. Lake
// starts with a negative count. Own only bounded compensating increments, then
// return those increments on exit/focus loss; never normalize an unrelated app.
thread_local unsigned cursorVisibilityCredits;
thread_local lakefix::VirtualCursorVisibility virtualCursorVisibility;

int WINAPI HookShowCursor(BOOL show) {
    const void* caller = LAKE_CALLER_ADDRESS();
    const DWORD saved = GetLastError();
    const ULONG_PTR rva = RendererAddress(caller)
        ? reinterpret_cast<ULONG_PTR>(caller) - reinterpret_cast<ULONG_PTR>(VerifiedRenderer().module) : 0;
    if (!lakefix::ShouldVirtualizeCursorVisibility(UseSystemCursor(), client != nullptr,
                                                  GetCurrentThreadId() == uiThreadId.load(), rva)) {
        SetLastError(saved);
        return ShowCursor(show); // Preserve the real API's result and LastError.
    }
    // Intentional virtualization of four legacy cursor-hide return sites, not
    // an HRESULT or a claim about OS visibility. Every other call stays native.
    // Our own DLL's IAT is never patched, so the one-time balanced probe cannot
    // recurse into this hook; it also leaves our compensating credits intact.
    const int result = virtualCursorVisibility.Adjust(show, cursorVisibilityCredits,
        [](BOOL visible) noexcept { return ShowCursor(visible); });
    SetLastError(saved);
    return result;
}

void RestoreCursorVisibility() {
    lakefix::ReleaseCursorVisibility(cursorVisibilityCredits, [](BOOL show) noexcept { return ShowCursor(show); });
}

lakefix::CursorVisibilitySample EnsureCursorVisible() {
    return lakefix::EnsureCursorVisibility(cursorVisibilityCredits,
        [](PCURSORINFO info) noexcept { return GetCursorInfo(info); },
        [](BOOL show) noexcept { return ShowCursor(show); });
}

LRESULT CALLBACK ChildCursorProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR, DWORD_PTR) {
    if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT && UseSystemCursor() &&
        client && IsChild(client, window)) {
        // Some native controls consume this message and hide the OS cursor
        // before it can reach main. Leave all input and coordinates native.
        SetCursor(systemArrow);
        EnsureCursorVisible();
        return TRUE;
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, ChildCursorProc, kChildCursorSubclass);
    return DefSubclassProc(window, message, wparam, lparam);
}

void AttachChildCursor(HWND window) {
    if (!UseSystemCursor() || !client || !window || !IsChild(client, window)) return;
    DWORD process{};
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (thread != GetCurrentThreadId() || thread != uiThreadId.load() || process != GetCurrentProcessId()) {
        Log(L"SystemCursor: child outside owning UI thread/process; native cursor/bitmap fallback retained", true);
        return;
    }
    if (!SetWindowSubclass(window, ChildCursorProc, kChildCursorSubclass, 0))
        Log(L"SystemCursor: child cursor subclass unavailable; native cursor/bitmap fallback retained", true);
}

BOOL CALLBACK AttachExistingChildCursor(HWND window, LPARAM) {
    AttachChildCursor(window);
    return TRUE;
}

struct CursorHostSample {
    bool overHost = false;
    bool insideClient = false;
};

CursorHostSample SampleCursorHost() {
    CursorHostSample sample;
    POINT point{};
    if (!host || IsIconic(host) || !GetCursorPos(&point)) return sample;
    const HWND under = WindowFromPoint(point);
    sample.overHost = under && GetAncestor(under, GA_ROOT) == host;
    if (client) {
        if (under == client || IsChild(client, under)) sample.insideClient = true;
        else if (under == host) {
            RECT area{};
            sample.insideClient = ScreenToClient(host, &point) && GetClientRect(host, &area) && PtInRect(&area, point);
        }
    }
    return sample;
}

int WINAPI HookMapWindowPoints(HWND from, HWND to, LPPOINT points, UINT count) {
    const void* caller = LAKE_CALLER_ADDRESS();
    const int result = MapWindowPoints(from, to, points, count);
    const DWORD saved = GetLastError();
    if (UseSystemCursor() && from == nullptr && to == client && count == 2 && points &&
         RendererAddress(caller) && lakefix::IsCursorPaintReturn(
              reinterpret_cast<ULONG_PTR>(caller) - reinterpret_cast<ULONG_PTR>(VerifiedRenderer().module))) {
        [[maybe_unused]] lakefix::profile::Scope timing(lakefix::profile::Metric::CursorPaint);
        // Outside this host, leave the other application's cursor alone and
        // suppress Lake's stale bitmap. Inside, suppress only after confirming
        // a visible OS cursor. Hidden-count/modal cases retain the legacy sprite.
        const CursorHostSample sample = SampleCursorHost();
        if (!sample.overHost) RestoreCursorVisibility();
        bool suppress = !sample.insideClient;
        if (!suppress) {
            SetCursor(systemArrow);
            const auto visibility = EnsureCursorVisible();
            // Reuse only this call's already-visible arrow observation. Any
            // recovery, failure or unexpected cursor still needs a fresh query.
            suppress = lakefix::ConfirmSystemCursor(visibility, systemArrow,
                [](PCURSORINFO info) noexcept { return GetCursorInfo(info); });
        }
        if (suppress && lakefix::HideSoftwareCursorRectangle(points, count) && !systemCursorLogged) {
            systemCursorLogged = true;
            Log(L"SystemCursor=1: OS arrow independent of presentation; verified legacy cursor paint rectangles clipped", true);
        }
    }
    SetLastError(saved);
    return result;
}

HFONT WINAPI HookCreateFontIndirectA(const LOGFONTA* input) {
    const void* caller = LAKE_CALLER_ADDRESS();
    LOGFONTA smooth{};
    const bool changed = input && scaleRenderingEnabled && smoothFontsEnabled && RendererAddress(caller) &&
        lakefix::PrepareSmoothFont(*input, smooth);
    const HFONT result = CreateFontIndirectA(changed ? &smooth : input);
    const DWORD saved = GetLastError();
    if (result && changed && !smoothFontLogged) {
        smoothFontLogged = true;
        Log(L"SmoothFonts=1: grayscale antialiasing for native Arial/Courier New; requested size/face retained", true);
    }
    SetLastError(saved);
    return result;
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
    [[maybe_unused]] lakefix::profile::Scope timing(lakefix::profile::Metric::CursorPosition);
    const void* caller = LAKE_CALLER_ADDRESS();
    cursorRoundTrip.armed = false;
    const BOOL result = GetCursorPos(point);
    const DWORD saved = GetLastError();
    if (result && point && scaleRenderingEnabled && client && host) {
        const POINT real = *point;
        const HWND under = WindowFromPoint(*point);
        const HWND capture = GetCapture();
        const bool clientHitOrCapture = under == client || IsChild(client, under) ||
            capture == client || IsChild(client, capture);
        if (clientHitOrCapture || under == host) {
            InputGeometry geometry;
            if (CaptureInputGeometry(geometry, true) && geometry.hasOrigin) {
                POINT hostOrigin{};
                const bool inHostClient = under == host && ClientToScreen(host, &hostOrigin) &&
                    real.x >= hostOrigin.x && real.y >= hostOrigin.y &&
                    real.x < hostOrigin.x + geometry.area.right && real.y < hostOrigin.y + geometry.area.bottom;
                if (clientHitOrCapture || inHostClient) {
                    MapScreenPoint(geometry, real, *point);
                    // Verified build-007 getter -> offset -> setter pair. Retain
                    // exactly the geometry that produced this virtual point.
                    // Other setters keep physical/screen semantics.
                    if (RendererCallsite(caller, lakefix::build007::cursorGetterReturn))
                        cursorRoundTrip = {true, client, real, *point, geometry.viewport, geometry.source};
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
        RendererCallsite(caller, lakefix::build007::cursorSetterReturn)) {
        const SIZE view{previous.viewport.right - previous.viewport.left, previous.viewport.bottom - previous.viewport.top};
        const POINT real = lakefix::TranslateCursorDelta(previous.real, previous.virtualPoint, {x, y}, view, previous.source);
        if (sessionConfig.Get(lakefix::ConfigKey::DetailedInput)) {
            cursorPairCount.fetch_add(1, std::memory_order_relaxed);
            if (x == previous.virtualPoint.x && y == previous.virtualPoint.y)
                cursorZeroDeltaCount.fetch_add(1, std::memory_order_relaxed);
            else if (real.x == previous.real.x && real.y == previous.real.y)
                cursorTruncatedDeltaCount.fetch_add(1, std::memory_order_relaxed);
        }
        x = real.x;
        y = real.y;
    }
    return SetCursorPos(x, y);
}

void MapMessagePoint(MSG* message) {
    if (!message || !scaleRenderingEnabled || !client ||
        message->message < WM_MOUSEFIRST || message->message > WM_MOUSELAST ||
         (message->hwnd != client && message->hwnd != host && !IsChild(client, message->hwnd))) return;
    [[maybe_unused]] lakefix::profile::Scope timing(lakefix::profile::Metric::QueueMapping);
    InputGeometry geometry;
    const bool haveGeometry = CaptureInputGeometry(geometry, true);
    const bool wheel = message->message == WM_MOUSEWHEEL || message->message == WM_MOUSEHWHEEL;
    if (wheel) message->hwnd = client; // Original main wheel router scales it once.
    else {
        POINT raw{static_cast<short>(LOWORD(message->lParam)), static_cast<short>(HIWORD(message->lParam))};
        MapWindowPoints(message->hwnd, client, &raw, 1);
        if (!haveGeometry) return;
        HWND target = GetCapture();
        const bool captured = target == client || IsChild(client, target);
        POINT source = TransformInputPoint(geometry, raw, captured);
        const bool down = message->message == WM_LBUTTONDOWN || message->message == WM_RBUTTONDOWN ||
            message->message == WM_MBUTTONDOWN || message->message == WM_XBUTTONDOWN ||
            message->message == WM_LBUTTONDBLCLK || message->message == WM_RBUTTONDBLCLK ||
            message->message == WM_MBUTTONDBLCLK || message->message == WM_XBUTTONDBLCLK;
        if (down && !lakefix::ViewportContains(geometry.viewport, raw)) {
            // A child's capture belongs to the existing drag, not to a NEW
            // margin click. Route through main's fresh hit-test rejection;
            // subsequent moves/releases still go to the original native capture.
            target = client;
        }
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
    // above, since native child cursor subclasses never rewrite mouse input.
    if (haveGeometry && MapScreenPoint(geometry, message->pt, mapped)) message->pt = mapped;
}

BOOL WINAPI HookClientRectangle(HWND window, LPRECT rectangle) {
    const void* caller = LAKE_CALLER_ADDRESS();
    const BOOL result = GetClientRect(window, rectangle);
    const DWORD error = GetLastError();
    if (window == client && scaleRenderingEnabled && !canvasLayoutSeeded &&
        RendererCallsite(caller, lakefix::build007::layoutReturn)) {
        // Build 007 uses display dimensions both to choose an adapter mode and
        // to seed its layout. Leave capability/mode enumeration truthful, then
        // seed ONLY the two writable layout dimensions after mode selection,
        // at the original windowed-surface GetClientRect query. No code patch,
        // GPU reset, display-mode spoof, or periodic resize writes are involved.
        constexpr ULONG_PTR dimensionsRva = lakefix::build007::layoutDimensions;
        const auto renderer = VerifiedRenderer();
        auto* dimensions = reinterpret_cast<LONG*>(reinterpret_cast<ULONG_PTR>(renderer.module) + dimensionsRva);
        MEMORY_BASIC_INFORMATION memory{};
        const bool valid = result && rectangle && rectangle->right == renderSize.cx && rectangle->bottom == renderSize.cy &&
            VirtualQuery(dimensions, &memory, sizeof(memory)) == sizeof(memory) &&
            lakefix::LayoutMemoryAdmitted(memory, reinterpret_cast<ULONG_PTR>(renderer.module), renderer.bytes, dimensionsRva) &&
            lakefix::LayoutValuesAdmitted(dimensions[0], dimensions[1]);
        if (valid) {
            dimensions[0] = renderSize.cx;
            dimensions[1] = renderSize.cy;
            canvasLayoutSeeded = true;
            startupStages.fetch_or(LayoutAdmitted);
            Log(L"Virtual canvas layout seeded after native adapter/mode selection", true);
        } else {
            startupFailure.store(ERROR_INVALID_DATA);
            Log(L"Startup failed stage=layout reason=writable-build-007-dimension-guard action=close-and-review-compatible-build; no native scaled fallback", true);
            // A refused layout may not proceed with a native image and scaled
            // input. This is an explicit startup admission failure, not a watchdog
            // reacting to a modal, a long frame, memory pressure or audio state.
            // This includes admission of a newly recreated canvas: never let a
            // failed layout return to a native image with scaled coordinates.
            lakefix::diagnostics::Shutdown(250);
            ExitProcess(ERROR_INVALID_DATA);
        }
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
    HRESULT result = original(guid, object, iid, outer);
    const DWORD saved = GetLastError();
    static constexpr GUID draw7{0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0, 0x60, 0x97, 0x97, 0xea, 0x5b}};
    if (memcmp(&iid, &draw7, sizeof(iid)) == 0) {
        result = lakefix::CompleteScaledDirectDrawCreation(result, object);
        if (SUCCEEDED(result)) startupStages.fetch_or(GraphicsProxyAvailable);
        else {
            startupFailure.store(static_cast<DWORD>(result));
            Log(L"Startup failed stage=DirectDraw-proxy reason=native-or-proxy-creation-failed action=review-HRESULT-and-matching-package; no fail-open object", true);
        }
    }
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
    const LONG width = initialViewSize.cx, height = initialViewSize.cy;
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
    if (message == startupFailedMessage) {
        fatalStartupCode = static_cast<DWORD>(wparam);
        SetPropW(window, L"LakeWindowedFix.StartupFailure", reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(fatalStartupCode)));
        closeAfterStartup = true;
        if (ready) { closeAfterStartup = false; PostMessageW(window, WM_CLOSE, 0, 0); }
        return 0;
    }
    if (UseSystemCursor() && message == WM_SETCURSOR) {
        EnsureCursorVisible();
        if (LOWORD(lparam) == HTCLIENT) {
            SetCursor(systemArrow);
            return TRUE;
        }
        // Nonclient sizing/moving cursors remain DefWindowProc's responsibility.
    }
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
    case WM_ACTIVATEAPP:
        // The focused HWND may be a native descendant rather than main. The
        // top-level deactivation covers that case without swallowing activation.
        if (!wparam) RestoreCursorVisibility();
        break;
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
        // Interactive tracking only; programmatic host resizing stays native.
        const RECT r = OuterSize(scaleRenderingEnabled ? minimumViewSize.cx : lakefix::kMinWidth,
                                 scaleRenderingEnabled ? minimumViewSize.cy : lakefix::kMinHeight);
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        SIZE minimum{r.right - r.left, r.bottom - r.top};
        if (scaleRenderingEnabled) {
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
                minimum = lakefix::ClampToWorkArea(minimum,
                    {monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top});
        }
        limits->ptMinTrackSize = {minimum.cx, minimum.cy};
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
        if (fatalStartupCode && !fatalStartupNotified) {
            fatalStartupNotified = true;
#if !defined(LAKE_FIX_TESTING)
            MessageBoxW(window,
                L"A inicializacao em janela foi recusada.\r\n\r\nEtapa: admissao do layout. Motivo: apresentacao antes de validar o canvas Build 007.\r\n\r\nFeche normalmente e confira o pacote e o diagnostico. Nenhuma imagem nativa com coordenadas escaladas sera apresentada. A confirmacao de fechamento do Lake continua sendo respeitada.",
                L"Lake Windowed Fix - Falha de inicializacao", MB_OK | MB_ICONERROR);
#endif
            Log(L"Controlled startup failure: UI requests original WM_CLOSE after modals; native confirmation/veto/cleanup retained", true);
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
        RestoreCursorVisibility();
        Log(L"Host WM_NCDESTROY", true);
        RemovePropW(window, L"LakeWindowedFix.HostSetupCompleted");
        RemovePropW(window, L"LakeWindowedFix.StartupFailure");
        RemoveWindowSubclass(window, HostProc, kSubclass);
        host = nullptr;
        ready = false;
        readyQueued = false;
        closeAfterStartup = false;
        fatalStartupCode = 0;
        fatalStartupNotified = false;
        sizing = pendingSize = false;
        break;
    }
    // WM_ACTIVATE/WM_ACTIVATEAPP are not forged or globally swallowed.
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK ClientProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                            UINT_PTR, DWORD_PTR) {
    if (UseSystemCursor() && message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {
        EnsureCursorVisible();
        SetCursor(systemArrow);
        return TRUE;
    }
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
        const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN ||
                message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK ||
                message == WM_MBUTTONDBLCLK || message == WM_XBUTTONDBLCLK;
        if (MapClientPoint(position, mapped, !down)) {
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
    if (message == readyMessage || message == startupFailedMessage) {
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
        RestoreCursorVisibility();
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
        RestoreCursorVisibility();
        Log(L"Client WM_NCDESTROY", true);
        RemoveWindowSubclass(window, ClientProc, kSubclass);
        client = nullptr;
        startupStages.fetch_or(ShutdownObserved);
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
        const SIZE initial = lakefix::AtLeastMinimum(initialViewSize, minimumViewSize);
        viewWidth = initial.cx;
        viewHeight = initial.cy;
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
    layoutAdmissionFailureLogged.store(false);
    client = window;
    startupStages.fetch_or(HostCanvasPrepared);
    // WM_CREATE can build the native controls before client is assigned. The
    // enumeration includes nested descendants, without touching owned dialogs.
    if (UseSystemCursor()) EnumChildWindows(window, AttachExistingChildCursor, 0);
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
    if (result > 0 && GetCurrentThreadId() == uiThreadId.load()) startupStages.fetch_or(ResponsivenessObserved);
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
    if (result > 0 && GetCurrentThreadId() == uiThreadId.load()) startupStages.fetch_or(ResponsivenessObserved);
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
    if (parent || (style & WS_CHILD) || !IsName(cls, "Lake Controller")) {
        const HWND window = CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, instance, param);
        const DWORD error = GetLastError();
        AttachChildCursor(window);
        SetLastError(error);
        return window;
    }
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
    if (parent || (style & WS_CHILD) || !IsName(cls, L"Lake Controller")) {
        const HWND window = CreateWindowExW(ex, cls, title, style, x, y, w, h, parent, menu, instance, param);
        const DWORD error = GetLastError();
        AttachChildCursor(window);
        SetLastError(error);
        return window;
    }
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
        if constexpr (lakefix::profile::kEnabled) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true, std::memory_order_relaxed))
                lakefix::profile::Report(PresenterLog, L"native-ui-quit-request-not-exit");
        }
    }
}

[[noreturn]] void WINAPI HookExit(UINT code) {
    startupStages.fetch_or(ShutdownObserved);
    LogSite(L"Application ExitProcess", code, LAKE_CALLER_ADDRESS(), true);
    if constexpr (lakefix::profile::kEnabled) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true, std::memory_order_relaxed))
            lakefix::profile::Report(PresenterLog, L"observed-ExitProcess-call");
    }
    lakefix::diagnostics::Shutdown(250); // Bounded control path, never VEH/render/DllMain.
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

bool AdmitLoadedRenderer(HMODULE module) {
    const auto cached = VerifiedRenderer();
    if (cached.module == module) return true; // Lifetime is guaranteed by PIN below.
    if (cached.module) {
        Log(L"Contour load refused: another module/base cannot replace the session-pinned admitted renderer", true);
        SetLastError(ERROR_REVISION_MISMATCH);
        return false;
    }
    if (admittedContour == INVALID_HANDLE_VALUE) { SetLastError(ERROR_INVALID_STATE); return false; }
    wchar_t path[32768]{}, canonical[32768]{};
    const DWORD count = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (!count) return false;
    if (count >= ARRAYSIZE(path)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return false; }
    const HANDLE actual = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (actual == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION identity{};
    const DWORD canonicalLength = GetFinalPathNameByHandleW(actual, canonical, ARRAYSIZE(canonical), FILE_NAME_NORMALIZED);
    const bool valid = GetFileInformationByHandle(actual, &identity) && canonicalLength && canonicalLength < ARRAYSIZE(canonical) &&
        !(identity.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
        lakefix::SameFileIdentity(identity, admittedContourIdentity) && _wcsicmp(canonical, admittedContourPath) == 0;
    CloseHandle(actual);
    if (!valid) {
        startupFailure.store(ERROR_FILE_INVALID);
        Log(L"Startup failed stage=loaded-Contour-identity reason=path-or-file-id-changed action=restore-matching-admitted-installation; no RVAs admitted", true);
        SetLastError(ERROR_FILE_INVALID);
        return false;
    }
    const ULONG_PTR bytes = ImageBytes(module);
    HMODULE pinned{};
    if (!bytes) { SetLastError(ERROR_BAD_EXE_FORMAT); return false; }
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(module), &pinned)) return false;
    if (pinned != module) { SetLastError(ERROR_INVALID_STATE); return false; }
    rendererImage.store((static_cast<std::uint64_t>(bytes) << 32) |
                        static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(module)), std::memory_order_release);
    Log(L"Loaded Contour canonical path/file identity matches retained SHA256-verified file; RVA scope admitted", true);
    return true;
}

bool ContourRequested(const char* name) {
    if (!name || reinterpret_cast<ULONG_PTR>(name) <= 0xffff) return false;
    const char* base = name;
    for (const char* p = name; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
    return _stricmp(base, "Contour.dll") == 0;
}
bool ContourRequested(const wchar_t* name) {
    if (!name || reinterpret_cast<ULONG_PTR>(name) <= 0xffff) return false;
    const wchar_t* base = name;
    for (const wchar_t* p = name; *p; ++p) if (*p == L'\\' || *p == L'/') base = p + 1;
    return _wcsicmp(base, L"Contour.dll") == 0;
}
bool ContourResult(HMODULE module) {
    if (!module) return false;
    if (module == VerifiedRenderer().module) return true;
    wchar_t path[32768]{};
    const DWORD count = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    return count && count < ARRAYSIZE(path) && ContourRequested(path);
}

HMODULE WINAPI HookLoadA(LPCSTR name) {
    HMODULE module = LoadLibraryA(name);
    const DWORD error = GetLastError();
    const bool contour = module && (ContourRequested(name) || ContourResult(module));
    if (contour &&
        (!AdmitLoadedRenderer(module) || !ResolveDrawCreateEx() || !PatchModule(module))) {
        // Don't let the caller initialize a partially hooked graphics module.
        const DWORD failure = GetLastError();
        FreeLibrary(module);
        SetLastError(failure ? failure : ERROR_INVALID_FUNCTION);
        return nullptr;
    }
    if (contour) Log(L"Contour imports patched after LoadLibraryA");
    SetLastError(error);
    return module;
}
HMODULE WINAPI HookLoadW(LPCWSTR name) {
    HMODULE module = LoadLibraryW(name);
    const DWORD error = GetLastError();
    const bool contour = module && (ContourRequested(name) || ContourResult(module));
    if (contour &&
        (!AdmitLoadedRenderer(module) || !ResolveDrawCreateEx() || !PatchModule(module))) {
        const DWORD failure = GetLastError();
        FreeLibrary(module);
        SetLastError(failure ? failure : ERROR_INVALID_FUNCTION);
        return nullptr;
    }
    if (contour) Log(L"Contour imports patched after LoadLibraryW");
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
    HOOK("USER32.dll", "ShowCursor", HookShowCursor),
    HOOK("USER32.dll", "GetCursorPos", HookCursorPosition),
    HOOK("USER32.dll", "SetCursorPos", HookSetCursorPosition),
    HOOK("USER32.dll", "GetClientRect", HookClientRectangle),
    HOOK("USER32.dll", "MapWindowPoints", HookMapWindowPoints),
    HOOK("GDI32.dll", "CreateFontIndirectA", HookCreateFontIndirectA),
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
    const DWORD saved = GetLastError();
    FARPROC result = original;
    if (original && reinterpret_cast<ULONG_PTR>(name) > 0xffff) {
        if (scaleRenderingEnabled && strcmp(name, "DirectDrawCreateEx") == 0 && module == GetModuleHandleW(L"DDRAW.dll")) {
            originalDrawCreateEx.store(reinterpret_cast<DrawCreateEx>(reinterpret_cast<void*>(original)));
            result = reinterpret_cast<FARPROC>(reinterpret_cast<void*>(HookDrawCreateEx));
        }
        for (const auto& hook : hooks) {
        // Compare both module identity and name; never replace another DLL's
        // coincidentally named export. Forwarders are resolved by GetProcAddress.
            if (!hook.applicationOnly && module == hook.provider && strcmp(name, hook.name) == 0) {
                result = reinterpret_cast<FARPROC>(hook.replacement);
                break;
            }
        }
    }
    SetLastError(saved);
    return result;
}

bool ReplaceImport(PVOID volatile* slot, PVOID replacement, PVOID expected) {
    if (*slot == replacement) return true;
    if (!expected || *slot != expected) {
        Log(L"Refusing to overwrite a third-party IAT hook", true);
        SetLastError(ERROR_ALREADY_EXISTS);
        return false;
    }
    DWORD protection{};
    if (!VirtualProtect(const_cast<PVOID*>(slot), sizeof(PVOID), PAGE_READWRITE, &protection)) return false;
    const PVOID previous = InterlockedCompareExchangePointer(slot, replacement, expected);
    DWORD ignored{};
    const bool restored = VirtualProtect(const_cast<PVOID*>(slot), sizeof(PVOID), protection, &ignored) != FALSE;
    if (!restored) return false;
    if (previous != expected && previous != replacement) { SetLastError(ERROR_ALREADY_EXISTS); return false; }
    return true;
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
    if (!success) SetLastError(ERROR_BAD_EXE_FORMAT);
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

namespace {
bool AccessibleMemory(const void* pointer, SIZE_T bytes, bool writable = false) {
    auto cursor = reinterpret_cast<ULONG_PTR>(pointer);
    if (!cursor || bytes > ~static_cast<ULONG_PTR>(0) - cursor) return false;
    const auto end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT || memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        const DWORD protection = memory.Protect & 0xff;
        const bool write = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const bool read = write || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
        if (!read || (writable && !write)) return false;
        const auto next = reinterpret_cast<ULONG_PTR>(memory.BaseAddress) + memory.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

DWORD AdmitInitializeRequest(const lakefix::InitializeRequest* request) {
    if (!AccessibleMemory(request, sizeof(*request)) || request->size != sizeof(*request) || request->abi != lakefix::kComponentAbi ||
        !lakefix::ComponentContractMatches(request->component)) return ERROR_REVISION_MISMATCH;
    for (const auto reserved : request->reserved) if (reserved) return ERROR_INVALID_PARAMETER;
    const auto* path = reinterpret_cast<const wchar_t*>(request->contourPath);
    const size_t length = wcsnlen_s(path, ARRAYSIZE(request->contourPath));
    if (length < 8 || length >= ARRAYSIZE(request->contourPath)) return ERROR_BAD_PATHNAME;
    admittedContour = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (admittedContour == INVALID_HANDLE_VALUE) return GetLastError();
    const auto reject = [](DWORD error) {
        CloseHandle(admittedContour); admittedContour = INVALID_HANDLE_VALUE; return error;
    };
    char digest[65]{}, requestedDigest[65]{};
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        requestedDigest[i * 2] = digits[request->contourSha256[i] >> 4];
        requestedDigest[i * 2 + 1] = digits[request->contourSha256[i] & 15];
    }
    if (!GetFileInformationByHandle(admittedContour, &admittedContourIdentity) ||
        admittedContourIdentity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ||
        !lakefix::Sha256FileHandle(admittedContour, digest)) return reject(ERROR_FILE_INVALID);
    const auto& expected = request->contourIdentity;
    const auto& actual = admittedContourIdentity;
    if (actual.dwVolumeSerialNumber != expected.volumeSerialNumber || actual.nFileIndexHigh != expected.fileIndexHigh ||
        actual.nFileIndexLow != expected.fileIndexLow || actual.nFileSizeHigh != expected.fileSizeHigh ||
        actual.nFileSizeLow != expected.fileSizeLow || actual.ftLastWriteTime.dwHighDateTime != expected.lastWriteTimeHigh ||
        actual.ftLastWriteTime.dwLowDateTime != expected.lastWriteTimeLow || strcmp(digest, requestedDigest)) return reject(ERROR_FILE_INVALID);
#if !defined(LAKE_FIX_TESTING)
    if (strcmp(digest, LAKE_BUILD007_CONTOUR_SHA256)) return reject(ERROR_INVALID_IMAGE_HASH);
    wchar_t executable[32768]{};
    const DWORD executableLength = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!executableLength || executableLength >= ARRAYSIZE(executable)) return reject(ERROR_BAD_PATHNAME);
    const HANDLE exe = CreateFileW(executable, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (exe == INVALID_HANDLE_VALUE) return reject(GetLastError());
    const bool admitted = lakefix::Sha256FileHandle(exe, digest) && strcmp(digest, LAKE_BUILD007_EXE_SHA256) == 0;
    CloseHandle(exe);
    if (!admitted) return reject(ERROR_INVALID_IMAGE_HASH);
#endif
    const DWORD canonicalLength = GetFinalPathNameByHandleW(admittedContour, admittedContourPath, ARRAYSIZE(admittedContourPath), FILE_NAME_NORMALIZED);
    if (!canonicalLength || canonicalLength >= ARRAYSIZE(admittedContourPath) || _wcsicmp(path, admittedContourPath)) return reject(ERROR_BAD_PATHNAME);
    return ERROR_SUCCESS;
}
} // namespace

extern "C" DWORD WINAPI LakeWindowedQueryContract(void* output) {
    auto* contract = static_cast<lakefix::ComponentContract*>(output);
    if (!AccessibleMemory(contract, sizeof(*contract), true) || contract->size != sizeof(*contract)) return ERROR_REVISION_MISMATCH;
    *contract = lakefix::ExpectedComponentContract();
    return ERROR_SUCCESS;
}

#if defined(LAKE_FIX_TESTING)
extern "C" DWORD WINAPI LakeWindowedFlushDiagnostics(void*) {
    return lakefix::diagnostics::Flush(2000) ? ERROR_SUCCESS : ERROR_TIMEOUT;
}
#endif

extern "C" DWORD WINAPI LakeWindowedInitialize(void* argument) {
    if (InterlockedCompareExchange(&initialization, 1, 0) != 0) return ERROR_ALREADY_INITIALIZED;
    const DWORD admitted = AdmitInitializeRequest(static_cast<const lakefix::InitializeRequest*>(argument));
    if (admitted) return admitted;
    // Required independently of the logger: from the first published callback,
    // VEH or IAT onwards, no failed initialization path may unload our code.
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&LakeWindowedInitialize), &pinned)) return GetLastError();
    InitializeLog();
    application = GetModuleHandleW(nullptr);
    applicationBytes = ImageBytes(application);
    lakefix::diagnostics::Options diagnosticOptions;
    diagnosticOptions.buildId = LAKE_FIX_COMPONENT_BUILD_ID;
    diagnosticOptions.statsCallback = DiagnosticStats;
    diagnosticOptions.mainImage = {reinterpret_cast<ULONG_PTR>(application), applicationBytes};
    if (logFile != INVALID_HANDLE_VALUE) {
        lakefix::diagnostics::Start(logFile, diagnosticOptions);
        CloseHandle(logFile); logFile = INVALID_HANDLE_VALUE;
    }
    startupStages.fetch_or(CompatibleDll);
    Log(L"Initialize begin");
    if constexpr (lakefix::profile::kEnabled) {
        Log(lakefix::profile::Initialize()
            ? L"Diagnostic profiling enabled at build time; bounded QPC aggregates, not displayed FPS"
            : L"Diagnostic profiling unavailable: performance clock initialization failed", true);
    }
    if (!LoadResizePolicy()) {
        Log(L"Startup failed stage=config reason=unreadable-unsafe-or-unsupported-schema action=review-INI-with-Lake-closed", true);
        return ERROR_INVALID_DATA;
    }
    lakefix::ConfigureScalePresenter(ScaledViewport, PresenterLog, ClipWindow, PaintLetterbox, renderSize.cx, renderSize.cy, CommitPresentation, AdmitScaledPresentation);
    if (const HMODULE loadedRenderer = GetModuleHandleW(L"Contour.dll"))
        if (!AdmitLoadedRenderer(loadedRenderer)) return ERROR_FILE_INVALID;
    if (!AddVectoredExceptionHandler(1, ObserveException)) Log(L"Exception observer unavailable; exit status still available with --wait");
    readyMessage = RegisterWindowMessageW(L"LakeWindowedFix.Ready.v1");
    clientGoneMessage = RegisterWindowMessageW(L"LakeWindowedFix.ClientGone.v1");
    startupFailedMessage = RegisterWindowMessageW(L"LakeWindowedFix.StartupFailed.v1");
    if (!readyMessage || !clientGoneMessage || !startupFailedMessage) return GetLastError();
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
    startupStages.fetch_or(HooksInstalled);
    // Imports point into this DLL until process exit. Unloading is unsupported.
    Log(L"Initialized outside loader lock; native flags present");
    return ERROR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) self = instance;
    return TRUE;
}
