#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
#include <windows.h>
#include <wtsapi32.h>
#include <ddraw.h>
#include <tlhelp32.h>
#include "ddraw_scale.h"
#include "scale_geometry.h"
#include "product_version.h"
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

// This is a separate, deliberately non-injected x86 application. It loads only
// Windows' DirectDraw, owns every window/surface it changes, and has no Lake,
// proprietary renderer, audio, global input, or process-DPI override path.
static_assert(sizeof(void*) == 4, "LakeFixGraphicsCheck requires Windows x86");

namespace {
constexpr GUID unknownId{0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID draw7Id{0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0, 0x60, 0x97, 0x97, 0xea, 0x5b}};
constexpr GUID surface7Id{0x06675a80, 0x3b9b, 0x11d2, {0xb9, 0x2f, 0, 0x60, 0x97, 0x97, 0xea, 0x5b}};
constexpr GUID d3d7Id{0xf5049e77, 0x4861, 0x11d2, {0xa4, 7, 0, 0xa0, 0xc9, 6, 0x29, 0xa8}};
constexpr wchar_t windowClass[] = L"LakeFixGraphicsCheck.OwnWindow";
constexpr DWORD background = 0x182434;
constexpr DWORD cornerColors[] = {0xf04040, 0x40d860, 0x4080f0, 0xf0d840};
constexpr DWORD swatchColors[] = {0xff0000, 0x00ff00, 0x0000ff, 0x00ffff, 0xff00ff, 0xffffff};
constexpr DWORD controlOff = 0x903050;
constexpr DWORD controlOn = 0x20b8c8;
constexpr DWORD frameColors[] = {0xe07020, 0x8040d0, 0x30c080, 0xd050b0, 0x4088d8, 0xc0b030, 0x78b8a0};
constexpr int pixelTolerance = 8; // Allows native 16-bpp quantization; never changes desktop BPP.

HRESULT WinError() noexcept {
    const DWORD error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

bool NoLakeProcess() noexcept {
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
        if (attempt != 7) Sleep(10);
    }
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool absent = Process32FirstW(snapshot, &entry) != FALSE;
    if (absent) {
        do {
            if (!_wcsicmp(entry.szExeFile, L"Lake Controller.exe")) { absent = false; break; }
        } while (Process32NextW(snapshot, &entry));
        if (absent && GetLastError() != ERROR_NO_MORE_FILES) absent = false;
    }
    CloseHandle(snapshot);
    return absent; // Read-only names only; never attach, stop or alter a process.
}

template<class T> struct ComPtr {
    T* value = nullptr;
    ~ComPtr() { Reset(); }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    void Reset() noexcept {
        if (value) value->lpVtbl->Release(value);
        value = nullptr;
    }
    T* operator->() const noexcept { return value; }
};

template<class T> T Function(HMODULE module, const char* name) noexcept {
    const FARPROC address = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(T) == sizeof(address), "Windows function-pointer size");
    T result = nullptr;
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

struct Report {
    HANDLE file = INVALID_HANDLE_VALUE;
    unsigned passed = 0, failed = 0, inconclusive = 0, skipped = 0;
    bool ioFailed = false;
    ~Report() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
    bool Open(const std::wstring& path) noexcept {
        // Do not overwrite an operator's existing report or an unrelated file.
        file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        return file != INVALID_HANDLE_VALUE;
    }
    void Line(const wchar_t* format, ...) noexcept {
        wchar_t text[2048]{};
        va_list args;
        va_start(args, format);
        const int count = std::vswprintf(text, ARRAYSIZE(text) - 2, format, args);
        va_end(args);
        if (count < 0) std::wcscpy(text, L"report-line-truncated");
        std::wcscat(text, L"\n");
        char utf8[8192]{};
        const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
        if (length <= 1) return;
        std::fwrite(utf8, 1, static_cast<size_t>(length - 1), stdout);
        std::fflush(stdout);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            if (!WriteFile(file, utf8, static_cast<DWORD>(length - 1), &written, nullptr) ||
                written != static_cast<DWORD>(length - 1)) ioFailed = true;
        }
    }
    bool Operation(const wchar_t* operation, HRESULT result) noexcept {
        Line(L"operation=%ls hresult=0x%08lX", operation, static_cast<unsigned long>(result));
        return SUCCEEDED(result);
    }
    void Stage(const wchar_t* name, const wchar_t* status, HRESULT result, const wchar_t* detail) noexcept {
        if (!std::wcscmp(status, L"PASS")) ++passed;
        else if (!std::wcscmp(status, L"FAIL")) ++failed;
        else if (!std::wcscmp(status, L"INCONCLUSIVE")) ++inconclusive;
        else ++skipped;
        Line(L"stage=%ls status=%ls hresult=0x%08lX detail=%ls", name, status,
             static_cast<unsigned long>(result), detail);
    }
};

struct Options {
    bool manual = false, modeGiven = false, confirmed = false, audioIsolated = false, selfTest = false, help = false;
    LONG width = 1088, height = 612;
    std::wstring reportPath;
};

bool Dimension(const wchar_t* text, LONG low, LONG& value) noexcept {
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text, &end, 10);
    if (!text[0] || !end || *end || parsed < low || parsed > 4096) return false;
    value = parsed;
    return true;
}

bool Parse(int argc, wchar_t** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const wchar_t* arg = argv[index];
        if (!std::wcscmp(arg, L"--help") || !std::wcscmp(arg, L"-h")) options.help = true;
        else if (!std::wcscmp(arg, L"--self-test")) options.selfTest = true;
        else if (!std::wcscmp(arg, L"--desktop-confirmed")) options.confirmed = true;
        else if (!std::wcscmp(arg, L"--no-operational-audio-confirmed")) options.audioIsolated = true;
        else if (!std::wcscmp(arg, L"--automated") || !std::wcscmp(arg, L"--manual")) {
            if (options.modeGiven) return false;
            options.modeGiven = true;
            options.manual = !std::wcscmp(arg, L"--manual");
        } else if (!std::wcscmp(arg, L"--width") && index + 1 < argc) {
            if (!Dimension(argv[++index], 320, options.width)) return false;
        } else if (!std::wcscmp(arg, L"--height") && index + 1 < argc) {
            if (!Dimension(argv[++index], 180, options.height)) return false;
        } else if (!std::wcscmp(arg, L"--report") && index + 1 < argc) {
            if (!options.reportPath.empty()) return false;
            options.reportPath = argv[++index];
            if (options.reportPath.empty()) return false;
        } else return false;
    }
    return options.help || options.selfTest || options.modeGiven;
}

void Usage() noexcept {
    std::puts("LakeFixGraphicsCheck.exe --automated|--manual --desktop-confirmed --no-operational-audio-confirmed\n"
              "  [--width 1088 --height 612] [--report NEW-FILE.log]\n"
              "LakeFixGraphicsCheck.exe --self-test | --help\n"
              "GPU execution requires operator confirmation AND active/unlocked desktop checks.\n"
              "Automated: own non-activating window/messages; no keyboard/pointer injection.\n"
              "Exit: 0 scoped checks passed, 1 failure, 2 not executed/usage, 3 inconclusive.\n"
              "This does not validate Lake, a proprietary renderer, or audio.");
}

std::wstring DefaultReportPath() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return {};
    wchar_t* slash = std::wcsrchr(path, L'\\');
    if (!slash) return {};
    slash[1] = 0;
    SYSTEMTIME now{};
    GetSystemTime(&now);
    wchar_t name[128]{};
    std::swprintf(name, ARRAYSIZE(name), L"graphics-check-%04u%02u%02u-%02u%02u%02u-%lu.log",
                  now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
    return std::wstring(path) + name;
}

HMODULE LoadWindowsLibrary(const wchar_t* name, Report& report) {
    wchar_t system[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system, ARRAYSIZE(system));
    if (!length || length >= ARRAYSIZE(system)) return nullptr;
    const std::wstring path = std::wstring(system) + L"\\" + name;
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    report.Line(L"system_library=%ls loaded=%u win32_error=%lu", path.c_str(), module != nullptr,
                module ? 0UL : GetLastError());
    wchar_t actual[32768]{};
    if (module && GetModuleFileNameW(module, actual, ARRAYSIZE(actual)))
        report.Line(L"system_library_resolved=%ls", actual);
    return module;
}

