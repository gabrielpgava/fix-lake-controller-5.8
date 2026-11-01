#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <new>
#include "../src/policy.h"
#include "../src/log_lock.h"
#include "../src/scale_geometry.h"

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
int checks;
int handledTestExceptions;
int destroyQuitCode = -1;
int lastPumpedQuit = -1;
int nativeParentMessages;
int nativeClientSizes;
bool observeSyntheticMouse;
int syntheticMouseMessages;
UINT lastMouseMessage;
WPARAM lastMouseKeys;
POINT lastMousePoint{};
bool passWheelToDefault;
int unhandledWheelMessages;
HWND wheelChild;
bool sendWheelToChild;
int syntheticChildMouseMessages;
POINT lastChildMousePoint{};

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
    ++checks;
    printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

void TestScaledWindowSizePolicy() {
    const SIZE defaults = lakefix::ScaledMinimum(lakefix::kScaledMinWidth, lakefix::kScaledMinHeight);
    Check(defaults.cx == 800 && defaults.cy == 450, "scaled minimum defaults to 800x450 logical client pixels");
    const struct {
        UINT width, height;
        SIZE expected;
    } minimumCases[] = {
        {320, 180, {320, 180}}, {8192, 8192, {8192, 8192}}, {1200, 675, {1200, 675}},
        {0, 0, {800, 450}}, {319, 600, {800, 600}}, {960, 179, {960, 450}},
        {8193, 700, {800, 700}}, {1024, 8193, {1024, 450}},
        {static_cast<UINT>(-1), 720, {800, 720}}, {1280, static_cast<UINT>(-1), {1280, 450}}
    };
    for (const auto& value : minimumCases) {
        const SIZE result = lakefix::ScaledMinimum(value.width, value.height);
        Check(result.cx == value.expected.cx && result.cy == value.expected.cy,
              "minimum bounds accept inclusive limits and fall back independently per axis");
    }
    const SIZE normal = lakefix::AtLeastMinimum({1024, 576}, defaults);
    Check(normal.cx == 1024 && normal.cy == 576, "default initial view remains 1024x576");
    const SIZE raised = lakefix::AtLeastMinimum({320, 180}, {1200, 675});
    Check(raised.cx == 1200 && raised.cy == 675, "initial view grows to a larger configured minimum");
    const SIZE mixed = lakefix::AtLeastMinimum({1400, 360}, defaults);
    Check(mixed.cx == 1400 && mixed.cy == 450, "initial minimum is applied independently per axis");
    const struct {
        SIZE requested, work, expected;
    } workCases[] = {
        {{816, 489}, {1920, 1040}, {816, 489}}, {{1000, 580}, {1000, 580}, {1000, 580}},
        {{1216, 714}, {1000, 580}, {1000, 580}}, {{2016, 489}, {1000, 580}, {1000, 489}},
        {{816, 939}, {1000, 580}, {816, 580}}, {{816, 489}, {0, -1}, {816, 489}}
    };
    for (const auto& value : workCases) {
        const SIZE result = lakefix::ClampToWorkArea(value.requested, value.work);
        Check(result.cx == value.expected.cx && result.cy == value.expected.cy,
              "outer minimum fits the work area per axis without enlarging it or using unknown bounds");
    }
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
    ++nativeParentMessages;
    if (message == WM_SIZE && lake && wparam != SIZE_MINIMIZED && LOWORD(lparam) && HIWORD(lparam)) {
        ++resizes;
        MoveWindow(lake, 0, 0, LOWORD(lparam), HIWORD(lparam), FALSE);
    }
    return DefWindowProcA(window, message, wparam, lparam);
}
LRESULT NativeFixtureProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, bool unicode) {
    if (message == WM_CREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        if (creation->lpCreateParams) {
            // The main HWND has not returned through AttachClient yet. Keep
            // this control away from every existing synthetic hit-test point.
            auto* initialChild = static_cast<HWND*>(creation->lpCreateParams);
            *initialChild = CreateWindowExW(0, L"LakeFix.InputChild", L"", WS_CHILD | WS_VISIBLE,
                                           1400, 820, 30, 20, window, nullptr, creation->hInstance, nullptr);
        }
    }
    if (message == WM_SIZE) ++nativeClientSizes;
    if (passWheelToDefault && (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)) {
        // Bound a regression's recursion in the synthetic target only. The
        // production hook must prevent even the first parent->child bounce.
        if (++unhandledWheelMessages > 8) return 0;
        if (sendWheelToChild && wheelChild && window == lake)
            return SendMessageW(wheelChild, message, wparam, lparam);
        return unicode ? DefWindowProcW(window, message, wparam, lparam)
                       : DefWindowProcA(window, message, wparam, lparam);
    }
    if (observeSyntheticMouse && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) {
        ++syntheticMouseMessages;
        lastMouseMessage = message;
        lastMouseKeys = wparam;
        lastMousePoint = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        return 0; // Record only explicitly sent synthetic input; no real input is generated.
    }
    if (message == 0x8032) ++startupNotifications;
    if (message == WM_KILLFOCUS) ++focusLosses;
    if (message == WM_ACTIVATEAPP) ++activations;
    if (message == WM_CLOSE) {
        ++closeRequests;
        if (vetoClose) return 0;
    }
    if (message == WM_DESTROY && destroyQuitCode >= 0) PostQuitMessage(destroyQuitCode);
    return unicode ? DefWindowProcW(window, message, wparam, lparam)
                   : DefWindowProcA(window, message, wparam, lparam);
}
LRESULT CALLBACK LakeProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return NativeFixtureProc(window, message, wparam, lparam, false);
}
LRESULT CALLBACK UnicodeLakeProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return NativeFixtureProc(window, message, wparam, lparam, true);
}
LRESULT CALLBACK InputChildProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SETCURSOR) {
        // Model the native button bar: never propagate this to main/host.
        SetCursor(nullptr);
        return TRUE;
    }
    if (passWheelToDefault && (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)) {
        if (++unhandledWheelMessages > 8) return 0;
    }
    if (observeSyntheticMouse && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) {
        ++syntheticChildMouseMessages;
        lastChildMousePoint = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void CheckChildCursor(HWND window, const char* name) {
    const HCURSOR previousCursor = GetCursor();
    const HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
    SetCursor(nullptr);
    const LRESULT selected = window ? SendMessageW(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window),
                                                  MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)) : 0;
    Check(window && selected && arrow && GetCursor() == arrow, name);
    const LRESULT native = window ? SendMessageW(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window),
                                                MAKELPARAM(HTLEFT, WM_MOUSEMOVE)) : 0;
    Check(window && native && GetCursor() == nullptr, "child nonclient cursor request still reaches the native handler");
    SetCursor(previousCursor); // Only cursor selection changes; no global movement.
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

