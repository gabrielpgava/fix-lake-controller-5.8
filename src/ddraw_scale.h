#pragma once

#include <windows.h>

namespace lakefix {

// On true, both rectangles are screen coordinates. fullClientScreen must be
// the presentation host's actual client area; fittedScreen must lie inside it.
// On false, rectangles/flags/effects are unchanged; COM proxy arguments still
// translate to their real interfaces for the native invocation.
// A failing native letterbox Blt may use the optional owned-window painter.
// If that also fails, return the native fill error; otherwise return the real
// image-presentation HRESULT. A later call retries failed geometry.
using ScaleViewportProvider = bool (*)(HWND window, LONG sourceWidth, LONG sourceHeight,
                                       RECT* fullClientScreen, RECT* fittedScreen) noexcept;
using ScalePresenterLog = void (*)(const wchar_t*) noexcept;
// Optional visible clipping HWND for a fixed-canvas child. nullptr declines.
using ScaleClipWindowProvider = HWND (*)(HWND cooperativeWindow) noexcept;
// Optional owned-window fallback when native primary COLORFILL is unsupported.
using ScaleLetterboxPainter = bool (*)(HWND, const RECT*, UINT) noexcept;

// Callbacks run without the registry lock. The logger must itself be best-effort.
void ConfigureScalePresenter(ScaleViewportProvider, ScalePresenterLog,
                             ScaleClipWindowProvider = nullptr, ScaleLetterboxPainter = nullptr) noexcept;

// Windows DirectDraw rejects modification of its native vtable identity. This
// bridge uses its OWN complete forwarding COM interfaces; native tables and
// native object pointers are never modified. The input must own one genuine
// IDirectDraw7 reference. On success that reference transfers to the proxy and
// *object is replaced; on failure *object and its reference remain untouched.
// Calling this again on the same owned proxy reference is a no-op.
// Fail-open bounds: 64 DD7 proxies and 256 primary-surface proxies.
// Other QI interfaces, offscreen surfaces, enumerated/attached aliases and
// GetDDInterface outputs remain native. This is not universal COM identity
// emulation. Direct interface arguments are unwrapped; BltBatch arrays and
// interface pointers embedded in effect structures are passed through and
// must contain native interfaces. Only full-source Blt is scaled, not Flip.
bool WrapDirectDraw7(void** object) noexcept;

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