// This is a necessary runtime guard, not a way to infer operator permission.
// No SwitchDesktop, session unlock, hooks or input APIs are used.
bool DesktopReady(Report& report, bool verbose) noexcept {
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || !session) {
        report.Line(L"desktop_guard=blocked reason=session-zero-or-query-failed error=%lu", GetLastError());
        return false;
    }
    LPWSTR data = nullptr;
    DWORD bytes = 0;
    const BOOL stateOk = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSConnectState, &data, &bytes);
    const DWORD stateError = stateOk ? 0 : GetLastError();
    const bool active = stateOk && bytes >= sizeof(WTS_CONNECTSTATE_CLASS) &&
                        *reinterpret_cast<WTS_CONNECTSTATE_CLASS*>(data) == WTSActive;
    if (data) WTSFreeMemory(data);
    data = nullptr;
    bytes = 0;
    const BOOL lockOk = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx, &data, &bytes);
    const DWORD lockError = lockOk ? 0 : GetLastError();
    LONG flags = -1;
    if (lockOk && bytes >= sizeof(WTSINFOEXW)) {
        const auto* info = reinterpret_cast<const WTSINFOEXW*>(data);
        if (info->Level == 1) flags = info->Data.WTSInfoExLevel1.SessionFlags;
    }
    if (data) WTSFreeMemory(data);
    // Windows 10+ is the supported platform; older Win7 reversed these flags.
    const bool unlocked = flags == WTS_SESSIONSTATE_UNLOCK;
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    wchar_t inputName[128]{}, threadName[128]{};
    DWORD needed = 0;
    const bool named = input && GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof(inputName), &needed) &&
                       GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                                                 threadName, sizeof(threadName), &needed);
    if (input) CloseDesktop(input);
    wchar_t stationName[128]{};
    USEROBJECTFLAGS stationFlags{};
    const bool station = GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, stationName, sizeof(stationName), &needed) &&
                         GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &stationFlags, sizeof(stationFlags), &needed) &&
                         !_wcsicmp(stationName, L"WinSta0") && (stationFlags.dwFlags & WSF_VISIBLE);
    const bool sameDesktop = named && station && !std::wcscmp(inputName, threadName) && !_wcsicmp(inputName, L"Default");
    if (verbose || !active || !unlocked || !sameDesktop)
        report.Line(L"desktop_guard session=%lu active=%u state_error=%lu unlocked=%u lock_flags=%ld lock_error=%lu input=%ls thread=%ls station=%ls visible_station=%u same_default=%u",
                    session, active, stateError, unlocked, flags, lockError, inputName, threadName, stationName, station, sameDesktop);
    return active && unlocked && sameDesktop;
}

struct Monitor {
    HMONITOR handle = nullptr;
    RECT bounds{}, work{};
    bool primary = false;
};

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info))
        reinterpret_cast<std::vector<Monitor>*>(context)->push_back(
            {monitor, info.rcMonitor, info.rcWork, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

struct App {
    Options options;
    Report& report;
    HWND window = nullptr;
    HWND foregroundBefore = nullptr;
    HMODULE ddrawModule = nullptr;
    ComPtr<IDirectDraw7> nativeGuard;
    ComPtr<IDirectDraw7> draw;
    ComPtr<IDirectDrawSurface7> primary, canvas;
    ComPtr<IDirectDrawClipper> clipper;
    std::vector<Monitor> monitors;
    bool running = true, needsPresent = false, synthetic = false, buttonDown = false;
    bool state = false, committed = false, pending = false, awaitingCommit = false, safetyStopped = false;
    bool suiteComplete = false, graphicsAttempted = false;
    unsigned clicks = 0, frame = 0, commits = 0;
    LONG pendingWidth = 0, pendingHeight = 0;
    RECT pendingFull{}, pendingViewport{}, committedFull{}, committedViewport{};
    POINT lastMapped{-1, -1};
    HRESULT lastPresent = E_PENDING;
    HRESULT lastFrameResult = E_PENDING;
    DDSURFACEDESC2 primaryDescription{};
    unsigned limitedReadbacks = 0;
    explicit App(const Options& config, Report& output) : options(config), report(output) {}
    ~App() {
        lakefix::ConfigureScalePresenter(nullptr, nullptr);
        canvas.Reset();
        primary.Reset();
        clipper.Reset();
        draw.Reset();
        nativeGuard.Reset();
        if (window && IsWindow(window)) DestroyWindow(window);
        if (ddrawModule) FreeLibrary(ddrawModule);
    }
    SIZE Source() const noexcept { return {options.width, options.height}; }
    RECT Control() const noexcept {
        return {options.width * 3 / 8, options.height * 5 / 12,
                options.width * 5 / 8, options.height * 7 / 12};
    }
    bool Guard() noexcept {
        if (safetyStopped) return false;
        if (!DesktopReady(report, false)) {
            safetyStopped = true;
            committed = false;
            report.Line(L"GPU_STOPPED=desktop-no-longer-confirmed-active-unlocked");
        }
        return !safetyStopped;
    }
};

App* app = nullptr; // One owned window, one UI thread; not a hook or shared registry.

bool Viewport(HWND window, LONG width, LONG height, RECT* full, RECT* fitted) noexcept {
    if (!app || window != app->window || IsIconic(window) || !full || !fitted ||
        width != app->options.width || height != app->options.height) return false;
    RECT client{};
    POINT origin{};
    if (!GetClientRect(window, &client) || !ClientToScreen(window, &origin)) return false;
    RECT fit = lakefix::FitViewport(client.right, client.bottom, width, height);
    if (fit.right <= fit.left || fit.bottom <= fit.top) return false;
    *full = {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
    OffsetRect(&fit, origin.x, origin.y);
    *fitted = fit;
    // The provider proposes geometry. It must not publish an input mapping
    // before the real presentation has actually succeeded.
    app->pendingFull = *full;
    app->pendingViewport = *fitted;
    app->pendingWidth = width;
    app->pendingHeight = height;
    app->pending = true;
    return true;
}

bool SameRect(const RECT& first, const RECT& second) noexcept {
    return first.left == second.left && first.top == second.top &&
           first.right == second.right && first.bottom == second.bottom;
}

void PresentationCommit(HWND window, LONG width, LONG height, const RECT* full, const RECT* fitted) noexcept {
    if (!app || !app->awaitingCommit || window != app->window || !app->pending || !full || !fitted ||
        width != app->options.width || height != app->options.height ||
        width != app->pendingWidth || height != app->pendingHeight ||
        !SameRect(*full, app->pendingFull) || !SameRect(*fitted, app->pendingViewport)) return;
    RECT client{};
    POINT origin{};
    if (!GetClientRect(window, &client) || !ClientToScreen(window, &origin)) return;
    OffsetRect(&client, origin.x, origin.y);
    if (!SameRect(client, *full)) return; // Reject a window resized during the native call.
    // The presenter acknowledges a successful native image Blt, NOT displayed
    // pixels. CheckFrame independently reads and compares those pixels later.
    app->committedFull = *full;
    app->committedViewport = *fitted;
    app->committed = true;
    ++app->commits;
}

void PresenterLog(const wchar_t* text) noexcept {
    if (app) app->report.Line(L"presenter=%ls", text ? text : L"(null)");
}

bool Letterbox(HWND window, const RECT* strips, UINT count) noexcept {
    if (!app || window != app->window || !strips || !app->Guard()) return false;
    POINT origin{};
    if (!ClientToScreen(window, &origin)) return false;
    HDC dc = GetDC(window);
    if (!dc) return false;
    bool ok = true;
    for (UINT index = 0; index < count; ++index) {
        RECT local = strips[index];
        OffsetRect(&local, -origin.x, -origin.y);
        if (local.right > local.left && local.bottom > local.top)
            ok = FillRect(dc, &local, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))) && ok;
    }
    ReleaseDC(window, dc);
    app->report.Line(L"letterbox_fallback=owned-window-GDI success=%u", ok);
    return ok;
}

POINT ClientToCanvas(App& current, LPARAM coordinates) noexcept {
    if (!current.committed) return {-1, -1};
    POINT point{static_cast<short>(LOWORD(coordinates)), static_cast<short>(HIWORD(coordinates))};
    if (!ClientToScreen(current.window, &point)) return {-1, -1};
    return lakefix::ViewToSource(current.committedViewport, current.Source(), point);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* current = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        current = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        current->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(current));
    }
    if (!current) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_MOUSEACTIVATE:
        if (!current->options.manual) return MA_NOACTIVATE;
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        current->needsPresent = true;
        return 0;
    }
    case WM_SIZE:
        current->committed = false;
        current->needsPresent = wparam != SIZE_MINIMIZED;
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        if (!current->options.manual && !current->synthetic) return 0;
        const POINT mapped = ClientToCanvas(*current, lparam);
        current->lastMapped = mapped;
        const bool hit = lakefix::ViewportContains(current->Control(), mapped);
        if (message == WM_LBUTTONDOWN) current->buttonDown = hit;
        else {
            if (hit && current->buttonDown) {
                current->state = !current->state;
                ++current->clicks;
                current->needsPresent = true;
            }
            current->buttonDown = false;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (current->options.manual && wparam == VK_ESCAPE) DestroyWindow(window);
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        current->running = false;
        current->committed = false;
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        current->window = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void Pump(App& current, DWORD milliseconds) noexcept {
    const ULONGLONG until = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { current.running = false; return; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!current.running || GetTickCount64() >= until) return;
        const auto remaining = static_cast<DWORD>(until - GetTickCount64());
        MsgWaitForMultipleObjects(0, nullptr, FALSE, (std::min)(remaining, 30UL), QS_ALLINPUT);
    } while (current.running);
}