bool SameRectangle(const RECT& first, const RECT& second) {
    return first.left == second.left && first.top == second.top &&
           first.right == second.right && first.bottom == second.bottom;
}

bool FixedScaledSource(HWND renderer, HWND frame) {
    RECT rectangle{};
    POINT origin{};
    if (!GetClientRect(renderer, &rectangle)) return false;
    MapWindowPoints(renderer, frame, &origin, 1);
    return rectangle.left == 0 && rectangle.top == 0 && rectangle.right == 1536 &&
           rectangle.bottom == 864 && origin.x == 0 && origin.y == 0;
}

// Independent oracle for a captured drag: continue the affine transform on
// each axis, including outside the image. No clamp and no shared sentinel.
POINT ExpectedCapturedPoint(const RECT& viewport, SIZE source, POINT point) {
    const auto axis = [](LONG p, LONG edge, LONG size, LONG view) {
        const std::int64_t product = (static_cast<std::int64_t>(p) - edge) * size;
        const std::int64_t quotient = product / view;
        return static_cast<LONG>(quotient - (product < 0 && product % view != 0 ? 1 : 0));
    };
    return {axis(point.x, viewport.left, source.cx, viewport.right - viewport.left),
            axis(point.y, viewport.top, source.cy, viewport.bottom - viewport.top)};
}

void CheckSyntheticMouse(HWND renderer, UINT message, POINT view, POINT expected,
                         WPARAM keys, bool delivered, const char* name) {
    const int before = syntheticMouseMessages;
    observeSyntheticMouse = true;
    SendMessageW(renderer, message, keys,
                 MAKELPARAM(static_cast<short>(view.x), static_cast<short>(view.y)));
    observeSyntheticMouse = false;
    const bool matched = syntheticMouseMessages == before + 1 && lastMouseMessage == message &&
                         lastMouseKeys == keys && lastMousePoint.x == expected.x && lastMousePoint.y == expected.y;
    Check(delivered ? matched : syntheticMouseMessages == before, name);
    if (delivered && !matched)
        printf("  view (%ld,%ld), received (%ld,%ld), expected (%ld,%ld), deliveries=%d\n",
               view.x, view.y, lastMousePoint.x, lastMousePoint.y, expected.x, expected.y,
               syntheticMouseMessages - before);
}

