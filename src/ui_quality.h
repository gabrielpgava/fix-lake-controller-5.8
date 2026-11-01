#pragma once

#include <windows.h>
#include <cstdint>
#include <limits>

namespace lakefix {

inline bool FontFaceEquals(const char (&face)[LF_FACESIZE], const char* expected) noexcept {
    for (unsigned i = 0; i < LF_FACESIZE; ++i) {
        const unsigned char a = static_cast<unsigned char>(face[i]);
        const unsigned char b = static_cast<unsigned char>(expected[i]);
        const auto lower = [](unsigned char c) noexcept {
            return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
        };
        if (lower(a) != lower(b)) return false;
        if (!a) return true;
    }
    return false;
}

// Only the scalable UI faces observed in Build 007. Keep size, weight, spacing,
// face and every other field. Grayscale AA avoids ClearType color fringes when
// the resulting bitmap is subsequently stretched. Do not replace LCD/raster fonts.
inline bool PrepareSmoothFont(const LOGFONTA& input, LOGFONTA& output) noexcept {
    if ((!FontFaceEquals(input.lfFaceName, "Arial") &&
         !FontFaceEquals(input.lfFaceName, "Courier New")) || input.lfQuality == ANTIALIASED_QUALITY)
        return false;
    output = input;
    output.lfQuality = ANTIALIASED_QUALITY;
    return true;
}

// Return addresses of the three bitmap-cursor-only MapWindowPoints calls in
// admitted Build 007. Input routing and ordinary coordinate mappings are not
// changed. These are RVAs, not process addresses or generic API callsites.
inline bool IsCursorPaintReturn(ULONG_PTR rva) noexcept {
    return rva == 0x283fb || rva == 0x2a453 || rva == 0x2a4e2;
}

// Build 007's frame and private-dialog ShowCursor normalization loops. Only
// these return RVAs may use a logical count instead of hiding the OS arrow.
inline bool IsCursorVisibilityReturnRva(ULONG_PTR rva) noexcept {
    return rva == 0x11d76 || rva == 0x11d84 || rva == 0x6465d || rva == 0x6466a;
}

// The caller must first establish renderer identity; use RVA zero otherwise.
inline bool ShouldVirtualizeCursorVisibility(bool systemCursorActive, bool hasClient,
                                            bool owningUiThread, ULONG_PTR rva) noexcept {
    return systemCursorActive && hasClient && owningUiThread && IsCursorVisibilityReturnRva(rva);
}

struct VirtualCursorVisibility {
    bool initialized = false;
    int count = 0;

    template<class RealAdjust>
    int Adjust(BOOL show, unsigned credits, RealAdjust real) noexcept {
        constexpr int low = (std::numeric_limits<int>::min)();
        constexpr int high = (std::numeric_limits<int>::max)();
        if (!initialized) {
            // One balanced probe, through the real API. Remove only our own
            // visibility credits to seed the legacy, pre-compensation view.
            real(TRUE);
            const std::int64_t initial = static_cast<std::int64_t>(real(FALSE)) -
                                         static_cast<std::int64_t>(credits);
            count = initial < low ? low : initial > high ? high : static_cast<int>(initial);
            initialized = true;
        }
        // Saturate rather than overflow. Subsequent legacy normalization never
        // touches the OS counter, even after thousands of frames or dialogs.
        if (show) {
            if (count < high) ++count;
        } else if (count > low) --count;
        return count;
    }
};

// Testable ShowCursor counter bookkeeping. An already-visible counter is left
// unchanged, and at most 16 of OUR increments can remain outstanding.
template<class Adjust>
inline void AcquireCursorVisibility(unsigned& credits, Adjust adjust) noexcept {
    while (credits < 16) {
        const int count = adjust(TRUE);
        if (count > 0) {
            adjust(FALSE);
            break;
        }
        ++credits;
        if (count == 0) break;
    }
}

template<class Adjust>
inline void ReleaseCursorVisibility(unsigned& credits, Adjust adjust) noexcept {
    while (credits) {
        adjust(FALSE);
        --credits;
    }
}

struct CursorVisibilitySample {
    CURSORINFO info{};
    bool queried = false;
    bool adjusted = false;
};

// One call-local observation only. Never reuse after returning to the message
// pump. Ordinary WM_SETCURSOR callers do not need a second visibility query.
template<class Query, class Adjust>
inline CursorVisibilitySample EnsureCursorVisibility(unsigned& credits, Query query, Adjust adjust) noexcept {
    const DWORD saved = GetLastError();
    CursorVisibilitySample sample;
    sample.info.cbSize = sizeof(sample.info);
    sample.queried = query(&sample.info) != FALSE;
    if (sample.queried && !(sample.info.flags & CURSOR_SHOWING)) {
        sample.adjusted = true; // Also recheck when the bounded credit cap is hit.
        AcquireCursorVisibility(credits, adjust);
    }
    SetLastError(saved);
    return sample;
}

template<class Query>
inline bool ConfirmSystemCursor(const CursorVisibilitySample& sample, HCURSOR expected, Query query) noexcept {
    if (!expected) return false;
    if (sample.queried && !sample.adjusted && (sample.info.flags & CURSOR_SHOWING) &&
        sample.info.hCursor == expected) return true;
    // Failed/suppressed/unexpected samples and every recovery attempt retain
    // the original post-recovery verification before hiding the native sprite.
    const DWORD saved = GetLastError();
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    const bool visible = query(&info) && (info.flags & CURSOR_SHOWING) && info.hCursor == expected;
    SetLastError(saved);
    return visible;
}

// Keep a valid sprite rectangle, but outside the fixed canvas. The native
// drawing helper still runs and clips it; no COM HRESULT is invented. Decline
// unexpected or large rectangles rather than risking an unbounded conversion.
inline bool HideSoftwareCursorRectangle(POINT* points, UINT count) noexcept {
    if (!points || count != 2) return false;
    const std::int64_t width = static_cast<std::int64_t>(points[1].x) - points[0].x;
    const std::int64_t height = static_cast<std::int64_t>(points[1].y) - points[0].y;
    if (width <= 0 || height <= 0 || width > 512 || height > 512) return false;
    points[0] = {-16384, -16384};
    points[1] = {-16384 + static_cast<LONG>(width), -16384 + static_cast<LONG>(height)};
    return true;
}

} // namespace lakefix
