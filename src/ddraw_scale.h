#pragma once

#include <windows.h>

namespace lakefix {

// QUERY ONLY: do not publish input/source geometry or presentation success here.
// On true, both rectangles are screen coordinates. fullClientScreen must be
// the presentation host's actual client area; fittedScreen must lie inside it.
// On false, rectangles/flags/effects are unchanged; COM proxy arguments still
// translate to their real interfaces for the native invocation.
// A failing native letterbox Blt may use the optional owned-window painter.
// If that also fails, return the native fill error; otherwise return the real
// image-presentation HRESULT. A later call retries failed geometry.
using ScaleViewportProvider = bool (*)(HWND window, LONG sourceWidth, LONG sourceHeight,
                                        RECT* fullClientScreen, RECT* fittedScreen) noexcept;
// Called only after an admitted image Blt succeeds, using the same callback
// snapshot/rectangles as the query. Never called for fills, bypasses or failures.
// This acknowledges the native HRESULT, not GPU completion/displayed pixels.
using ScalePresentationCommit = void (*)(HWND window, LONG sourceWidth, LONG sourceHeight,
                                         const RECT* fullClientScreen, const RECT* fittedScreen) noexcept;
// Optional fail-closed prerequisite for a complete frame already classified by
// native source role/dimensions, destination extent and flags. Runs before the
// viewport query, without presenter locks, from the same callback snapshot.
// FAILED returns that exact HRESULT without a native Blt, query or commit and
// preserves caller LastError. Success does not replace the eventual native
// HRESULT/LastError. nullptr admits (standalone/low-level use); production must
// supply its layout/startup guard rather than encode fatal refusal as query=false.
using ScalePresentationAdmission = HRESULT (*)(HWND window) noexcept;
using ScalePresenterLog = void (*)(const wchar_t*) noexcept;
// Optional visible clipping HWND for a fixed-canvas child. nullptr declines.
using ScaleClipWindowProvider = HWND (*)(HWND cooperativeWindow) noexcept;
// Optional owned-window fallback when native primary COLORFILL is unsupported.
using ScaleLetterboxPainter = bool (*)(HWND, const RECT*, UINT) noexcept;

// Callbacks run without the registry lock. The logger must itself be best-effort.
void ConfigureScalePresenter(ScaleViewportProvider, ScalePresenterLog,
                              ScaleClipWindowProvider = nullptr, ScaleLetterboxPainter = nullptr,
                              LONG expectedSourceWidth = 0, LONG expectedSourceHeight = 0,
                              ScalePresentationCommit = nullptr,
                              ScalePresentationAdmission = nullptr) noexcept;

// Windows DirectDraw rejects modification of its native vtable identity. This
// bridge uses its OWN complete forwarding COM interfaces; native tables and
// native object pointers are never modified. The input must own one genuine
// IDirectDraw7 reference. On success that reference transfers to the proxy and
// *object is replaced; on failure *object and its reference remain untouched.
// Calling this again on the same owned proxy reference is a no-op.
// Hard bounds: 64 DD7 proxies and 256 primary-surface proxies.
// Other QI interfaces, offscreen surfaces, enumerated/attached aliases and
// GetDDInterface outputs remain native. This is not universal COM identity
// emulation. Direct interface arguments are unwrapped; BltBatch arrays and
// interface pointers embedded in effect structures are passed through and
// must contain native interfaces. Scaling requires configured positive expected
// dimensions, a native OFFSCREENPLAIN source description with exactly that size,
// and a primary destination rectangle with the full source extent. Null/partial
// destinations, non-null sourceRect, texture/primary/backbuffer/unknown roles and
// unsupported Blt operations remain native. No capabilities are fabricated.
// This low-level bool API does NOT consume ownership on failure: it must not be
// used as fail-open scaled creation. Use the finalizer below at the factory hook.
bool WrapDirectDraw7(void** object) noexcept;

// Call only for a scaled IDirectDraw7 factory result. Preserves a native failure
// unchanged. On success transfers ownership into a proxy; on wrap failure releases
// the returned native reference exactly once, clears *object and returns a failing
// HRESULT. Keeps the factory's LastError even across cleanup COM callbacks.
// Primary CreateSurface follows the same fail-closed rule automatically.
HRESULT CompleteScaledDirectDrawCreation(HRESULT nativeResult, void** object) noexcept;

// Resource admission is shared by production and tests, not a test-only success
// bypass. Limits can only reduce the hard bounds. allocationBudget counts future
// allocation attempts (owner and proxy); ~0UL means unlimited. Changing limits
// never retires live objects. Normal integration need not call this function.
struct ScalePresenterResources {
    unsigned long drawCapacity = 64;
    unsigned long primaryCapacity = 256;
    unsigned long allocationBudget = ~0UL;
};
void ConfigureScalePresenterResources(const ScalePresenterResources&) noexcept;

// Fixed diagnostic table: at most 96 (operation,HRESULT) pairs, four messages per
// pair over process lifetime (failure/repetition/recovery combined), no eviction.
// Counts saturate; excess pairs are counted but not logged. Configure does not
// reset diagnostic budgets. Callbacks/COM never run under a presenter lock.
struct ScalePresenterDiagnostics {
    unsigned long trackedPairs;
    unsigned long failures;
    unsigned long recoveries;
    unsigned long suppressedMessages; // Eligible notifications omitted after a pair's budget was spent.
    unsigned long untrackedFailures;
};
ScalePresenterDiagnostics GetScalePresenterDiagnostics() noexcept;

// Registry counts exclude retired proxies. liveProxyObjects includes retired
// proxies pinned by in-flight calls. Two complete shared proxy tables are static;
// there are no per-native-object vtable clones. Native ULONG counts do not govern
// proxy lifetime: API references and active metadata pins are counted separately.
struct ScalePresenterStats {
    unsigned long directDrawObjects;
    unsigned long primarySurfaces;
    unsigned long liveProxyObjects;
    unsigned long ownerContexts;
};
ScalePresenterStats GetScalePresenterStats() noexcept;

} // namespace lakefix