void TestScaledMouse(HWND renderer, LONG width, LONG height) {
    printf("Synthetic mouse viewport %ldx%ld (no global cursor movement)\n", width, height);
    const SIZE source{1536, 864};
    const RECT viewport = lakefix::FitViewport(width, height, source.cx, source.cy);
    const POINT center{viewport.left + (viewport.right - viewport.left) / 2,
                       viewport.top + (viewport.bottom - viewport.top) / 2};
    const POINT inside[] = {
        {viewport.left, viewport.top}, {viewport.right - 1, viewport.top},
        {viewport.left, viewport.bottom - 1}, {viewport.right - 1, viewport.bottom - 1}, center};
    const char* names[] = {"scaled mouse top-left", "scaled mouse top-right", "scaled mouse bottom-left",
                           "scaled mouse bottom-right", "scaled mouse center"};
    for (size_t i = 0; i < ARRAYSIZE(inside); ++i)
        CheckSyntheticMouse(renderer, WM_MOUSEMOVE, inside[i],
                            lakefix::ViewToSource(viewport, source, inside[i]), 0, true, names[i]);
    CheckSyntheticMouse(renderer, WM_LBUTTONDOWN, center, lakefix::ViewToSource(viewport, source, center),
                        MK_LBUTTON, true, "scaled button down maps center and preserves keys");
    CheckSyntheticMouse(renderer, WM_LBUTTONUP, center, lakefix::ViewToSource(viewport, source, center),
                        0, true, "scaled button up maps center");
    CheckSyntheticMouse(renderer, WM_LBUTTONDOWN, inside[0], {0, 0}, MK_LBUTTON, true,
                        "scaled button down maps inclusive corner");
    CheckSyntheticMouse(renderer, WM_LBUTTONUP, inside[3], lakefix::ViewToSource(viewport, source, inside[3]),
                         0, true, "scaled button up maps final destination pixel");
    POINT origin{};
    ClientToScreen(renderer, &origin);
    const POINT sourceCenter = lakefix::ViewToSource(viewport, source, center);
    const UINT wheels[]{WM_MOUSEWHEEL, WM_MOUSEHWHEEL};
    for (UINT wheel : wheels)
        CheckSyntheticMouse(renderer, wheel, {origin.x + center.x, origin.y + center.y},
                            {origin.x + sourceCenter.x, origin.y + sourceCenter.y},
                            MAKEWPARAM(MK_CONTROL, WHEEL_DELTA), true,
                            "wheel maps screen coordinates once and preserves delta/keys");

    const POINT outside[] = {{center.x, viewport.top - 1}, {center.x, viewport.bottom},
                             {viewport.left - 1, center.y}, {viewport.right, center.y}};
    for (const POINT point : outside) {
        CheckSyntheticMouse(renderer, WM_MOUSEMOVE, point, {-1, -1}, 0, true,
                            "outside/letterbox hover is signed (-1,-1)");
        CheckSyntheticMouse(renderer, WM_LBUTTONDOWN, point, {-1, -1}, MK_LBUTTON, false,
                            "outside/letterbox button down is not delivered");
        CheckSyntheticMouse(renderer, WM_LBUTTONUP, point, {-1, -1}, 0, true,
                            "outside button up is delivered for native release cleanup");
    }
    if (viewport.top > 0) {
        CheckSyntheticMouse(renderer, WM_MOUSEMOVE, {center.x, 0}, {-1, -1}, 0, true,
                            "entire top letterbox bar is outside");
        CheckSyntheticMouse(renderer, WM_LBUTTONDOWN, {center.x, height - 1}, {-1, -1}, MK_LBUTTON, false,
                            "bottom letterbox bar cannot activate native controls");
    }

    SetCapture(renderer); // The fixture's own window only; never move the global cursor.
    const bool captured = GetCapture() == renderer;
    Check(captured, "synthetic renderer can own mouse capture");
    if (captured) {
        CheckSyntheticMouse(renderer, WM_MOUSEMOVE, {-7, -9}, ExpectedCapturedPoint(viewport, source, {-7, -9}), MK_LBUTTON, true,
                             "captured negative coordinates extrapolate without jumping to sentinel");
        CheckSyntheticMouse(renderer, WM_LBUTTONDOWN, {-7, -9}, {-1, -1}, MK_LBUTTON, false,
                             "capture does not allow an outside button down");
        CheckSyntheticMouse(renderer, WM_LBUTTONUP, {-7, -9}, ExpectedCapturedPoint(viewport, source, {-7, -9}), 0, true,
                             "captured outside button up reaches native cleanup");
        for (const POINT point : outside) {
            CheckSyntheticMouse(renderer, WM_MOUSEMOVE, point, ExpectedCapturedPoint(viewport, source, point),
                                MK_LBUTTON | MK_CONTROL | MK_SHIFT, true,
                                "captured edge preserves the valid axis and modifiers");
            CheckSyntheticMouse(renderer, WM_XBUTTONUP, point, ExpectedCapturedPoint(viewport, source, point),
                                MAKEWPARAM(MK_CONTROL, XBUTTON2), true,
                                "captured extra-button release preserves destination and button");
        }
        CheckSyntheticMouse(renderer, WM_MOUSEMOVE, center, lakefix::ViewToSource(viewport, source, center),
                            MK_LBUTTON, true, "captured inside movement is scaled once");
        ReleaseCapture();
        Check(GetCapture() != renderer, "synthetic capture is released");
    }
}

