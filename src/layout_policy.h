#pragma once
#include <windows.h>
#include <cstdint>

namespace lakefix {
inline bool LayoutMemoryAdmitted(const MEMORY_BASIC_INFORMATION& memory, std::uintptr_t imageBase,
                                 std::uintptr_t imageBytes, std::uintptr_t rva) noexcept {
    constexpr std::uintptr_t count = 2 * sizeof(LONG);
    if (!imageBase || rva > imageBytes || imageBytes - rva < count || rva > UINTPTR_MAX - imageBase) return false;
    const std::uintptr_t start = imageBase + rva;
    const auto region = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    return memory.State == MEM_COMMIT && memory.Type == MEM_IMAGE && memory.AllocationBase == reinterpret_cast<void*>(imageBase) &&
           (memory.Protect & 0xff) == PAGE_READWRITE && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
           region <= start && memory.RegionSize >= count && start - region <= memory.RegionSize - count;
}
inline bool LayoutValuesAdmitted(LONG width, LONG height) noexcept {
    return width > 0 && height > 0 && width <= 32768 && height <= 32768;
}
// For an otherwise eligible full presentation, lack of layout admission is an
// explicit shim failure, NOT a provider decline/native auxiliary bypass.
inline HRESULT ScaledLayoutAdmission(bool ownCanvas, bool layoutAdmitted) noexcept {
    return !ownCanvas ? E_INVALIDARG : layoutAdmitted ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
}
} // namespace lakefix
