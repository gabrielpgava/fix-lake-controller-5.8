#pragma once

#include <windows.h>

namespace lakefix {
constexpr int kMinWidth = 800;
constexpr int kMinHeight = 600;
constexpr int kScaledMinWidth = 800;
constexpr int kScaledMinHeight = 450;

// Scaled window sizes are logical Win32 client pixels, not canvas dimensions.
// GetPrivateProfileInt returns UINT even for negative input; reject each axis
// independently before narrowing or doing frame/work-area arithmetic.
inline SIZE ScaledMinimum(UINT width, UINT height) noexcept {
    return {width >= 320 && width <= 8192 ? static_cast<LONG>(width) : kScaledMinWidth,
            height >= 180 && height <= 8192 ? static_cast<LONG>(height) : kScaledMinHeight};
}

inline SIZE AtLeastMinimum(SIZE requested, SIZE minimum) noexcept {
    return {requested.cx < minimum.cx ? minimum.cx : requested.cx,
            requested.cy < minimum.cy ? minimum.cy : requested.cy};
}

// Only a known positive work-area axis can cap a requested outer window size.
inline SIZE ClampToWorkArea(SIZE requested, SIZE work) noexcept {
    return {work.cx > 0 && requested.cx > work.cx ? work.cx : requested.cx,
            work.cy > 0 && requested.cy > work.cy ? work.cy : requested.cy};
}

inline DWORD HostStyle(DWORD style) {
    return (style & ~(WS_POPUP | WS_CHILD)) | WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
}

inline DWORD ClientStyle(DWORD style) {
    return (style & ~(WS_POPUP | WS_OVERLAPPEDWINDOW)) | WS_CHILD;
}

inline DWORD WindowExStyle(DWORD style) {
    return style & ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
}

inline bool HasPixels(WPARAM type, LPARAM size) {
    return type != SIZE_MINIMIZED && type != SIZE_MAXHIDE &&
           LOWORD(size) != 0 && HIWORD(size) != 0;
}
} // namespace lakefix