void TestScaledMessagePoints(HWND renderer, HWND frame) {
    // Obtain unredirected peeks via KernelBase's original resolver. This never
    // moves the global cursor and compares the exact same queued MSG twice.
    auto lookup = reinterpret_cast<decltype(&GetProcAddress)>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"KernelBase.dll"), "GetProcAddress")));
    Check(lookup != nullptr, "obtain original resolver for message-queue comparison");
    if (!lookup) return;
    auto rawPeek = reinterpret_cast<decltype(&PeekMessageW)>(reinterpret_cast<void*>(
        lookup(GetModuleHandleW(L"user32.dll"), "PeekMessageW")));
    Check(rawPeek != nullptr, "obtain original queue peek without fake user input");
    if (!rawPeek) return;
    RECT area{};
    GetClientRect(frame, &area);
    const SIZE source{1536, 864};
    const RECT viewport = lakefix::FitViewport(area.right, area.bottom, source.cx, source.cy);
    POINT origin{};
    ClientToScreen(renderer, &origin);
    auto rawCursor = reinterpret_cast<decltype(&GetCursorPos)>(reinterpret_cast<void*>(
        lookup(GetModuleHandleW(L"user32.dll"), "GetCursorPos")));
    Check(rawCursor != nullptr, "original cursor getter is available without cursor movement");
    if (rawCursor) {
        SetCapture(renderer);
        POINT raw{}, mapped{}, after{};
        const bool queried = rawCursor(&raw) && GetCursorPos(&mapped) && rawCursor(&after);
        ReleaseCapture();
        const POINT expected = ExpectedCapturedPoint(viewport, source,
                                                      {raw.x - origin.x, raw.y - origin.y});
        Check(queried && raw.x == after.x && raw.y == after.y && mapped.x == origin.x + expected.x &&
              mapped.y == origin.y + expected.y,
              "captured GetCursorPos maps even outside image without moving real cursor");
    }
    const LPARAM coordinates = MAKELPARAM(100, 90);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND inputChild = IsWindowUnicode(renderer)
        ? CreateWindowExW(0, L"LakeFix.InputChild", L"", WS_CHILD | WS_VISIBLE,
                          40, 50, 200, 100, renderer, nullptr, instance, nullptr)
        : CreateWindowExA(0, "LakeFix.InputChild", "", WS_CHILD | WS_VISIBLE,
                          40, 50, 200, 100, renderer, nullptr, instance, nullptr);
    Check(inputChild != nullptr, "create distinct child HWND inside virtual canvas");
    CheckChildCursor(inputChild, "future A/W child selects OS arrow before its cursor-hiding native handler");
    for (int kind = 0; kind < (inputChild ? 8 : 4); ++kind) {
        const HWND target = kind < 4 ? renderer : inputChild;
        const LPARAM originalCoordinates = kind < 4 ? coordinates : MAKELPARAM(60, 40);
        const int api = kind % 4;
        MSG raw{}, mapped{}, repeated{}, removed{};
        Check(PostMessageW(target, WM_MOUSEMOVE, MK_SHIFT, originalCoordinates) != FALSE,
              "post synthetic mouse without moving system cursor");
        Check(rawPeek(&raw, target, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_NOREMOVE) != FALSE,
              "original peek observes unmodified queued mouse");
        const POINT expected = lakefix::ViewToSource(viewport, source,
                                                     {raw.pt.x - origin.x, raw.pt.y - origin.y});
        BOOL got = FALSE;
        if (api == 0) got = PeekMessageA(&mapped, target, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_NOREMOVE);
        if (api == 1) got = PeekMessageW(&mapped, target, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_NOREMOVE);
        if (api == 2) got = GetMessageA(&mapped, target, WM_MOUSEMOVE, WM_MOUSEMOVE);
        if (api == 3) got = GetMessageW(&mapped, target, WM_MOUSEMOVE, WM_MOUSEMOVE);
        Check(got > 0 && mapped.hwnd == renderer && mapped.pt.x == origin.x + expected.x &&
              mapped.pt.y == origin.y + expected.y && mapped.lParam == coordinates && mapped.wParam == MK_SHIFT,
              "Get/Peek routes to main with mapped pt and unscaled main-client lParam");
        if (api < 2) {
            Check(PeekMessageW(&repeated, target, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_NOREMOVE) &&
                  repeated.pt.x == mapped.pt.x && repeated.pt.y == mapped.pt.y,
                  "repeated PM_NOREMOVE never double-scales MSG.pt");
            Check(rawPeek(&removed, target, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE) &&
                  removed.pt.x == raw.pt.x && removed.pt.y == raw.pt.y && removed.lParam == originalCoordinates,
                  "queue retains original pt after mapped peek");
        }
        const int before = syntheticMouseMessages;
        const int beforeChild = syntheticChildMouseMessages;
        observeSyntheticMouse = true;
        if (IsWindowUnicode(renderer)) DispatchMessageW(&mapped); else DispatchMessageA(&mapped);
        observeSyntheticMouse = false;
        const POINT clientExpected = lakefix::ViewToSource(viewport, source, {100, 90});
        Check(syntheticMouseMessages == before + 1 && lastMousePoint.x == clientExpected.x &&
              lastMousePoint.y == clientExpected.y && syntheticChildMouseMessages == beforeChild,
              "dispatch routes original child input through main and scales only once");
    }
    const POINT center{area.right / 2, area.bottom / 2};
    CheckSyntheticMouse(frame, WM_MOUSEMOVE, center, lakefix::ViewToSource(viewport, source, center),
                        0, true, "host-area mouse forwards to fixed client and scales once");
    Check(PostMessageW(frame, WM_MOUSEMOVE, 0, MAKELPARAM(center.x, center.y)) != FALSE,
          "post synthetic host-area mouse");
    MSG hostMouse{};
    Check(PeekMessageW(&hostMouse, frame, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE) && hostMouse.hwnd == renderer &&
          hostMouse.lParam == MAKELPARAM(center.x, center.y), "queued host-area mouse targets client without pre-scaling lParam");
    if (inputChild) {
        const POINT viewPoint = lakefix::SourceToView(viewport, source, {100, 90});
        POINT virtualLocal = lakefix::ViewToSource(viewport, source, viewPoint);
        MapWindowPoints(renderer, inputChild, &virtualLocal, 1);
        PostMessageW(renderer, WM_MOUSEMOVE, 0, MAKELPARAM(viewPoint.x, viewPoint.y));
        MSG childMouse{};
        Check(PeekMessageW(&childMouse, renderer, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE) &&
              childMouse.hwnd == inputChild && GET_X_LPARAM(childMouse.lParam) == virtualLocal.x &&
              GET_Y_LPARAM(childMouse.lParam) == virtualLocal.y,
              "virtual hit-test selects native child independently of physical hit HWND");
        const int beforeChild = syntheticChildMouseMessages;
        observeSyntheticMouse = true;
        DispatchMessageW(&childMouse);
        observeSyntheticMouse = false;
        Check(syntheticChildMouseMessages == beforeChild + 1 && lastChildMousePoint.x == virtualLocal.x &&
               lastChildMousePoint.y == virtualLocal.y, "native child receives exactly one virtual local mouse point");

        SetCapture(inputChild);
        Check(GetCapture() == inputChild, "native child can own capture without shim-owned capture state");
        const POINT points[] = {{-7, 90}, {100, -9}, {area.right + 7, area.bottom + 9}, {100, 90}};
        const UINT messages[] = {WM_MOUSEMOVE, WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP};
        for (UINT message : messages) {
            for (const POINT point : points) {
                POINT expectedLocal = ExpectedCapturedPoint(viewport, source, point);
                MapWindowPoints(renderer, inputChild, &expectedLocal, 1);
                const WPARAM keys = message == WM_XBUTTONUP ? MAKEWPARAM(MK_SHIFT, XBUTTON2) : MK_CONTROL;
                PostMessageW(renderer, message, keys, MAKELPARAM(point.x, point.y));
                MSG peek{}, again{}, removed{};
                const bool first = PeekMessageA(&peek, renderer, message, message, PM_NOREMOVE) != FALSE;
                const bool second = PeekMessageW(&again, renderer, message, message, PM_NOREMOVE) != FALSE;
                const bool got = GetMessageW(&removed, renderer, message, message) > 0;
                Check(first && second && got && peek.hwnd == inputChild && removed.hwnd == inputChild &&
                      peek.lParam == again.lParam && again.lParam == removed.lParam && removed.wParam == keys &&
                      GET_X_LPARAM(removed.lParam) == expectedLocal.x && GET_Y_LPARAM(removed.lParam) == expectedLocal.y,
                      "captured child gets continuous local point, buttons and repeat-peek exactly once");
                const int before = syntheticChildMouseMessages;
                observeSyntheticMouse = true;
                DispatchMessageW(&removed);
                observeSyntheticMouse = false;
                Check(syntheticChildMouseMessages == before + 1 && lastChildMousePoint.x == expectedLocal.x &&
                      lastChildMousePoint.y == expectedLocal.y, "captured child dispatch preserves release recipient without double transform");
            }
        }
        ReleaseCapture();
        Check(GetCapture() == nullptr, "native ReleaseCapture leaves no shim interaction to become stuck");
    }
    passWheelToDefault = true;
    unhandledWheelMessages = 0;
    const LPARAM screenCenter = MAKELPARAM(origin.x + center.x, origin.y + center.y);
    SendMessageW(renderer, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), screenCenter);
    Check(unhandledWheelMessages == 1, "unconsumed child wheel propagates to parent without recursion");
    unhandledWheelMessages = 0;
    SendMessageW(frame, WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA), screenCenter);
    Check(unhandledWheelMessages == 1, "unconsumed host wheel reaches client only once");
    wheelChild = inputChild;
    sendWheelToChild = true;
    unhandledWheelMessages = 0;
    SendMessageW(renderer, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), screenCenter);
    Check(unhandledWheelMessages == 2, "native child wheel propagation never re-enters main router");
    sendWheelToChild = false;
    wheelChild = nullptr;
    passWheelToDefault = false;
    if (inputChild) DestroyWindow(inputChild);
}

