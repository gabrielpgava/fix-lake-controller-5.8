#pragma once

#include <cstdint>
#include <cstddef>

// The integrator supplies the same source-controlled identity to both builds.
// There is deliberately no implicit version/build accepted by the launcher.
#if __has_include("product_version.h")
#include "product_version.h"
#endif
#ifndef LAKE_FIX_VERSION
#define LAKE_FIX_VERSION ""
#endif
#ifndef LAKE_FIX_BUILD_ID
#define LAKE_FIX_BUILD_ID ""
#endif
#ifndef LAKE_FIX_ABI_VERSION
#define LAKE_FIX_ABI_VERSION 2
#endif
#ifndef LAKE_FIX_COMPONENT_BUILD_ID
#ifdef LAKE_FIX_TESTING
#define LAKE_FIX_COMPONENT_BUILD_ID LAKE_FIX_BUILD_ID "-fixture"
#else
#define LAKE_FIX_COMPONENT_BUILD_ID LAKE_FIX_BUILD_ID
#endif
#endif

namespace lakefix {
constexpr std::uint32_t kComponentMagic = 0x58464b4c; // LKFX, little endian
constexpr std::uint32_t kComponentAbi = LAKE_FIX_ABI_VERSION;
constexpr char kQueryContractExport[] = "LakeWindowedQueryContract";
constexpr char kInitializeExport[] = "LakeWindowedInitialize";

// Wire format: no pointers, bools, compiler enums or platform-sized fields.
// Export (undecorated, including on x86):
//   extern "C" DWORD WINAPI LakeWindowedQueryContract(void* output);
// output points to this writable structure in the DLL's own process. Caller
// sets size and zeroes the rest. Reject a different size with
// ERROR_REVISION_MISMATCH; otherwise copy ExpectedComponentContract() and return
// ERROR_SUCCESS. Query must not initialize hooks, logging or other state.
// DllMain must remain inert: the query runs AFTER LoadLibraryW, BEFORE init.
// LakeWindowedInitialize retains its DWORD WINAPI(void*) ABI but its argument
// is now a required InitializeRequest (defined below), not nullptr.
struct ComponentContract {
    std::uint32_t size;
    std::uint32_t magic;
    std::uint32_t abi;
    std::uint32_t machine;
    std::uint32_t pointerBytes;
    std::uint32_t flags; // Reserved: zero in ABI 2.
    char productVersion[32]; // NUL terminated AND zero padded.
    char buildId[64];        // NUL terminated AND zero padded; include build flavor.
    std::uint32_t reserved[4];
};
static_assert(sizeof(ComponentContract) == 136, "Component ABI layout changed");
static_assert(offsetof(ComponentContract, productVersion) == 24, "Component ABI alignment changed");
static_assert(offsetof(ComponentContract, reserved) == 120, "Component ABI alignment changed");

struct ComponentFileIdentity {
    std::uint32_t volumeSerialNumber;
    std::uint32_t fileIndexHigh;
    std::uint32_t fileIndexLow;
    std::uint32_t fileSizeHigh;
    std::uint32_t fileSizeLow;
    std::uint32_t lastWriteTimeHigh;
    std::uint32_t lastWriteTimeLow;
};
static_assert(sizeof(ComponentFileIdentity) == 28, "File identity ABI changed");

// The launcher retains the verified files with FILE_SHARE_READ ONLY through
// init. DLL must copy/validate this block before returning (remote buffer is
// then freed), open its own read-only/no-write/no-delete Contour handle, and
// retain that handle for later LoadLibrary hooks. Compare the actual module's
// canonical path + file identity + admitted hash BEFORE using renderer RVAs.
// Never use this request as a runtime hash allowlist/bypass in a production DLL.
struct InitializeRequest {
    std::uint32_t size;
    std::uint32_t abi;
    ComponentContract component;
    ComponentFileIdentity contourIdentity;
    std::uint8_t contourSha256[32];
    std::uint16_t contourPath[32768]; // Absolute GetFinalPathNameByHandleW path, NUL terminated.
    std::uint32_t reserved[4];
};
static_assert(sizeof(InitializeRequest) == 65756, "Initialize request ABI changed");
static_assert(offsetof(InitializeRequest, contourPath) == 204, "Initialize request alignment changed");

template<std::size_t VersionBytes, std::size_t BuildBytes>
constexpr ComponentContract MakeComponentContract(const char (&version)[VersionBytes],
                                                   const char (&build)[BuildBytes]) noexcept {
    static_assert(VersionBytes <= 32 && BuildBytes <= 64, "Component identity is too long");
    ComponentContract value{};
    value.size = sizeof(value);
    value.magic = kComponentMagic;
    value.abi = kComponentAbi;
    value.machine = 0x014c; // IMAGE_FILE_MACHINE_I386
    value.pointerBytes = 4;
    for (std::size_t i = 0; i < VersionBytes; ++i) value.productVersion[i] = version[i];
    for (std::size_t i = 0; i < BuildBytes; ++i) value.buildId[i] = build[i];
    return value;
}

constexpr ComponentContract ExpectedComponentContract() noexcept {
    return MakeComponentContract(LAKE_FIX_VERSION, LAKE_FIX_COMPONENT_BUILD_ID);
}

constexpr bool ComponentIdentityConfigured() noexcept {
    const auto value = ExpectedComponentContract();
    return value.productVersion[0] != '\0' && value.buildId[0] != '\0';
}

inline bool ComponentContractMatches(const ComponentContract& actual,
                                     const ComponentContract& expected = ExpectedComponentContract()) noexcept {
    if (!expected.productVersion[0] || !expected.buildId[0]) return false;
    // All bytes are defined by the wire format, including padding in strings.
    const auto* a = reinterpret_cast<const unsigned char*>(&actual);
    const auto* b = reinterpret_cast<const unsigned char*>(&expected);
    for (std::size_t i = 0; i < sizeof(actual); ++i) if (a[i] != b[i]) return false;
    return true;
}
} // namespace lakefix