void Environment(App& current) {
    auto& report = current.report;
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    report.Line(L"environment process_bits=32 native_arch=%u remote_session=%d process_dpi_aware=%u",
                system.wProcessorArchitecture, GetSystemMetrics(SM_REMOTESESSION), IsProcessDPIAware());
    using Version = LONG (WINAPI*)(OSVERSIONINFOW*);
    auto version = Function<Version>(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    if (version && version(&os) == 0)
        report.Line(L"environment Windows=%lu.%lu build=%lu", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
    report.Line(L"environment virtual_desktop=[%d,%d,%d,%d] primary_metrics=%dx%d",
                GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    HDC dc = GetDC(nullptr);
    if (dc) {
        report.Line(L"environment native_device_caps BITSPIXEL=%d PLANES=%d LOGPIXELS=%dx%d HORZRES=%d VERTRES=%d",
                    GetDeviceCaps(dc, BITSPIXEL), GetDeviceCaps(dc, PLANES), GetDeviceCaps(dc, LOGPIXELSX),
                    GetDeviceCaps(dc, LOGPIXELSY), GetDeviceCaps(dc, HORZRES), GetDeviceCaps(dc, VERTRES));
        ReleaseDC(nullptr, dc);
    }
    // Observe awareness only. Do not change this process, any legacy process,
    // GetDeviceCaps, display mode, or native DirectDraw capability results.
    using GetContext = HANDLE (WINAPI*)();
    using GetAwareness = int (WINAPI*)(HANDLE);
    HMODULE user = GetModuleHandleW(L"user32.dll");
    auto context = Function<GetContext>(user, "GetThreadDpiAwarenessContext");
    auto awareness = Function<GetAwareness>(user, "GetAwarenessFromDpiAwarenessContext");
    report.Line(L"environment thread_dpi_awareness=%d dpi_policy=unchanged-no-manifest-or-runtime-override",
                context && awareness ? awareness(context()) : -1);
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&current.monitors));
    HMODULE shcore = LoadWindowsLibrary(L"shcore.dll", report);
    using Scale = HRESULT (WINAPI*)(HMONITOR, int*);
    auto scale = Function<Scale>(shcore, "GetScaleFactorForMonitor");
    for (size_t index = 0; index < current.monitors.size(); ++index) {
        const auto& monitor = current.monitors[index];
        report.Line(L"monitor=%u primary=%u bounds=[%ld,%ld,%ld,%ld] work=[%ld,%ld,%ld,%ld]",
                    static_cast<unsigned>(index), monitor.primary, monitor.bounds.left, monitor.bounds.top,
                    monitor.bounds.right, monitor.bounds.bottom, monitor.work.left, monitor.work.top,
                    monitor.work.right, monitor.work.bottom);
        int percent = 0;
        const HRESULT scaleResult = scale ? scale(monitor.handle, &percent) : E_NOTIMPL;
        report.Line(L"monitor=%u scale_percent=%d scale_query_hresult=0x%08lX",
                    static_cast<unsigned>(index), percent, static_cast<unsigned long>(scaleResult));
    }
    if (shcore) FreeLibrary(shcore);
    report.Line(L"limitation=desktop-composition-DPI-virtualization-RDP-color-management-and-occlusion-can-affect-readback");
    report.Line(L"limitation=readback-is-sampled-primary-memory-or-DC-not-a-camera-scanout-or-hardware-cursor-test");
}

bool SetClient(App& current, const Monitor& monitor, LONG width, LONG height, bool preferNegative = false) noexcept {
    if (!current.Guard() || !current.running) return false;
    RECT outer{0, 0, width, height};
    const DWORD extended = current.options.manual ? 0 : WS_EX_NOACTIVATE;
    if (!AdjustWindowRectEx(&outer, WS_OVERLAPPEDWINDOW, FALSE, extended)) return false;
    const LONG availableWidth = monitor.work.right - monitor.work.left;
    const LONG availableHeight = monitor.work.bottom - monitor.work.top;
    LONG outerWidth = (std::min)(outer.right - outer.left, availableWidth);
    LONG outerHeight = (std::min)(outer.bottom - outer.top, availableHeight);
    if (outerWidth < 240 || outerHeight < 180) return false;
    const LONG x = preferNegative && monitor.work.left < 0 ? monitor.work.left : monitor.work.left + (availableWidth - outerWidth) / 2;
    const LONG y = preferNegative && monitor.work.top < 0 ? monitor.work.top : monitor.work.top + (availableHeight - outerHeight) / 2;
    current.committed = false;
    const BOOL ok = SetWindowPos(current.window, nullptr, x, y, outerWidth, outerHeight,
                                  SWP_NOACTIVATE | SWP_NOZORDER);
    current.report.Line(L"own_window_set x=%ld y=%ld outer=%ldx%ld success=%u error=%lu",
                        x, y, outerWidth, outerHeight, ok, ok ? 0UL : GetLastError());
    Pump(current, 120);
    RECT client{};
    if (GetClientRect(current.window, &client))
        current.report.Line(L"own_window_actual_client=%ldx%ld", client.right, client.bottom);
    return ok != FALSE;
}

bool CreateOwnWindow(App& current, const Monitor& monitor) noexcept {
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = WindowProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); // Class cursor only; never move the pointer.
    type.lpszClassName = windowClass;
    if (!RegisterClassExW(&type)) return false;
    const DWORD extended = current.options.manual ? 0 : WS_EX_NOACTIVATE;
    current.foregroundBefore = GetForegroundWindow();
    current.window = CreateWindowExW(extended, windowClass,
        L"LakeFix Graphics Check - independent synthetic patterns (no Lake/audio)",
        WS_OVERLAPPEDWINDOW, monitor.work.left, monitor.work.top, 480, 320,
        nullptr, nullptr, type.hInstance, &current);
    if (!current.window) return false;
    if (!SetClient(current, monitor, current.options.width, current.options.height)) return false;
    ShowWindow(current.window, current.options.manual ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    UpdateWindow(current.window);
    Pump(current, 120);
    using WindowDpi = UINT (WINAPI*)(HWND);
    auto dpi = Function<WindowDpi>(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    current.report.Line(L"own_window_dpi=%u dpi_policy=observed-only-not-modified", dpi ? dpi(current.window) : 0);
    return current.running;
}

bool Canonical(IUnknown* first, IUnknown* second) noexcept {
    if (!first || !second) return first == second;
    ComPtr<IUnknown> a, b;
    const HRESULT ha = first->lpVtbl->QueryInterface(first, unknownId, reinterpret_cast<void**>(&a.value));
    const HRESULT hb = second->lpVtbl->QueryInterface(second, unknownId, reinterpret_cast<void**>(&b.value));
    return SUCCEEDED(ha) && SUCCEEDED(hb) && a.value == b.value;
}

bool InitializeDraw(App& current) {
    auto& report = current.report;
    if (!current.Guard()) return false;
    current.ddrawModule = LoadWindowsLibrary(L"ddraw.dll", report);
    if (!current.ddrawModule) return false;
    using Create = HRESULT (WINAPI*)(GUID*, void**, REFIID, IUnknown*);
    auto create = Function<Create>(current.ddrawModule, "DirectDrawCreateEx");
    if (!create) return false;
    current.graphicsAttempted = true;
    HRESULT result = create(nullptr, reinterpret_cast<void**>(&current.draw.value), draw7Id, nullptr);
    if (!report.Operation(L"DirectDrawCreateEx-native", result) || !current.draw.value) return false;
    auto* native = current.draw.value;
    native->lpVtbl->AddRef(native);
    current.nativeGuard.value = native; // Independent genuine reference for native-preservation checks.
    ComPtr<IUnknown> d3dBefore, d3dAfter;
    const HRESULT before = native->lpVtbl->QueryInterface(native, d3d7Id, reinterpret_cast<void**>(&d3dBefore.value));
    report.Operation(L"native-QI-D3D7-before-wrap", before);
    DDSURFACEDESC2 modeBefore{};
    modeBefore.dwSize = sizeof(modeBefore);
    const HRESULT modeHr = native->lpVtbl->GetDisplayMode(native, &modeBefore);
    report.Operation(L"native-GetDisplayMode-before-wrap", modeHr);
    DDCAPS capsBefore{};
    capsBefore.dwSize = sizeof(capsBefore);
    const HRESULT capsHr = native->lpVtbl->GetCaps(native, &capsBefore, nullptr);
    report.Operation(L"native-GetCaps-before-wrap", capsHr);
    const auto* nativeTable = native->lpVtbl;
    void* owned = current.draw.value;
    if (!lakefix::WrapDirectDraw7(&owned)) return false;
    current.draw.value = static_cast<IDirectDraw7*>(owned);
    auto* draw = current.draw.value;
    const HRESULT after = draw->lpVtbl->QueryInterface(draw, d3d7Id, reinterpret_cast<void**>(&d3dAfter.value));
    report.Operation(L"proxy-QI-D3D7-after-wrap", after);
    ComPtr<IUnknown> identity;
    ComPtr<IDirectDraw7> identity7;
    const HRESULT identityHr = draw->lpVtbl->QueryInterface(draw, unknownId, reinterpret_cast<void**>(&identity.value));
    report.Operation(L"proxy-QI-IUnknown", identityHr);
    const HRESULT identity7Hr = identity.value ? identity->lpVtbl->QueryInterface(identity.value, draw7Id,
        reinterpret_cast<void**>(&identity7.value)) : E_POINTER;
    report.Operation(L"proxy-IUnknown-QI-DD7", identity7Hr);
    DDSURFACEDESC2 modeAfter{};
    modeAfter.dwSize = sizeof(modeAfter);
    const HRESULT modeAfterHr = draw->lpVtbl->GetDisplayMode(draw, &modeAfter);
    report.Operation(L"proxy-GetDisplayMode", modeAfterHr);
    DDCAPS capsAfter{};
    capsAfter.dwSize = sizeof(capsAfter);
    const HRESULT capsAfterHr = draw->lpVtbl->GetCaps(draw, &capsAfter, nullptr);
    report.Operation(L"proxy-GetCaps", capsAfterHr);
    const bool preserved = draw != native && native->lpVtbl == nativeTable && before == after &&
        (FAILED(before) || Canonical(d3dBefore.value, d3dAfter.value)) &&
        SUCCEEDED(identityHr) && SUCCEEDED(identity7Hr) && identity.value == reinterpret_cast<IUnknown*>(draw) &&
        identity7.value == draw && modeHr == modeAfterHr && capsHr == capsAfterHr &&
        modeBefore.dwWidth == modeAfter.dwWidth && modeBefore.dwHeight == modeAfter.dwHeight &&
        modeBefore.ddpfPixelFormat.dwRGBBitCount == modeAfter.ddpfPixelFormat.dwRGBBitCount &&
        capsBefore.dwCaps == capsAfter.dwCaps && capsBefore.dwCaps2 == capsAfter.dwCaps2;
    report.Line(L"native_preservation table_unchanged=%u d3d7_available=%u display=%lux%lu bpp=%lu caps=0x%08lX caps2=0x%08lX",
                native->lpVtbl == nativeTable, SUCCEEDED(before), modeAfter.dwWidth, modeAfter.dwHeight,
                modeAfter.ddpfPixelFormat.dwRGBBitCount, capsAfter.dwCaps, capsAfter.dwCaps2);
    report.Stage(L"native-QI-and-proxy-identity", preserved ? L"PASS" : L"FAIL", after,
                 L"D3D7 availability/result and canonical native identity preserved; DD7 proxy identity retained");
    if (!preserved) return false;
    lakefix::ConfigureScalePresenter(Viewport, PresenterLog, nullptr, Letterbox,
                                     current.options.width, current.options.height, PresentationCommit);
    result = draw->lpVtbl->SetCooperativeLevel(draw, current.window, DDSCL_NORMAL);
    if (!report.Operation(L"SetCooperativeLevel-DDSCL_NORMAL", result)) return false;
    DDSURFACEDESC2 primary{};
    primary.dwSize = sizeof(primary);
    primary.dwFlags = DDSD_CAPS;
    primary.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    result = draw->lpVtbl->CreateSurface(draw, &primary, &current.primary.value, nullptr);
    if (!report.Operation(L"CreateSurface-primary", result) || !current.primary.value) return false;
    current.primaryDescription.dwSize = sizeof(current.primaryDescription);
    result = current.primary->lpVtbl->GetSurfaceDesc(current.primary.value, &current.primaryDescription);
    if (!report.Operation(L"primary-GetSurfaceDesc", result)) return false;
    const auto& description = current.primaryDescription;
    report.Line(L"primary size=%lux%lu native_bpp=%lu rgb_masks=%08lX,%08lX,%08lX",
                description.dwWidth, description.dwHeight, description.ddpfPixelFormat.dwRGBBitCount,
                description.ddpfPixelFormat.dwRBitMask, description.ddpfPixelFormat.dwGBitMask,
                description.ddpfPixelFormat.dwBBitMask);
    result = draw->lpVtbl->CreateClipper(draw, 0, &current.clipper.value, nullptr);
    if (!report.Operation(L"CreateClipper", result) || !current.clipper.value) return false;
    result = current.clipper->lpVtbl->SetHWnd(current.clipper.value, 0, current.window);
    if (!report.Operation(L"clipper-SetHWnd-own-window", result)) return false;
    result = current.primary->lpVtbl->SetClipper(current.primary.value, current.clipper.value);
    if (!report.Operation(L"primary-SetClipper", result)) return false;
    DDSURFACEDESC2 canvas{};
    canvas.dwSize = sizeof(canvas);
    canvas.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    canvas.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    canvas.dwWidth = static_cast<DWORD>(current.options.width);
    canvas.dwHeight = static_cast<DWORD>(current.options.height);
    canvas.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    canvas.ddpfPixelFormat.dwFlags = DDPF_RGB;
    canvas.ddpfPixelFormat.dwRGBBitCount = 32;
    canvas.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
    canvas.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
    canvas.ddpfPixelFormat.dwBBitMask = 0x000000ff;
    result = draw->lpVtbl->CreateSurface(draw, &canvas, &current.canvas.value, nullptr);
    if (!report.Operation(L"CreateSurface-offscreen-system-memory-32bpp", result) || !current.canvas.value) return false;
    DDSURFACEDESC2 actual{};
    actual.dwSize = sizeof(actual);
    result = current.canvas->lpVtbl->GetSurfaceDesc(current.canvas.value, &actual);
    if (!report.Operation(L"canvas-GetSurfaceDesc", result)) return false;
    ComPtr<IUnknown> surfaceIdentity;
    ComPtr<IDirectDrawSurface7> surfaceIdentity7;
    const HRESULT sh = current.primary->lpVtbl->QueryInterface(current.primary.value, unknownId,
        reinterpret_cast<void**>(&surfaceIdentity.value));
    report.Operation(L"primary-QI-IUnknown", sh);
    const HRESULT s7h = surfaceIdentity.value ? surfaceIdentity->lpVtbl->QueryInterface(surfaceIdentity.value,
        surface7Id, reinterpret_cast<void**>(&surfaceIdentity7.value)) : E_POINTER;
    report.Operation(L"primary-IUnknown-QI-Surface7", s7h);
    const auto stats = lakefix::GetScalePresenterStats();
    const bool valid = actual.dwWidth == canvas.dwWidth && actual.dwHeight == canvas.dwHeight &&
        actual.ddpfPixelFormat.dwRGBBitCount == 32 && actual.ddpfPixelFormat.dwRBitMask == 0x00ff0000 &&
        actual.ddpfPixelFormat.dwGBitMask == 0x0000ff00 && actual.ddpfPixelFormat.dwBBitMask == 0x000000ff &&
        SUCCEEDED(sh) && SUCCEEDED(s7h) && surfaceIdentity.value == reinterpret_cast<IUnknown*>(current.primary.value) &&
        surfaceIdentity7.value == current.primary.value && stats.directDrawObjects == 1 && stats.primarySurfaces == 1 &&
        current.canvas->lpVtbl != current.primary->lpVtbl;
    report.Stage(L"real-DD7-primary-clipper-canvas", valid ? L"PASS" : L"FAIL", result,
                 L"real NORMAL primary with HWND clipper; native offscreen xRGB32; only DD7/primary proxied");
    return valid;
}

struct Image {
    unsigned char* pixels;
    LONG pitch, width, height;
    void Fill(RECT rect, DWORD color) const noexcept {
        rect.left = (std::max)(0L, rect.left);
        rect.top = (std::max)(0L, rect.top);
        rect.right = (std::min)(width, rect.right);
        rect.bottom = (std::min)(height, rect.bottom);
        for (LONG y = rect.top; y < rect.bottom; ++y) {
            auto* row = reinterpret_cast<DWORD*>(pixels + static_cast<std::ptrdiff_t>(y) * pitch);
            for (LONG x = rect.left; x < rect.right; ++x) row[x] = color;
        }
    }
    void Text(LONG x, LONG y, const char* text, LONG scale = 1) const noexcept {
        // Locally specified 5x7 geometric glyphs; no fonts/assets from a product.
        static constexpr unsigned char digits[][5] = {
            {62,81,73,69,62},{0,66,127,64,0},{98,81,73,73,70},{34,65,73,73,54},{24,20,18,127,16},
            {39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},{54,73,73,73,54},{6,73,73,41,30}};
        static constexpr unsigned char letters[][5] = {
            {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
            {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
            {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
            {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
            {62,65,81,33,94},{127,9,25,41,70},{38,73,73,73,50},{1,1,127,1,1},
            {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
            {3,4,120,4,3},{97,81,73,69,67}};
        for (; *text; ++text, x += 6 * scale) {
            const unsigned char* glyph = nullptr;
            if (*text >= '0' && *text <= '9') glyph = digits[*text - '0'];
            if (*text >= 'A' && *text <= 'Z') glyph = letters[*text - 'A'];
            for (LONG column = 0; column != 5; ++column)
                for (LONG row = 0; glyph && row != 7; ++row)
                    if (glyph[column] & (1U << row))
                        Fill({x + column * scale, y + row * scale,
                              x + (column + 1) * scale, y + (row + 1) * scale}, 0xffffff);
        }
    }
};

struct Probe {
    const wchar_t* name;
    POINT source{};
    POINT screen{};
    DWORD expected = 0;
    COLORREF observed = CLR_INVALID;
    bool bar = false;
};

POINT Center(const RECT& rect) noexcept { return {rect.left + (rect.right - rect.left) / 2, rect.top + (rect.bottom - rect.top) / 2}; }

void Pattern(const Image* image, LONG width, LONG height, bool state, unsigned frame, std::vector<Probe>& probes) {
    if (image) {
        image->Fill({0, 0, width, height}, background);
        for (LONG column = 1; column < 12; ++column) {
            const LONG x = column * width / 12;
            image->Fill({x, 0, x + 1, height}, 0x405060);
        }
        for (LONG row = 1; row < 8; ++row) {
            const LONG y = row * height / 8;
            image->Fill({0, y, width, y + 1}, 0x405060);
        }
    }
    const LONG mx = width / 40, my = height / 32, cw = width / 10, ch = height / 9;
    const RECT corners[] = {{mx, my, mx + cw, my + ch}, {width - mx - cw, my, width - mx, my + ch},
                            {mx, height - my - ch, mx + cw, height - my},
                            {width - mx - cw, height - my - ch, width - mx, height - my}};
    static constexpr const wchar_t* names[] = {L"top-left", L"top-right", L"bottom-left", L"bottom-right"};
    for (unsigned index = 0; index != ARRAYSIZE(corners); ++index) {
        if (image) image->Fill(corners[index], cornerColors[index]);
        probes.push_back({names[index], Center(corners[index]), {}, cornerColors[index]});
    }
    for (LONG index = 0; index != 6; ++index) {
        const RECT swatch{width / 4 + index * width / 12, height * 7 / 8,
                          width / 4 + (index + 1) * width / 12 - 2, height * 15 / 16};
        if (image) image->Fill(swatch, swatchColors[index]);
        probes.push_back({L"RGB-swatch", Center(swatch), {}, swatchColors[index]});
    }
    const RECT button{width * 3 / 8, height * 5 / 12, width * 5 / 8, height * 7 / 12};
    const RECT border{button.left - 2, button.top - 2, button.right + 2, button.bottom + 2};
    const RECT epoch{width * 7 / 16, my, width * 9 / 16, my + ch};
    const DWORD frameColor = frameColors[frame % ARRAYSIZE(frameColors)];
    if (image) {
        image->Fill(border, 0xffffff);
        image->Fill(button, state ? controlOn : controlOff);
        image->Fill(epoch, frameColor);
        const LONG scale = (std::max)(1L, (std::min)(3L, height / 240));
        image->Text(width / 4, height / 4, "LAKEFIX GRAPHICS CHECK", scale);
        image->Text(width / 4, height / 4 + 10 * scale, "OWN GRID CORNERS RGB", scale);
        image->Text(button.left + 6, button.top + (button.bottom - button.top) / 2, state ? "ON" : "OFF", scale);
        image->Text(width / 4, height * 3 / 4, "NO LAKE NO AUDIO", scale);
    }
    probes.push_back({L"control-state", {button.left + (button.right - button.left) / 4,
                                        button.top + (button.bottom - button.top) / 4}, {}, state ? controlOn : controlOff});
    probes.push_back({L"frame-marker", Center(epoch), {}, frameColor});
}

bool DrawPattern(App& current, std::vector<Probe>& probes) {
    if (!current.Guard()) return false;
    DDSURFACEDESC2 lock{};
    lock.dwSize = sizeof(lock);
    HRESULT result = current.canvas->lpVtbl->Lock(current.canvas.value, nullptr, &lock, DDLOCK_WAIT, nullptr);
    current.lastFrameResult = result;
    if (!current.report.Operation(L"canvas-Lock-write-own-pattern", result)) return false;
    const bool format = lock.lpSurface && lock.ddpfPixelFormat.dwRGBBitCount == 32 &&
                        (lock.lPitch >= current.options.width * 4 || lock.lPitch <= -current.options.width * 4) &&
                        lock.dwWidth == static_cast<DWORD>(current.options.width) &&
                        lock.dwHeight == static_cast<DWORD>(current.options.height);
    if (format) {
        const Image image{static_cast<unsigned char*>(lock.lpSurface), lock.lPitch,
                           current.options.width, current.options.height};
        Pattern(&image, image.width, image.height, current.state, current.frame, probes);
    }
    result = current.canvas->lpVtbl->Unlock(current.canvas.value, nullptr);
    current.lastFrameResult = !format && SUCCEEDED(result) ? E_UNEXPECTED : result;
    return current.report.Operation(L"canvas-Unlock", result) && format;
}

bool Present(App& current, std::vector<Probe>& probes) {
    current.lastFrameResult = E_ABORT;
    if (!current.Guard() || !current.running || IsIconic(current.window)) return false;
    ++current.frame;
    if (!DrawPattern(current, probes)) return false;
    current.pending = false;
    current.committed = false;
    current.awaitingCommit = true;
    POINT origin{};
    if (!ClientToScreen(current.window, &origin)) {
        current.lastFrameResult = WinError();
        current.awaitingCommit = false;
        return false;
    }
    // The admitted application operation has the full source extent. The
    // presenter replaces it with the fitted actual client rectangle. The HWND
    // clipper also prevents a bypass from painting another application's area.
    RECT destination{origin.x, origin.y, origin.x + current.options.width, origin.y + current.options.height};
    current.lastPresent = current.primary->lpVtbl->Blt(current.primary.value, &destination,
        current.canvas.value, nullptr, DDBLT_WAIT, nullptr);
    current.awaitingCommit = false;
    current.lastFrameResult = current.lastPresent;
    current.report.Operation(L"primary-full-source-Blt", current.lastPresent);
    current.committed = current.committed && SUCCEEDED(current.lastPresent);
    current.needsPresent = false;
    if (!current.committed) {
        if (SUCCEEDED(current.lastFrameResult)) current.lastFrameResult = E_UNEXPECTED;
        return false;
    }
    current.report.Line(L"presentation frame=%u commits=%u state=%u clicks=%u full=[%ld,%ld,%ld,%ld] fitted=[%ld,%ld,%ld,%ld]",
        current.frame, current.commits, current.state, current.clicks, current.committedFull.left, current.committedFull.top,
        current.committedFull.right, current.committedFull.bottom, current.committedViewport.left,
        current.committedViewport.top, current.committedViewport.right, current.committedViewport.bottom);
    const RECT fit = current.committedViewport;
    for (auto& probe : probes) {
        // Centers of large solid regions, not text/filtered boundaries.
        probe.screen = {fit.left + static_cast<LONG>((2LL * probe.source.x + 1) * (fit.right - fit.left) / (2LL * current.options.width)),
                        fit.top + static_cast<LONG>((2LL * probe.source.y + 1) * (fit.bottom - fit.top) / (2LL * current.options.height))};
    }
    const RECT full = current.committedFull;
    const RECT strips[] = {{full.left, full.top, full.right, fit.top}, {full.left, fit.bottom, full.right, full.bottom},
                           {full.left, fit.top, fit.left, fit.bottom}, {fit.right, fit.top, full.right, fit.bottom}};
    for (const auto& strip : strips)
        if (strip.right - strip.left >= 4 && strip.bottom - strip.top >= 4)
            probes.push_back({L"letterbox-black", {}, Center(strip), 0, CLR_INVALID, true});
    Pump(current, 90); // Allow normal message handling/composition; no injected input.
    return true;
}

DWORD Channel(DWORD pixel, DWORD mask) noexcept {
    if (!mask) return 0;
    unsigned shift = 0;
    while (!(mask & 1)) { ++shift; mask >>= 1; }
    return static_cast<DWORD>((static_cast<std::uint64_t>((pixel >> shift) & mask) * 255 + mask / 2) / mask);
}

bool PrimaryLockRead(App& current, std::vector<Probe>& probes) noexcept {
    DDSURFACEDESC2 lock{};
    lock.dwSize = sizeof(lock);
    const HRESULT result = current.primary->lpVtbl->Lock(current.primary.value, nullptr, &lock,
                                                        DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
    if (!current.report.Operation(L"primary-Lock-read-only", result)) return false;
    const DWORD bits = lock.ddpfPixelFormat.dwRGBBitCount;
    bool valid = lock.lpSurface && (lock.ddpfPixelFormat.dwFlags & DDPF_RGB) && (bits == 16 || bits == 24 || bits == 32);
    for (auto& probe : probes) {
        const LONG x = probe.screen.x, y = probe.screen.y;
        if (!valid || x < 0 || y < 0 || static_cast<DWORD>(x) >= lock.dwWidth || static_cast<DWORD>(y) >= lock.dwHeight) {
            valid = false;
            break;
        }
        DWORD pixel = 0;
        const auto* row = static_cast<const unsigned char*>(lock.lpSurface) + static_cast<std::ptrdiff_t>(y) * lock.lPitch;
        std::memcpy(&pixel, row + static_cast<size_t>(x) * (bits / 8), bits / 8);
        probe.observed = RGB(Channel(pixel, lock.ddpfPixelFormat.dwRBitMask),
                             Channel(pixel, lock.ddpfPixelFormat.dwGBitMask), Channel(pixel, lock.ddpfPixelFormat.dwBBitMask));
    }
    const HRESULT unlock = current.primary->lpVtbl->Unlock(current.primary.value, nullptr);
    current.report.Operation(L"primary-Unlock-read-only", unlock);
    return valid && SUCCEEDED(unlock);
}

bool ReadPixels(App& current, std::vector<Probe>& probes, const wchar_t*& method) noexcept {
    if (!current.Guard()) return false;
    HDC dc = nullptr;
    const HRESULT result = current.primary->lpVtbl->GetDC(current.primary.value, &dc);
    current.report.Operation(L"primary-GetDC-readback", result);
    bool valid = SUCCEEDED(result) && dc;
    if (valid) {
        for (auto& probe : probes) {
            probe.observed = GetPixel(dc, probe.screen.x, probe.screen.y);
            valid = probe.observed != CLR_INVALID && valid;
        }
        const HRESULT release = current.primary->lpVtbl->ReleaseDC(current.primary.value, dc);
        current.report.Operation(L"primary-ReleaseDC-readback", release);
        valid = valid && SUCCEEDED(release);
    }
    if (valid) { method = L"primary-GetDC-GetPixel"; return true; }
    if (PrimaryLockRead(current, probes)) { method = L"primary-Lock-RGB-read-only-origin-0-0"; return true; }
    // Negative desktop coordinates may not be addressable on the primary
    // adapter's surface. Keep this weaker path explicit rather than inventing
    // a successful primary readback or trusting Blt's HRESULT.
    dc = GetDC(current.window);
    if (!dc) return false;
    POINT origin{};
    valid = ClientToScreen(current.window, &origin) != FALSE;
    if (valid) for (auto& probe : probes) {
        probe.observed = GetPixel(dc, probe.screen.x - origin.x, probe.screen.y - origin.y);
        valid = probe.observed != CLR_INVALID && valid;
    }
    ReleaseDC(current.window, dc);
    method = L"owned-window-DC-primary-unavailable-LIMITED";
    if (valid) ++current.limitedReadbacks;
    return valid;
}

bool Matches(const Probe& probe) noexcept {
    if (probe.observed == CLR_INVALID) return false;
    const int expected[] = {static_cast<int>((probe.expected >> 16) & 255),
                            static_cast<int>((probe.expected >> 8) & 255), static_cast<int>(probe.expected & 255)};
    const int actual[] = {GetRValue(probe.observed), GetGValue(probe.observed), GetBValue(probe.observed)};
    for (unsigned index = 0; index != 3; ++index)
        if (std::abs(expected[index] - actual[index]) > pixelTolerance) return false;
    return true;
}

bool Unoccluded(App& current, const std::vector<Probe>& probes) noexcept {
    for (const auto& probe : probes) {
        const HWND atPoint = WindowFromPoint(probe.screen);
        if (atPoint != current.window) {
            current.report.Line(L"readback_occluded sample=%ls screen=[%ld,%ld] owned_window_at_point=0",
                                probe.name, probe.screen.x, probe.screen.y);
            return false;
        }
    }
    return true;
}

bool CheckFrame(App& current, const wchar_t* stage, bool expectedState, unsigned expectedClicks) {
    auto& report = current.report;
    if (!current.Guard() || !current.running) {
        report.Stage(stage, L"INCONCLUSIVE", E_ABORT, L"desktop guard stopped execution or owned window closed");
        return false;
    }
    if (current.state != expectedState || current.clicks != expectedClicks) {
        report.Stage(stage, L"FAIL", E_UNEXPECTED, L"control state/click count changed unexpectedly");
        return false;
    }
    std::vector<Probe> probes;
    if (!Present(current, probes)) {
        report.Stage(stage, current.safetyStopped ? L"INCONCLUSIVE" : L"FAIL", current.lastFrameResult,
                     L"pattern write/presentation/expected-size geometry commit failed");
        return false;
    }
    if (!Unoccluded(current, probes)) {
        report.Stage(stage, L"INCONCLUSIVE", S_FALSE, L"another window covers sampled owned pixels; no third-party window moved");
        return false;
    }
    bool read = false, match = false, occluded = false;
    const wchar_t* method = L"unavailable";
    for (unsigned attempt = 0; attempt != 5 && current.running; ++attempt) {
        if (!current.Guard()) break;
        if (!Unoccluded(current, probes)) { occluded = true; break; }
        read = ReadPixels(current, probes, method);
        match = read && std::all_of(probes.begin(), probes.end(), Matches);
        report.Line(L"readback stage=%ls attempt=%u method=%ls samples=%u pixels_match=%u tolerance=%d",
                    stage, attempt + 1, method, static_cast<unsigned>(probes.size()), match, pixelTolerance);
        if (match || current.safetyStopped) break;
        Pump(current, 100);
    }
    if (match && (!current.running || !Unoccluded(current, probes))) occluded = true;
    for (const auto& probe : probes)
        report.Line(L"pixel stage=%ls name=%ls screen=[%ld,%ld] expected_rgb=%06lX observed_rgb=%02X%02X%02X valid=%u match=%u",
                    stage, probe.name, probe.screen.x, probe.screen.y, probe.expected,
                    GetRValue(probe.observed), GetGValue(probe.observed), GetBValue(probe.observed),
                    probe.observed != CLR_INVALID, Matches(probe));
    const bool ownForeground = current.window && GetForegroundWindow() == current.window;
    report.Line(L"input_policy stage=%ls mode=%ls own_window_foreground=%u global_input=none pointer_position_apis=none hooks=none",
                stage, current.options.manual ? L"manual" : L"automated-no-activate", ownForeground);
    if (!current.options.manual && ownForeground) {
        report.Stage(stage, L"FAIL", E_UNEXPECTED, L"automated owned window became foreground; keyboard isolation not established");
        return false;
    }
    if (current.state != expectedState || current.clicks != expectedClicks) {
        report.Stage(stage, L"FAIL", E_UNEXPECTED, L"control state/click count changed during readback");
        return false;
    }
    const bool inconclusive = !read || occluded || current.safetyStopped;
    report.Stage(stage, inconclusive ? L"INCONCLUSIVE" : match ? L"PASS" : L"FAIL", current.lastPresent,
                 inconclusive ? L"primary/DC readback unavailable or desktop guard stopped; HRESULT alone is not proof" :
                 match ? L"sampled corners/RGB/frame/control/available bars agree with expected state" :
                         L"native Blt returned but displayed samples differ; see desktop/DPI/readback limitations");
    return !inconclusive && match;
}

bool OwnClick(App& current, bool expectedState, unsigned expectedClicks) noexcept {
    if (!current.Guard() || !current.committed) return false;
    const POINT source = Center(current.Control());
    POINT point = lakefix::SourceToView(current.committedViewport, current.Source(), source);
    if (!ScreenToClient(current.window, &point) || point.x < 0 || point.y < 0 || point.x > 32767 || point.y > 32767)
        return false;
    const LPARAM coordinates = MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y));
    current.synthetic = true;
    SendMessageW(current.window, WM_LBUTTONDOWN, MK_LBUTTON, coordinates);
    SendMessageW(current.window, WM_LBUTTONUP, 0, coordinates);
    current.synthetic = false;
    current.report.Line(L"own_message_control client=[%ld,%ld] mapped=[%ld,%ld] state=%u clicks=%u keyboard_or_pointer_injection=none",
                        point.x, point.y, current.lastMapped.x, current.lastMapped.y, current.state, current.clicks);
    return current.state == expectedState && current.clicks == expectedClicks &&
           lakefix::ViewportContains(current.Control(), current.lastMapped);
}

bool RejectedOwnClick(App& current) noexcept {
    if (!current.Guard() || !current.committed) return false;
    const unsigned clicks = current.clicks;
    const bool state = current.state;
    const RECT full = current.committedFull, fit = current.committedViewport;
    POINT point{full.left + 1, full.top + 1}; // Outside the control, in a bar when one exists.
    if (fit.left > full.left + 2) point.x = full.left + (fit.left - full.left) / 2;
    else if (fit.top > full.top + 2) point.y = full.top + (fit.top - full.top) / 2;
    if (!ScreenToClient(current.window, &point)) return false;
    current.synthetic = true;
    const LPARAM coordinates = MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y));
    SendMessageW(current.window, WM_LBUTTONDOWN, MK_LBUTTON, coordinates);
    SendMessageW(current.window, WM_LBUTTONUP, 0, coordinates);
    current.synthetic = false;
    return current.state == state && current.clicks == clicks;
}

bool RestoreSurfaces(App& current) noexcept {
    if (!current.Guard()) return false;
    IDirectDrawSurface7* surfaces[] = {current.primary.value, current.canvas.value};
    for (auto* surface : surfaces) {
        HRESULT result = surface->lpVtbl->IsLost(surface);
        current.report.Operation(surface == current.primary.value ? L"primary-IsLost-after-restore" : L"canvas-IsLost-after-restore", result);
        if (result == DDERR_SURFACELOST) {
            result = surface->lpVtbl->Restore(surface);
            if (!current.report.Operation(L"own-surface-Restore", result)) return false;
        } else if (FAILED(result)) return false;
    }
    const HRESULT result = current.primary->lpVtbl->SetClipper(current.primary.value, current.clipper.value);
    return current.report.Operation(L"primary-rebind-own-clipper-after-restore", result);
}

void Suite(App& current, const Monitor& initial) {
    RECT originalClient{};
    GetClientRect(current.window, &originalClient);
    if (!CheckFrame(current, L"initial-1088x612-or-configured", false, 0) && current.safetyStopped) return;
    if (!current.running) return;
    bool clicked = OwnClick(current, true, 1);
    if (current.safetyStopped) return;
    if (!clicked) current.report.Stage(L"own-control-message", L"FAIL", E_UNEXPECTED, L"committed mapping did not toggle the owned control");
    CheckFrame(current, L"own-control-visible-on", true, 1);
    if (current.safetyStopped || !current.running) return;
    const LONG workWidth = initial.work.right - initial.work.left;
    const LONG workHeight = initial.work.bottom - initial.work.top;
    if (!SetClient(current, initial, workWidth * 9 / 10, workHeight / 2)) {
        current.report.Stage(L"resize-wide", current.safetyStopped ? L"INCONCLUSIVE" : L"FAIL", WinError(), L"could not resize own window");
        return;
    }
    RECT wideClient{};
    GetClientRect(current.window, &wideClient);
    const bool wideChanged = !SameRect(originalClient, wideClient);
    current.report.Stage(L"resize-wide-actual-size", wideChanged ? L"PASS" : L"FAIL", wideChanged ? S_OK : E_FAIL,
                         L"actual owned client dimensions must differ from the original");
    CheckFrame(current, L"resize-wide-preserve-control", true, 1);
    if (current.safetyStopped || !current.running) return;
    const bool ignored = RejectedOwnClick(current);
    if (current.safetyStopped) return;
    current.report.Stage(L"bar-or-background-message-rejected", ignored ? L"PASS" : L"FAIL", ignored ? S_OK : E_UNEXPECTED,
                         L"own synthetic click outside control must not alter state");
    if (!SetClient(current, initial, workWidth / 2, workHeight * 4 / 5)) {
        current.report.Stage(L"resize-tall", current.safetyStopped ? L"INCONCLUSIVE" : L"FAIL", WinError(), L"could not resize own window");
        return;
    }
    RECT tallClient{};
    GetClientRect(current.window, &tallClient);
    const bool tallChanged = !SameRect(wideClient, tallClient) && !SameRect(originalClient, tallClient);
    current.report.Stage(L"resize-tall-actual-size", tallChanged ? L"PASS" : L"FAIL", tallChanged ? S_OK : E_FAIL,
                         L"actual owned client dimensions must differ from original and wide sizes");
    CheckFrame(current, L"resize-tall-preserve-control", true, 1);
    if (current.safetyStopped || !current.running) return;
    clicked = OwnClick(current, false, 2);
    if (current.safetyStopped) return;
    if (!clicked) current.report.Stage(L"own-control-after-resize", L"FAIL", E_UNEXPECTED, L"resized committed mapping did not toggle control");
    CheckFrame(current, L"resized-control-visible-off", false, 2);
    if (current.safetyStopped || !current.running) return;
    if (!SetClient(current, initial, current.options.width, current.options.height)) return;
    RECT returnedClient{};
    GetClientRect(current.window, &returnedClient);
    const bool returned = SameRect(originalClient, returnedClient);
    current.report.Stage(L"return-original-actual-size", returned ? L"PASS" : L"FAIL", returned ? S_OK : E_FAIL,
                         L"actual owned client dimensions must equal the original clamped size");
    CheckFrame(current, L"return-original-corners-and-control", false, 2);
    if (!current.Guard() || !current.running) return;
    ShowWindow(current.window, SW_SHOWMINNOACTIVE);
    Pump(current, 180);
    if (!current.running) return;
    const bool minimized = IsIconic(current.window) != FALSE;
    current.report.Stage(L"minimize-owned-window", minimized ? L"PASS" : L"FAIL", minimized ? S_OK : E_FAIL,
                         L"no presentation while minimized; only owned window changed; no explicit foreign-window/focus API used");
    if (!current.Guard()) return;
    current.report.Operation(L"TestCooperativeLevel-while-minimized", current.draw->lpVtbl->TestCooperativeLevel(current.draw.value));
    ShowWindow(current.window, SW_SHOWNOACTIVATE); // Restores like SHOWNORMAL without activation.
    Pump(current, 200);
    if (!current.running) return;
    if (IsIconic(current.window) || !RestoreSurfaces(current)) {
        current.report.Stage(L"restore-owned-window", current.safetyStopped ? L"INCONCLUSIVE" : L"FAIL", E_FAIL,
                             L"restore or lost-surface recovery failed, or desktop guard stopped execution");
        return;
    }
    CheckFrame(current, L"minimize-restore-corners-and-control", false, 2);
    if (current.safetyStopped || !current.running) return;
    const auto negative = std::find_if(current.monitors.begin(), current.monitors.end(), [](const Monitor& monitor) {
        return monitor.work.left < 0 || monitor.work.top < 0;
    });
    if (negative == current.monitors.end()) {
        current.report.Stage(L"negative-monitor-position", L"SKIP", S_FALSE,
                             L"no available monitor has negative desktop coordinates; no foreign window moved");
    } else if (SetClient(current, *negative, current.options.width, current.options.height, true)) {
        POINT origin{};
        ClientToScreen(current.window, &origin);
        if (origin.x >= 0 && origin.y >= 0)
            current.report.Stage(L"negative-monitor-position", L"SKIP", S_FALSE,
                                 L"available work area cannot place the owned client at a negative origin");
        else CheckFrame(current, L"negative-monitor-position", false, 2);
        if (current.safetyStopped || !current.running) return;
        if (SetClient(current, initial, current.options.width, current.options.height))
            CheckFrame(current, L"return-from-negative-monitor", false, 2);
        else return;
    } else current.report.Stage(L"negative-monitor-position", current.safetyStopped ? L"INCONCLUSIVE" : L"FAIL", WinError(),
                                L"could not position owned window on available negative-coordinate monitor");
    current.suiteComplete = true;
}

int CpuSelfTest(const Options& options) {
    int failures = 0;
    const auto check = [&](bool condition, const char* name) {
        std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) ++failures;
    };
    const LONG width = options.width, height = options.height;
    std::vector<DWORD> pixels(static_cast<size_t>(width) * height);
    const Image image{reinterpret_cast<unsigned char*>(pixels.data()), width * 4, width, height};
    for (unsigned frame = 0; frame != 3; ++frame) {
        std::vector<Probe> probes;
        Pattern(&image, width, height, (frame & 1) != 0, frame, probes);
        bool valid = probes.size() == 12;
        for (const auto& probe : probes)
            valid = valid && probe.source.x >= 0 && probe.source.y >= 0 && probe.source.x < width && probe.source.y < height &&
                    pixels[static_cast<size_t>(probe.source.y) * width + probe.source.x] == probe.expected;
        check(valid, "own pattern pixels, frame marker and control-state expectations");
    }
    RECT viewport = lakefix::FitViewport(701, 503, width, height);
    OffsetRect(&viewport, -1500, -800);
    const POINT point = lakefix::SourceToView(viewport, {width, height}, {width / 2, height / 2});
    const POINT mapped = lakefix::ViewToSource(viewport, {width, height}, point);
    check(mapped.x >= width * 3 / 8 && mapped.x < width * 5 / 8 && mapped.y >= height * 5 / 12 && mapped.y < height * 7 / 12,
          "control mapping in resized negative-coordinate viewport");
    Probe sample{L"test", {}, {}, 0xff0000, RGB(255, 0, 0)};
    check(Matches(sample), "RGB readback channel order");
    sample.observed = RGB(0, 0, 255);
    check(!Matches(sample), "readback rejects wrong color despite successful API result");
    check(Channel(0xf800, 0xf800) == 255 && Channel(0x07e0, 0x07e0) == 255 && Channel(0, 0x001f) == 0,
          "native RGB565 readback decoding without changing desktop BPP");
    std::puts("CPU-ONLY: no window, DirectDraw library, GPU, global input or proprietary application was opened.");
    return failures ? 1 : 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    if (!Parse(argc, argv, options)) { Usage(); return 2; }
    if (options.help) { Usage(); return 0; }
    if (options.selfTest) return CpuSelfTest(options);
    if (options.reportPath.empty()) options.reportPath = DefaultReportPath();
    Report report;
    if (options.reportPath.empty() || !report.Open(options.reportPath)) {
        std::fwprintf(stderr, L"Cannot create NEW report file (error %lu): %ls\n", GetLastError(), options.reportPath.c_str());
        return 2;
    }
    report.Line(L"LakeFixGraphicsCheck independent-synthetic-DD7-check report_version=1");
    report.Line(L"product=%ls build=%ls architecture=x86", LAKE_FIX_VERSION_W, LAKE_FIX_BUILD_ID_W);
    report.Line(L"pid=%lu mode=%ls canvas=%ldx%ld requested_bpp=32", GetCurrentProcessId(),
                options.manual ? L"manual" : L"automated", options.width, options.height);
    report.Line(L"scope=own-application-only NO-Lake NO-proprietary-renderer NO-audio NO-injection NO-global-input");
    if (!options.confirmed || !options.audioIsolated) {
        report.Stage(L"desktop-permission", L"NOT_EXECUTED", E_ACCESSDENIED,
                      L"desktop AND no-operational-audio confirmations required; no window created, ddraw.dll not loaded, GPU not executed");
        report.Line(L"summary=NOT_EXECUTED exit=2 graphics_executed=0");
        return 2;
    }
    if (!NoLakeProcess()) {
        report.Stage(L"operational-safety", L"NOT_EXECUTED", E_ACCESSDENIED,
                     L"a Lake process exists or process inventory is unavailable; no process was changed and GPU was not executed");
        report.Line(L"summary=NOT_EXECUTED exit=2 graphics_executed=0");
        return 2;
    }
    if (!DesktopReady(report, true)) {
        report.Stage(L"desktop-safety", L"NOT_EXECUTED", E_ACCESSDENIED,
                     L"active/unlocked Default desktop could not be established; ddraw.dll not loaded, GPU not executed");
        report.Line(L"summary=NOT_EXECUTED exit=2 graphics_executed=0");
        return 2;
    }
    int exit = 1;
    bool graphicsAttempted = false;
    {
        App current(options, report);
        app = &current;
        Environment(current);
        const auto primary = std::find_if(current.monitors.begin(), current.monitors.end(), [](const Monitor& monitor) { return monitor.primary; });
        if (primary == current.monitors.end() || !CreateOwnWindow(current, *primary)) {
            report.Stage(L"own-window-initialization", L"FAIL", WinError(), L"could not create/place owned window");
        } else if (!InitializeDraw(current)) {
            report.Stage(L"DirectDraw-initialization", current.safetyStopped ? L"INCONCLUSIVE" : L"FAIL", E_FAIL,
                         L"real DirectDraw setup failed; see individual native HRESULTs");
        } else {
            Suite(current, *primary);
            if (!current.suiteComplete)
                report.Stage(L"sequence-incomplete", current.safetyStopped || !current.running ? L"INCONCLUSIVE" : L"FAIL",
                             E_ABORT, L"required resize/return/minimize/restore sequence did not complete");
            if (options.manual && current.running && !current.safetyStopped) {
                report.Line(L"manual_ui=click-owned-control-or-resize-window; ESC-or-close-to-finish; suite-results-remain-recorded");
                while (current.running && current.Guard()) {
                    Pump(current, 60);
                    if (current.running && current.needsPresent && !IsIconic(current.window)) {
                        std::vector<Probe> probes;
                        if (!Present(current, probes)) {
                            report.Stage(L"manual-repaint", current.safetyStopped ? L"INCONCLUSIVE" : L"FAIL",
                                         current.lastFrameResult, L"manual presentation failed or desktop guard stopped execution");
                            break;
                        }
                    }
                }
            }
        }
        report.Line(L"environment foreground_before=%p foreground_after=%p owned_foreground=%u limited_DC_readbacks=%u",
                    static_cast<void*>(current.foregroundBefore), static_cast<void*>(GetForegroundWindow()),
                    current.window && GetForegroundWindow() == current.window, current.limitedReadbacks);
        if (current.safetyStopped && !report.inconclusive)
            report.Stage(L"desktop-became-unavailable", L"INCONCLUSIVE", E_ABORT, L"remaining GPU stages stopped immediately");
        graphicsAttempted = current.graphicsAttempted;
        // Destroy all COM references before checking the presenter's own registry.
    }
    app = nullptr;
    const auto stats = lakefix::GetScalePresenterStats();
    const bool clean = !stats.directDrawObjects && !stats.primarySurfaces && !stats.liveProxyObjects && !stats.ownerContexts;
    report.Stage(L"proxy-cleanup", clean ? L"PASS" : L"FAIL", clean ? S_OK : E_UNEXPECTED,
                 L"owned DD7/primary proxies and owner contexts released");
    exit = report.failed || report.ioFailed ? 1 : report.inconclusive ? 3 : report.passed > 2 ? 0 : 1;
    report.Line(L"summary=%ls exit=%d passed=%u failed=%u inconclusive=%u skipped=%u graphics_executed=%u report_write_error=%u",
                exit == 0 ? L"PASS-SCOPED-ONLY" : exit == 3 ? L"INCONCLUSIVE" : L"FAIL",
                exit, report.passed, report.failed, report.inconclusive, report.skipped, graphicsAttempted, report.ioFailed);
    report.Line(L"NOT-A-VALIDATION-OF-LAKE-PROPRIETARY-RENDERERS-OR-AUDIO");
    if (report.ioFailed) exit = 1;
    return exit;
}