void ResizeScaledHost(HWND renderer, HWND frame, LONG width, LONG height, bool moveWindow,
                      int originalSizeMessages) {
    RECT outer{0, 0, width, height};
    const DWORD style = static_cast<DWORD>(GetWindowLongW(frame, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongW(frame, GWL_EXSTYLE));
    const bool adjusted = AdjustWindowRectEx(&outer, style, FALSE, exStyle) != FALSE;
    Check(adjusted, "compute scaled host non-client borders");
    if (!adjusted) return;
    BOOL resized;
    if (moveWindow) {
        RECT position{};
        GetWindowRect(frame, &position);
        resized = MoveWindow(frame, position.left, position.top, outer.right - outer.left,
                             outer.bottom - outer.top, FALSE);
    } else {
        resized = SetWindowPos(frame, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
                               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Check(resized != FALSE, moveWindow ? "real MoveWindow resizes only the owned host"
                                       : "real SetWindowPos resizes only the owned host");
    Pump();
    RECT actual{};
    GetClientRect(frame, &actual);
    Check(actual.left == 0 && actual.top == 0 && actual.right == width && actual.bottom == height,
          moveWindow ? "host client really becomes 400x300" : "host client really becomes 800x450");
    Check(FixedScaledSource(renderer, frame), "real host resize preserves 1536x864 source at origin");
    Check(resizes == 0 && nativeClientSizes == originalSizeMessages && nativeParentMessages == 0,
          "real host resize never calls native WM_SIZE/rebuild");
    if (actual.right == width && actual.bottom == height) TestScaledMouse(renderer, width, height);
}

void CheckScaledHostSizePolicy(HWND frame, const wchar_t* configuration) {
    const SIZE minimum = lakefix::ScaledMinimum(
        GetPrivateProfileIntW(L"window", L"MinWidth", lakefix::kScaledMinWidth, configuration),
        GetPrivateProfileIntW(L"window", L"MinHeight", lakefix::kScaledMinHeight, configuration));
    const DWORD style = static_cast<DWORD>(GetWindowLongW(frame, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongW(frame, GWL_EXSTYLE));
    RECT minimumOuter{0, 0, minimum.cx, minimum.cy}, borders{};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    const bool geometry = AdjustWindowRectEx(&minimumOuter, style, FALSE, exStyle) &&
        AdjustWindowRectEx(&borders, style, FALSE, exStyle) &&
        GetMonitorInfoW(MonitorFromWindow(frame, MONITOR_DEFAULTTONEAREST), &monitor);
    Check(geometry, "obtain logical frame borders and host monitor work area for minimum policy");
    if (!geometry) return;
    const SIZE work{monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top};
    const SIZE expected = lakefix::ClampToWorkArea(
        {minimumOuter.right - minimumOuter.left, minimumOuter.bottom - minimumOuter.top}, work);
    MINMAXINFO limits{};
    SendMessageW(frame, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
    Check(limits.ptMinTrackSize.x == expected.cx && limits.ptMinTrackSize.y == expected.cy,
          "scaled tracking minimum is configured client plus frame, capped to monitor work area");
    RECT area{}, outer{};
    const bool measured = GetClientRect(frame, &area) && GetWindowRect(frame, &outer);
    Check(measured, "measure hidden initial host without showing it or rebuilding graphics");
    if (!measured) return;
    Check(area.right > 0 && area.bottom > 0 && outer.right - outer.left <= work.cx &&
          outer.bottom - outer.top <= work.cy, "initial scaled host has pixels and fits the monitor work area");
    const SIZE actualBorders{outer.right - outer.left - area.right, outer.bottom - outer.top - area.bottom};
    const LONG borderWidth = borders.right - borders.left, borderHeight = borders.bottom - borders.top;
    const bool minimumFitsWidth = minimum.cx + borderWidth <= work.cx && minimum.cx + actualBorders.cx <= work.cx;
    const bool minimumFitsHeight = minimum.cy + borderHeight <= work.cy && minimum.cy + actualBorders.cy <= work.cy;
    Check((!minimumFitsWidth || area.right >= minimum.cx) && (!minimumFitsHeight || area.bottom >= minimum.cy),
          "initial host meets the configured minimum on every feasible axis");
    SIZE requested{static_cast<LONG>(GetPrivateProfileIntW(L"window", L"Width", 1024, configuration)),
                   static_cast<LONG>(GetPrivateProfileIntW(L"window", L"Height", 576, configuration))};
    if (requested.cx < 320 || requested.cx > 8192) requested.cx = 1024;
    if (requested.cy < 180 || requested.cy > 8192) requested.cy = 576;
    const SIZE initial = lakefix::AtLeastMinimum(requested, minimum);
    const bool initialFitsWidth = initial.cx + borderWidth <= work.cx && initial.cx + actualBorders.cx <= work.cx;
    const bool initialFitsHeight = initial.cy + borderHeight <= work.cy && initial.cy + actualBorders.cy <= work.cy;
    Check((!initialFitsWidth || area.right == initial.cx) && (!initialFitsHeight || area.bottom == initial.cy),
          "fitting initial dimensions retain max(window size, configured minimum)");
}

void RunScaledWindowPath(HINSTANCE instance, bool unicode, const wchar_t* configuration, bool policyOnly) {
    printf("Scaled %s creation/recreation path\n", unicode ? "Unicode" : "ANSI");
    // Deliberately request a different native size: [render], not the initial
    // CreateWindow dimensions or [window], owns the fixed renderer dimensions.
    HWND initialChild = nullptr;
    lake = unicode ? CreateWindowExW(WS_EX_TOPMOST, L"Lake Controller", L"Unicode scaled fixture",
                                     WS_POPUP | WS_SYSMENU, 0, 0, 1024, 768, nullptr, nullptr, instance, &initialChild)
                   : CreateWindowExA(WS_EX_TOPMOST, "Lake Controller", "ANSI scaled fixture",
                                      WS_POPUP | WS_SYSMENU, 0, 0, 1024, 768, nullptr, nullptr, instance, &initialChild);
    Check(lake != nullptr, unicode ? "Unicode CreateWindowEx path creates renderer" : "ANSI CreateWindowEx path creates renderer");
    if (!lake) return;
    Check(initialChild && GetParent(initialChild) == lake, "native child is created during main WM_CREATE before attachment");
    HWND frame = GetParent(lake);
    Check(frame != nullptr && frame != lake, "scaled renderer has a separate private host");
    if (!frame) {
        DestroyWindow(lake);
        lake = nullptr;
        Pump();
        return;
    }
    wchar_t className[128]{};
    GetClassNameW(frame, className, ARRAYSIZE(className));
    Check(wcscmp(className, L"LakeWindowedFix.ScaledHost.v1") == 0, "host uses private scaled class, not ContourParent");
    const DWORD hostStyle = static_cast<DWORD>(GetWindowLongW(frame, GWL_STYLE));
    Check((hostStyle & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW && !(hostStyle & (WS_POPUP | WS_CHILD)),
           "scaled host has caption/system menu/resize/minimize/maximize styles");
    Check(!(hostStyle & WS_CLIPCHILDREN), "scaled host allows DirectDraw clip region behind fixed child");
    Check((GetWindowLongW(lake, GWL_STYLE) & WS_CHILD) &&
          !(GetWindowLongW(lake, GWL_STYLE) & (WS_POPUP | WS_CAPTION)), "fixed renderer is a borderless child");
    Check(!(GetWindowLongW(lake, GWL_EXSTYLE) & WS_EX_TOPMOST), "scaled renderer is not topmost");
    Check(FixedScaledSource(lake, frame), "render INI creates fixed 1536x864 client at origin");
    RECT initialHost{};
    GetClientRect(frame, &initialHost);
    CheckScaledHostSizePolicy(frame, configuration);
    Check(!GetPropW(frame, L"LakeWindowedFix.HostSetupCompleted"), "host readiness starts deferred");
    if (policyOnly) {
        // The runner gives each policy its own process/INI. Never reload a live
        // shim policy or show extra windows just to test tracking constraints.
        DestroyWindow(frame);
        lake = nullptr;
        Pump();
        return;
    }
    Check(initialHost.right >= 320 && initialHost.bottom >= 180 && initialHost.right <= 1024 && initialHost.bottom <= 576 &&
          initialHost.right < 1536 && initialHost.bottom < 864, "host is born at fitting window size, smaller than source");
    const int initialSizeMessages = nativeClientSizes;
    SendMessageW(frame, WM_SIZE, SIZE_RESTORED, MAKELPARAM(800, 450));
    SendMessageW(frame, WM_SIZE, SIZE_MINIMIZED, 0);
    SendMessageW(frame, WM_SIZE, SIZE_RESTORED, 0);
    Pump();
    Check(!IsWindowVisible(frame) && !GetPropW(frame, L"LakeWindowedFix.HostSetupCompleted") &&
          resizes == 0 && nativeClientSizes == initialSizeMessages && nativeParentMessages == 0,
          "WM_SIZE and startup pumping before readiness never call native rebuild");

    const int startupsBefore = startupNotifications;
    const int earlyCloses = closeRequests;
    vetoClose = true;
    SendMessageW(frame, WM_CLOSE, 0, 0);
    Check(closeRequests == earlyCloses && IsWindow(lake), "close before startup is deferred, not destructive during native modal");
    Check(PostMessageW(lake, 0x8032, 0, 0) != FALSE, "queue native startup notification");
    ShowWindow(lake, SW_SHOWNORMAL);
    Check(IsWindowVisible(frame) && IsWindowVisible(lake) && resizes == 0 && nativeClientSizes == initialSizeMessages,
          "native ShowWindow reveals scaled host without native resize");
    Pump();
    Check(startupNotifications == startupsBefore + 1 && GetPropW(frame, L"LakeWindowedFix.HostSetupCompleted"),
           "native 0x8032 and plugin-style Pump complete host readiness");
    const HCURSOR previousCursor = GetCursor();
    const HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
    Check(SendMessageW(lake, WM_SETCURSOR, reinterpret_cast<WPARAM>(lake), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)) &&
          arrow && GetCursor() == arrow, "scaled client selects the shared OS arrow without waiting for a rendered frame");
    Check(SendMessageW(frame, WM_SETCURSOR, reinterpret_cast<WPARAM>(frame), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)) &&
          GetCursor() == arrow, "scaled host client area selects the same OS arrow");
    SetCursor(previousCursor); // Restore the test thread's prior cursor; never move the user's pointer.
    CheckChildCursor(initialChild, "initial WM_CREATE child selects OS arrow before its cursor-hiding native handler");
    SendMessageW(frame, WM_ACTIVATEAPP, FALSE, 0);
    SendMessageW(frame, WM_ACTIVATEAPP, TRUE, 0);
    Check(IsWindow(lake) && FixedScaledSource(lake, frame) && resizes == 0 && nativeParentMessages == 0,
          "host deactivation/reactivation preserves canvas and native lifecycle");
    Check(closeRequests == earlyCloses + 1 && IsWindow(lake), "deferred startup close reaches native handler and respects veto once ready");
    vetoClose = false;
    RECT readyHost{};
    GetClientRect(frame, &readyHost);
    Check(SameRectangle(initialHost, readyHost) && FixedScaledSource(lake, frame),
          "deferred setup changes placement only, not host or source size");
    Check(resizes == 0 && nativeClientSizes == initialSizeMessages && nativeParentMessages == 0,
          "readiness never calls ContourParent or native renderer WM_SIZE");

    ResizeScaledHost(lake, frame, 800, 450, false, initialSizeMessages);
    ResizeScaledHost(lake, frame, 400, 300, true, initialSizeMessages);
    TestScaledMessagePoints(lake, frame);

    SendMessageW(frame, WM_ENTERSIZEMOVE, 0, 0);
    SendMessageW(frame, WM_SIZE, SIZE_RESTORED, MAKELPARAM(900, 650));
    SendMessageW(frame, WM_SIZE, SIZE_RESTORED, MAKELPARAM(950, 700));
    SendMessageW(frame, WM_EXITSIZEMOVE, 0, 0);
    SendMessageW(frame, WM_SIZE, SIZE_MINIMIZED, 0);
    SendMessageW(frame, WM_SIZE, SIZE_RESTORED, 0);
    Check(resizes == 0 && nativeClientSizes == initialSizeMessages && nativeParentMessages == 0 && FixedScaledSource(lake, frame),
          "post-ready drag/minimize/empty WM_SIZE never invokes native rebuild");
    SetWindowLongW(lake, GWL_STYLE, WS_POPUP | WS_CAPTION);
    Check((GetWindowLongW(lake, GWL_STYLE) & WS_CHILD) && !(GetWindowLongW(lake, GWL_STYLE) & (WS_POPUP | WS_CAPTION)),
          "scaled renderer style guard preserves borderless child");
    SetWindowPos(lake, HWND_TOPMOST, 20, 30, 100, 100, SWP_NOACTIVATE);
    Check(FixedScaledSource(lake, frame) && !(GetWindowLongW(lake, GWL_EXSTYLE) & WS_EX_TOPMOST),
          "native SetWindowPos cannot move/resize/promote fixed source");
    MoveWindow(lake, 20, 30, 320, 200, FALSE);
    Check(FixedScaledSource(lake, frame), "native MoveWindow cannot move/resize fixed source");
    ShowWindow(lake, SW_HIDE);
    Check(!IsWindowVisible(frame), "native hide hides scaled host");
    ShowWindow(lake, SW_SHOW);
    Check(IsWindowVisible(frame) && IsWindowVisible(lake), "native show restores scaled host and source");
    ShowWindow(lake, SW_MINIMIZE);
    Check(IsIconic(frame) && !IsIconic(lake), "native minimize targets scaled host only");
    ShowWindow(lake, SW_RESTORE);
    Pump();
    Check(!IsIconic(frame) && IsWindowVisible(lake) && FixedScaledSource(lake, frame),
          "native restore preserves visible fixed source");
    Check(resizes == 0 && nativeClientSizes == initialSizeMessages && nativeParentMessages == 0,
          "native geometry and visibility requests never rebuild source");

    const int closesBefore = closeRequests;
    vetoClose = true;
    SendMessageW(frame, WM_CLOSE, 0, 0);
    Check(IsWindow(lake) && IsWindow(frame) && closeRequests == closesBefore + 1, "scaled host close respects native veto");
    vetoClose = false;
    destroyQuitCode = 23;
    lastPumpedQuit = -1;
    SendMessageW(frame, WM_CLOSE, 0, 0);
    Pump();
    Check(!IsWindow(lake) && !IsWindow(frame) && !IsWindow(initialChild) && closeRequests == closesBefore + 2,
          "scaled close runs native cleanup before destroying host and cursor-subclassed child");
    Check(lastPumpedQuit == 23, "scaled cleanup preserves native WM_DESTROY quit code 23");
    // Bounded cleanup of this test's own windows if a close assertion failed.
    if (IsWindow(lake)) DestroyWindow(lake);
    if (IsWindow(frame)) DestroyWindow(frame);
    lake = nullptr;
    Pump();
    destroyQuitCode = -1;
}

int RunScaledFixture(const wchar_t* configuration) {
    printf("Fixture ScaleRendering=1 NativeResize=0 (synthetic Win32 only; no pixel/GPU validation)\n");
    Check(sizeof(void*) == 4, "x86 scaled fixture");
    Check(GetPrivateProfileIntW(L"render", L"Width", 0, configuration) == 1536 &&
          GetPrivateProfileIntW(L"render", L"Height", 0, configuration) == 864, "scaled suite has independent render configuration");
    TestScaledWindowSizePolicy();
    const bool policyOnly = GetPrivateProfileIntW(L"fixture", L"WindowPolicyOnly", 0, configuration) == 1;
    HMODULE fontModule = LoadLibraryW(L"Contour.dll");
    Check(fontModule != nullptr, "load synthetic renderer for module-scoped font hook");
    if (fontModule) {
        FARPROC address = GetProcAddress(fontModule, "FixtureCreateFont");
        if (!address) address = GetProcAddress(fontModule, "_FixtureCreateFont");
        Check(address != nullptr, "synthetic font factory exported");
        if (address) {
            using Factory = HFONT (__cdecl*)(const LOGFONTA*);
            const auto factory = reinterpret_cast<Factory>(reinterpret_cast<void*>(address));
            LOGFONTA request{}, observed{};
            request.lfHeight = -13;
            request.lfWeight = FW_NORMAL;
            request.lfQuality = NONANTIALIASED_QUALITY;
            strcpy_s(request.lfFaceName, "Arial");
            const LOGFONTA original = request;
            HFONT font = factory(&request);
            Check(font && GetObjectA(font, sizeof(observed), &observed) == sizeof(observed) &&
                  observed.lfQuality == ANTIALIASED_QUALITY && observed.lfHeight == request.lfHeight &&
                  observed.lfWeight == request.lfWeight, "renderer font factory receives grayscale AA with requested size/weight intact");
            Check(memcmp(&request, &original, sizeof(request)) == 0, "font hook never writes caller LOGFONT storage");
            if (font) DeleteObject(font);
            font = CreateFontIndirectA(&request);
            Check(font && GetObjectA(font, sizeof(observed), &observed) == sizeof(observed) &&
                  observed.lfQuality == NONANTIALIASED_QUALITY, "font calls outside renderer retain original quality");
            if (font) DeleteObject(font);
            strcpy_s(request.lfFaceName, "Small Fonts");
            font = factory(&request);
            Check(font && GetObjectA(font, sizeof(observed), &observed) == sizeof(observed) &&
                  observed.lfQuality == NONANTIALIASED_QUALITY, "raster/LCD faces are not silently replaced");
            if (font) DeleteObject(font);
        }
    }
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW childClass{};
    childClass.hInstance = instance;
    childClass.lpfnWndProc = InputChildProc;
    childClass.lpszClassName = L"LakeFix.InputChild";
    Check(RegisterClassW(&childClass) != 0, "register distinct native input-child class once for both creation paths");
    WNDCLASSEXA ansi{};
    ansi.cbSize = sizeof(ansi);
    ansi.hInstance = instance;
    ansi.lpfnWndProc = ParentProc;
    ansi.lpszClassName = "ContourParent";
    Check(RegisterClassExA(&ansi) != 0, "register native parent only as a rebuild tripwire");
    ansi.lpfnWndProc = LakeProc;
    ansi.lpszClassName = "Lake Controller";
    const bool ansiRegistered = RegisterClassExA(&ansi) != 0;
    Check(ansiRegistered, "register ANSI native client fixture");
    if (ansiRegistered) RunScaledWindowPath(instance, false, configuration, policyOnly);
    const bool unregistered = ansiRegistered && UnregisterClassA("Lake Controller", instance) != FALSE;
    Check(unregistered, "ANSI client class can be replaced after complete cleanup");
    if (unregistered) {
        WNDCLASSEXW wide{};
        wide.cbSize = sizeof(wide);
        wide.hInstance = instance;
        wide.lpfnWndProc = UnicodeLakeProc;
        wide.lpszClassName = L"Lake Controller";
        const bool wideRegistered = RegisterClassExW(&wide) != 0;
        Check(wideRegistered, "register genuine Unicode native client fixture");
        if (wideRegistered) {
            RunScaledWindowPath(instance, true, configuration, policyOnly);
            UnregisterClassW(wide.lpszClassName, instance);
        }
    }
    Check(nativeParentMessages == 0 && resizes == 0, "scaled suite never enters native parent procedure");
    UnregisterClassA("ContourParent", instance);
    UnregisterClassW(childClass.lpszClassName, childClass.hInstance);
    if (fontModule) FreeLibrary(fontModule);
    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
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
    const bool scaleRendering = GetPrivateProfileIntW(L"compatibility", L"ScaleRendering", 1, configuration) == 1;
    if (scaleRendering && !nativeResize) return RunScaledFixture(configuration);
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
    RECT minimum{0, 0, lakefix::kMinWidth, lakefix::kMinHeight};
    const bool minimumAdjusted = AdjustWindowRectEx(&minimum, static_cast<DWORD>(GetWindowLongW(frame, GWL_STYLE)),
                                                    FALSE, static_cast<DWORD>(GetWindowLongW(frame, GWL_EXSTYLE))) != FALSE;
    Check(minimumAdjusted && limits.ptMinTrackSize.x == minimum.right - minimum.left &&
          limits.ptMinTrackSize.y == minimum.bottom - minimum.top,
          "legacy 800x600 client minimum includes frame borders and ignores scaled minimum keys");
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
