#pragma once

#include <windows.h>

namespace lakefix {
constexpr int kMinWidth = 800;
constexpr int kMinHeight = 600;

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
