#pragma once

#include <windows.h>
#include <cstdint>
#include <limits>

namespace lakefix {

// Pixel rectangles use exclusive right/bottom edges. Integer fitting rounds
// the non-limiting dimension down, except that a valid image occupies at least
// one pixel on each axis. An odd spare pixel belongs to the right/bottom bar.
inline RECT FitViewport(LONG viewportWidth, LONG viewportHeight,
                        LONG sourceWidth, LONG sourceHeight) noexcept {
    if (viewportWidth <= 0 || viewportHeight <= 0 || sourceWidth <= 0 || sourceHeight <= 0)
        return {};

    const std::int64_t vw = viewportWidth;
    const std::int64_t vh = viewportHeight;
    const std::int64_t sw = sourceWidth;
    const std::int64_t sh = sourceHeight;
    std::int64_t width = vw;
    std::int64_t height = vh;
    // Positive LONG products fit in signed 64 bits, including LONG_MAX pairs.
    if (vw * sh <= vh * sw) {
        height = vw * sh / sw;
        if (height < 1) height = 1;
    } else {
        width = vh * sw / sh;
        if (width < 1) width = 1;
    }
    const std::int64_t left = (vw - width) / 2;
    const std::int64_t top = (vh - height) / 2;
    // All four edges remain within [0, viewport dimension], before narrowing.
    return {static_cast<LONG>(left), static_cast<LONG>(top),
            static_cast<LONG>(left + width), static_cast<LONG>(top + height)};
}

inline bool ViewportContains(const RECT& viewport, POINT point) noexcept {
    return viewport.right > viewport.left && viewport.bottom > viewport.top &&
           point.x >= viewport.left && point.x < viewport.right &&
           point.y >= viewport.top && point.y < viewport.bottom;
}

// Map pixel coordinates using floor(offset * destination / source), not
// rounded pixel centers. Letterbox bars and invalid sizes return (-1, -1).
inline POINT ViewToSource(const RECT& viewport, SIZE source, POINT point) noexcept {
    if (source.cx <= 0 || source.cy <= 0 || !ViewportContains(viewport, point))
        return {-1, -1};

    // Widen before subtracting: callers may supply arbitrary LONG edges,
    // including a rectangle spanning LONG_MIN through LONG_MAX.
    const std::int64_t width = static_cast<std::int64_t>(viewport.right) - viewport.left;
    const std::int64_t height = static_cast<std::int64_t>(viewport.bottom) - viewport.top;
    const std::int64_t x = static_cast<std::int64_t>(point.x) - viewport.left;
    const std::int64_t y = static_cast<std::int64_t>(point.y) - viewport.top;
    // A contained offset is at most 2^32-2; multiplying it by a positive
    // LONG (at most 2^31-1) is still below INT64_MAX.
    return {static_cast<LONG>(x * source.cx / width),
            static_cast<LONG>(y * source.cy / height)};
}

// A captured interaction already has a recipient. Keep the same affine map
// beyond every edge, independently per axis; do not turn a release/drag into a
// fresh hit-test or clamp it to an unrelated control. Floor extends the inside
// convention continuously to negative coordinates. Saturation is only for LONG
// representability, never to the image bounds.
inline POINT ViewToSourceCaptured(const RECT& viewport, SIZE source, POINT point) noexcept {
    if (source.cx <= 0 || source.cy <= 0 || viewport.right <= viewport.left || viewport.bottom <= viewport.top)
        return {-1, -1};
    const auto axis = [](LONG coordinate, LONG edge, LONG extent, std::int64_t view) noexcept {
        const std::int64_t product = (static_cast<std::int64_t>(coordinate) - edge) * extent;
        const std::int64_t mapped = product / view - (product < 0 && product % view != 0 ? 1 : 0);
        const LONG low = (std::numeric_limits<LONG>::min)();
        const LONG high = (std::numeric_limits<LONG>::max)();
        return mapped < low ? low : mapped > high ? high : static_cast<LONG>(mapped);
    };
    return {axis(point.x, viewport.left, source.cx, static_cast<std::int64_t>(viewport.right) - viewport.left),
            axis(point.y, viewport.top, source.cy, static_cast<std::int64_t>(viewport.bottom) - viewport.top)};
}

// The reverse mapping uses the same floor/leading-edge convention. Scaling
// discards pixels, so a round trip is not generally an identity. Invalid
// rectangles, sizes or source coordinates return (-1, -1).
inline POINT SourceToView(const RECT& viewport, SIZE source, POINT point) noexcept {
    if (source.cx <= 0 || source.cy <= 0 || viewport.right <= viewport.left ||
        viewport.bottom <= viewport.top || point.x < 0 || point.y < 0 ||
        point.x >= source.cx || point.y >= source.cy)
        return {-1, -1};

    const std::int64_t width = static_cast<std::int64_t>(viewport.right) - viewport.left;
    const std::int64_t height = static_cast<std::int64_t>(viewport.bottom) - viewport.top;
    const std::int64_t x = static_cast<std::int64_t>(viewport.left) +
                           static_cast<std::int64_t>(point.x) * width / source.cx;
    const std::int64_t y = static_cast<std::int64_t>(viewport.top) +
                           static_cast<std::int64_t>(point.y) * height / source.cy;
    // Each mapped coordinate lies inside its original LONG rectangle.
    return {static_cast<LONG>(x), static_cast<LONG>(y)};
}

// Preserve the real starting pixel for GetCursorPos -> offset -> SetCursorPos.
// Absolute inverse-floor mapping would drift even for a zero offset. Signed
// deltas truncate toward zero and results saturate at the Win32 LONG bounds.
inline POINT TranslateCursorDelta(POINT real, POINT previousVirtual, POINT requestedVirtual,
                                  SIZE view, SIZE source) noexcept {
    if (view.cx <= 0 || view.cy <= 0 || source.cx <= 0 || source.cy <= 0) return real;
    const auto axis = [](LONG start, LONG before, LONG requested, LONG to, LONG from) noexcept {
        const std::int64_t delta = static_cast<std::int64_t>(requested) - before;
        const std::int64_t result = static_cast<std::int64_t>(start) + delta * to / from;
        const LONG low = (std::numeric_limits<LONG>::min)();
        const LONG high = (std::numeric_limits<LONG>::max)();
        return result < low ? low : result > high ? high : static_cast<LONG>(result);
    };
    return {axis(real.x, previousVirtual.x, requestedVirtual.x, view.cx, source.cx),
            axis(real.y, previousVirtual.y, requestedVirtual.y, view.cy, source.cy)};
}
} // namespace lakefix
