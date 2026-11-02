#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
#include "../src/ddraw_scale.h"
#include "../src/profile.h"
#include <ddraw.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <new>
#include <type_traits>

// Own native-like COM fixtures only: no GPU, DirectDrawCreate*, Lake, real
// application window or installed DLL. EVERY native stub checks both its real
// object address and ORIGINAL table identity before accessing fixture fields.
namespace {
constexpr GUID unknownId{0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID draw7Id{0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0, 0x60, 0x97, 0x97, 0xea, 0x5b}};
constexpr GUID surface7Id{0x06675a80, 0x3b9b, 0x11d2, {0xb9, 0x2f, 0, 0x60, 0x97, 0x97, 0xea, 0x5b}};
// Same query used by the real Windows regression probe. Only the returned
// alias's IUnknown prefix is exercised; no fake or real D3D rendering is done.
constexpr GUID d3d7Id{0xf5049e77, 0x4861, 0x11d2, {0xa4, 7, 0, 0xa0, 0xc9, 6, 0x29, 0xa8}};
constexpr GUID otherSurfaceId{0x12345678, 0x4321, 0xabcd, {1, 2, 3, 4, 5, 6, 7, 8}};
constexpr DWORD bltError = 0x2143;
constexpr DWORD nativeError = 0x2222;
constexpr DWORD createError = 0x4141;
constexpr DWORD coopError = 0x4142;
constexpr DWORD clipError = 0x5152;
int failures = 0;
volatile LONG badNativeThis = 0;
volatile LONG badNativeArguments = 0;

void Check(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}
bool SameId(REFIID a, const GUID& b) noexcept { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }
bool SameRect(const RECT& a, const RECT& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
bool StatsAre(unsigned long draws, unsigned long surfaces, unsigned long live, unsigned long owners) noexcept {
    const auto stats = lakefix::GetScalePresenterStats();
    return stats.directDrawObjects == draws && stats.primarySurfaces == surfaces &&
           stats.liveProxyObjects == live && stats.ownerContexts == owners;
}
HWND Window(UINT_PTR value) noexcept { return reinterpret_cast<HWND>(value); }

struct FakeDraw;
struct FakeSurface;
struct FakeAlias {
    IUnknown iface{};
    FakeDraw* draw = nullptr;
    FakeSurface* surface = nullptr;
};
struct FakeClipper {
    IDirectDrawClipper iface{};
    volatile LONG references = 1;
    volatile LONG calls[9]{};
    bool destroyed = false;
    HWND window = nullptr;
    DWORD seenFlags = 0;
    HWND seenWindow = nullptr;
    HRESULT getResult = S_OK;
    HRESULT setResult = S_OK;
};
struct BltCall {
    LPRECT destinationPointer = nullptr;
    RECT destination{};
    IDirectDrawSurface7* source = nullptr;
    LPRECT sourceRect = nullptr;
    DWORD flags = 0;
    LPDDBLTFX effects = nullptr;
    DWORD fillSize = 0;
    DWORD fillColor = 1;
    DWORD incomingError = 0;
};
struct FakeSurface {
    IDirectDrawSurface7 iface{};
    volatile LONG references = 1;
    volatile LONG calls[49]{};
    FakeAlias alias{};
    bool destroyed = false;
    bool issued = false;
    DWORD width = 1024;
    DWORD height = 768;
    DWORD caps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
    DWORD descriptionFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_CAPS;
    DWORD caps2 = 0;
    HRESULT descriptionResult = S_OK;
    HRESULT presentationResult = S_FALSE;
    HRESULT fillResult = S_OK;
    HRESULT setClipperResult = S_OK;
    HRESULT getClipperResult = S_OK;
    HRESULT restoreResult = S_OK;
    HRESULT releaseDCResult = S_OK;
    DWORD incomingRestoreError = 0, incomingReleaseDCError = 0;
    HDC seenDC = nullptr;
    bool letterboxContents = false;
    bool dcDrewOverLetterbox = false;
    unsigned descriptions = 0;
    bool descriptionSizeCorrect = true;
    DWORD incomingDescriptionError = 0;
    unsigned blts = 0;
    unsigned fills = 0;
    BltCall bltCalls[16]{};
    IDirectDrawSurface7* seenSurface = nullptr;
    IDirectDraw* seenDraw = nullptr;
    DWORD seenFlags = 0;
    DWORD incomingClipperError = 0;
    FakeClipper* clipper = nullptr;
    IDirectDrawClipper* seenClipper = nullptr;
    FakeSurface* attached = nullptr;
    FakeDraw* parent = nullptr;
    GUID seenGuid{};
    void (*onFinalRelease)(FakeSurface*) noexcept = nullptr;
    void (*onDescription)(FakeSurface*) noexcept = nullptr;
    void (*onBlt)(FakeSurface*) noexcept = nullptr;
    void (*onRestore)(FakeSurface*) noexcept = nullptr;
    void (*onReleaseDC)(FakeSurface*) noexcept = nullptr;
};
struct FakeDraw {
    IDirectDraw7 iface{};
    volatile LONG references = 1;
    volatile LONG calls[30]{};
    FakeAlias alias{};
    bool destroyed = false;
    FakeSurface* nextSurface = nullptr;
    HRESULT createResult = S_OK;
    HRESULT coopResult = S_OK;
    HRESULT evaluateResult = S_FALSE;
    HRESULT restoreAllResult = S_OK;
    DWORD incomingRestoreAllError = 0;
    LPDDSURFACEDESC2 seenDescription = nullptr;
    LPDIRECTDRAWSURFACE7* seenOutput = nullptr;
    IUnknown* seenOuter = nullptr;
    HWND seenWindow = nullptr;
    DWORD seenFlags = 0;
    DWORD incomingEvaluateError = 0;
    IDirectDrawSurface7* seenSurface = nullptr;
    void (*onFinalRelease)(FakeDraw*) noexcept = nullptr;
    void (*onEvaluateMode)(FakeDraw*) noexcept = nullptr;
    void (*onRestoreAll)(FakeDraw*) noexcept = nullptr;
};

FakeDraw* realDraws[96]{};
FakeSurface* realSurfaces[320]{};
FakeClipper* realClippers[8]{};
unsigned drawCount = 0, surfaceCount = 0, clipperCount = 0;
IDirectDraw7Vtbl* NativeDrawTable() noexcept;
IDirectDrawSurface7Vtbl* NativeSurfaceTable() noexcept;
IDirectDrawClipperVtbl* NativeClipperTable() noexcept;
IUnknownVtbl* NativeAliasTable() noexcept;

template<class T, size_t N>
void Register(T* (&entries)[N], unsigned& count, T* value) noexcept {
    for (unsigned index = 0; index != count; ++index) if (entries[index] == value) return;
    if (count < N) entries[count++] = value;
    else InterlockedIncrement(&badNativeThis);
}
void ResetFixtures() noexcept { drawCount = surfaceCount = clipperCount = 0; }

FakeDraw* Native(IDirectDraw7* self, unsigned slot) noexcept {
    for (unsigned index = 0; index != drawCount; ++index) {
        auto* value = realDraws[index];
        if (&value->iface == self && value->iface.lpVtbl == NativeDrawTable() &&
            !value->destroyed && value->references > 0) {
            InterlockedIncrement(&value->calls[slot]);
            return value;
        }
    }
    InterlockedIncrement(&badNativeThis);
    return nullptr;
}
FakeSurface* Native(IDirectDrawSurface7* self, unsigned slot) noexcept {
    for (unsigned index = 0; index != surfaceCount; ++index) {
        auto* value = realSurfaces[index];
        if (&value->iface == self && value->iface.lpVtbl == NativeSurfaceTable() &&
            !value->destroyed && value->references > 0) {
            InterlockedIncrement(&value->calls[slot]);
            return value;
        }
    }
    InterlockedIncrement(&badNativeThis);
    return nullptr;
}
FakeClipper* Native(IDirectDrawClipper* self, unsigned slot) noexcept {
    for (unsigned index = 0; index != clipperCount; ++index) {
        auto* value = realClippers[index];
        if (&value->iface == self && value->iface.lpVtbl == NativeClipperTable() &&
            !value->destroyed && value->references > 0) {
            InterlockedIncrement(&value->calls[slot]);
            return value;
        }
    }
    InterlockedIncrement(&badNativeThis);
    return nullptr;
}
FakeAlias* Native(IUnknown* self) noexcept {
    for (unsigned index = 0; index != drawCount; ++index) {
        if (&realDraws[index]->alias.iface == self && self->lpVtbl == NativeAliasTable()) return &realDraws[index]->alias;
    }
    for (unsigned index = 0; index != surfaceCount; ++index) {
        if (&realSurfaces[index]->alias.iface == self && self->lpVtbl == NativeAliasTable()) return &realSurfaces[index]->alias;
    }
    InterlockedIncrement(&badNativeThis);
    return nullptr;
}
bool IsNativeSurface(const IDirectDrawSurface7* value) noexcept {
    if (!value) return true;
    for (unsigned index = 0; index != surfaceCount; ++index) {
        if (&realSurfaces[index]->iface == value && value->lpVtbl == NativeSurfaceTable() &&
            !realSurfaces[index]->destroyed) return true;
    }
    return false;
}
bool IsNativeDraw(const void* value) noexcept {
    if (!value) return true;
    for (unsigned index = 0; index != drawCount; ++index) {
        if (&realDraws[index]->iface == value && realDraws[index]->iface.lpVtbl == NativeDrawTable() &&
            !realDraws[index]->destroyed) return true;
    }
    return false;
}

// Correct stdcall signatures are deduced from the SDK, including unused slots.
template<class Signature, unsigned Slot> struct NativeStub;
template<unsigned Slot, class Self, class... Args>
struct NativeStub<HRESULT (STDMETHODCALLTYPE*)(Self*, Args...), Slot> {
    static HRESULT STDMETHODCALLTYPE Call(Self* self, Args...) {
        if (!Native(self, Slot)) return E_FAIL;
        SetLastError(nativeError);
        return S_FALSE;
    }
};

ULONG STDMETHODCALLTYPE DrawAddRef(IDirectDraw7* self) {
    auto* value = Native(self, 1);
    if (!value) return 0;
    const ULONG result = static_cast<ULONG>(InterlockedIncrement(&value->references));
    SetLastError(nativeError);
    return result;
}
ULONG STDMETHODCALLTYPE SurfaceAddRef(IDirectDrawSurface7* self) {
    auto* value = Native(self, 1);
    if (!value) return 0;
    const ULONG result = static_cast<ULONG>(InterlockedIncrement(&value->references));
    SetLastError(nativeError);
    return result;
}
ULONG STDMETHODCALLTYPE DrawRelease(IDirectDraw7* self) {
    auto* value = Native(self, 2);
    if (!value) return 0;
    const ULONG result = static_cast<ULONG>(InterlockedDecrement(&value->references));
    if (!result) {
        const auto callback = value->onFinalRelease;
        value->destroyed = true;
        if (callback) callback(value);
    }
    SetLastError(0x3141);
    return result; // No access to the possibly recreated fixture after callback.
}
ULONG STDMETHODCALLTYPE SurfaceRelease(IDirectDrawSurface7* self) {
    auto* value = Native(self, 2);
    if (!value) return 0;
    const ULONG result = static_cast<ULONG>(InterlockedDecrement(&value->references));
    if (!result) {
        const auto callback = value->onFinalRelease;
        value->destroyed = true;
        if (callback) callback(value);
    }
    SetLastError(0x3142);
    return result;
}
ULONG STDMETHODCALLTYPE AliasAddRef(IUnknown* self) {
    auto* alias = Native(self);
    if (!alias) return 0;
    return alias->draw ? DrawAddRef(&alias->draw->iface) : SurfaceAddRef(&alias->surface->iface);
}
ULONG STDMETHODCALLTYPE AliasRelease(IUnknown* self) {
    auto* alias = Native(self);
    if (!alias) return 0;
    return alias->draw ? DrawRelease(&alias->draw->iface) : SurfaceRelease(&alias->surface->iface);
}
HRESULT STDMETHODCALLTYPE AliasQuery(IUnknown* self, REFIID id, void** output) {
    if (!Native(self)) return E_FAIL;
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!SameId(id, unknownId)) return E_NOINTERFACE;
    AliasAddRef(self);
    *output = self;
    return S_OK;
}
IUnknownVtbl* NativeAliasTable() noexcept {
    static IUnknownVtbl table{AliasQuery, AliasAddRef, AliasRelease};
    return &table;
}
HRESULT STDMETHODCALLTYPE DrawQuery(IDirectDraw7* self, REFIID id, void** output) {
    auto* value = Native(self, 0);
    if (!value) return E_NOINTERFACE; // Explicit native-vtable-identity regression check.
    if (!output) return E_POINTER;
    *output = nullptr;
    if (SameId(id, unknownId) || SameId(id, draw7Id) || SameId(id, d3d7Id)) {
        DrawAddRef(self);
        *output = SameId(id, d3d7Id) ? static_cast<void*>(&value->alias.iface) : static_cast<void*>(self);
        SetLastError(nativeError);
        return S_OK;
    }
    SetLastError(nativeError);
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE SurfaceQuery(IDirectDrawSurface7* self, REFIID id, void** output) {
    auto* value = Native(self, 0);
    if (!value) return E_NOINTERFACE;
    if (!output) return E_POINTER;
    *output = nullptr;
    if (SameId(id, unknownId) || SameId(id, surface7Id) || SameId(id, otherSurfaceId)) {
        SurfaceAddRef(self);
        *output = SameId(id, otherSurfaceId) ? static_cast<void*>(&value->alias.iface) : static_cast<void*>(self);
        SetLastError(nativeError);
        return S_OK;
    }
    SetLastError(nativeError);
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE CreateSurface(IDirectDraw7* self, LPDDSURFACEDESC2 description,
                                        LPDIRECTDRAWSURFACE7* output, IUnknown* outer) {
    auto* value = Native(self, 6);
    if (!value) return E_FAIL;
    value->seenDescription = description;
    value->seenOutput = output;
    value->seenOuter = outer;
    if (output) {
        *output = nullptr;
        if (SUCCEEDED(value->createResult) && value->nextSurface) {
            auto* surface = value->nextSurface;
            if (surface->issued) SurfaceAddRef(&surface->iface);
            surface->issued = true;
            surface->parent = value;
            *output = &surface->iface;
        }
    }
    SetLastError(createError);
    return value->createResult;
}
HRESULT STDMETHODCALLTYPE Cooperative(IDirectDraw7* self, HWND window, DWORD flags) {
    auto* value = Native(self, 20);
    if (!value) return E_FAIL;
    value->seenWindow = window;
    value->seenFlags = flags;
    SetLastError(coopError);
    return value->coopResult;
}
HRESULT STDMETHODCALLTYPE EvaluateMode(IDirectDraw7* self, DWORD flags, DWORD* timeout) {
    auto* value = Native(self, 29);
    if (!value) return E_FAIL;
    value->seenFlags = flags;
    value->incomingEvaluateError = GetLastError();
    if (timeout) *timeout = flags + 17;
    const HRESULT result = value->evaluateResult;
    const auto callback = value->onEvaluateMode;
    if (callback) callback(value);
    SetLastError(nativeError);
    return result;
}
HRESULT STDMETHODCALLTYPE Duplicate(IDirectDraw7* self, LPDIRECTDRAWSURFACE7 surface,
                                     LPDIRECTDRAWSURFACE7* output) {
    auto* value = Native(self, 7);
    if (!value) return E_FAIL;
    if (!IsNativeSurface(surface)) InterlockedIncrement(&badNativeArguments);
    value->seenSurface = surface;
    if (output) {
        *output = surface;
        if (surface) SurfaceAddRef(surface);
    }
    SetLastError(nativeError);
    return S_FALSE;
}
HRESULT STDMETHODCALLTYPE Description(IDirectDrawSurface7* self, LPDDSURFACEDESC2 output) {
    auto* value = Native(self, 22);
    if (!value) return E_FAIL;
    value->incomingDescriptionError = GetLastError();
    ++value->descriptions;
    if (!output) return E_POINTER;
    value->descriptionSizeCorrect = value->descriptionSizeCorrect && output->dwSize == sizeof(*output);
    if (SUCCEEDED(value->descriptionResult)) {
        output->dwFlags = value->descriptionFlags;
        output->dwWidth = value->width;
        output->dwHeight = value->height;
        output->ddsCaps.dwCaps = value->caps;
        output->ddsCaps.dwCaps2 = value->caps2;
    }
    const HRESULT result = value->descriptionResult;
    const auto callback = value->onDescription;
    if (callback) callback(value);
    SetLastError(0x5151);
    return result;
}
HRESULT STDMETHODCALLTYPE NativeBlt(IDirectDrawSurface7* self, LPRECT destination,
                                     LPDIRECTDRAWSURFACE7 source, LPRECT sourceRect,
                                     DWORD flags, LPDDBLTFX effects) {
    auto* value = Native(self, 5);
    if (!value) return E_FAIL;
    if (!IsNativeSurface(source)) InterlockedIncrement(&badNativeArguments);
    const bool fill = !source && (flags & DDBLT_COLORFILL) != 0;
    if (value->blts < ARRAYSIZE(value->bltCalls)) {
        auto& call = value->bltCalls[value->blts];
        call.destinationPointer = destination;
        call.destination = destination ? *destination : RECT{};
        call.source = source;
        call.sourceRect = sourceRect;
        call.flags = flags;
        call.effects = effects;
        call.fillSize = fill && effects ? effects->dwSize : 0;
        call.fillColor = fill && effects ? effects->dwFillColor : 1;
        call.incomingError = GetLastError();
    }
    ++value->blts;
    if (fill) ++value->fills;
    if (fill && SUCCEEDED(value->fillResult)) value->letterboxContents = true;
    const auto callback = value->onBlt;
    const HRESULT result = fill ? value->fillResult : value->presentationResult;
    if (callback) callback(value);
    SetLastError(bltError);
    return result;
}
HRESULT Peer(IDirectDrawSurface7* self, LPDIRECTDRAWSURFACE7 peer, unsigned slot, DWORD flags = 0) {
    auto* value = Native(self, slot);
    if (!value) return E_FAIL;
    if (!IsNativeSurface(peer)) InterlockedIncrement(&badNativeArguments);
    value->seenSurface = peer;
    value->seenFlags = flags;
    SetLastError(nativeError);
    return S_FALSE;
}
HRESULT STDMETHODCALLTYPE AddAttached(IDirectDrawSurface7* self, LPDIRECTDRAWSURFACE7 peer) { return Peer(self, peer, 3); }
HRESULT STDMETHODCALLTYPE DeleteAttached(IDirectDrawSurface7* self, DWORD flags, LPDIRECTDRAWSURFACE7 peer) { return Peer(self, peer, 8, flags); }
HRESULT STDMETHODCALLTYPE Fast(IDirectDrawSurface7* self, DWORD, DWORD, LPDIRECTDRAWSURFACE7 peer, LPRECT, DWORD flags) { return Peer(self, peer, 7, flags); }
HRESULT STDMETHODCALLTYPE Flip(IDirectDrawSurface7* self, LPDIRECTDRAWSURFACE7 peer, DWORD flags) { return Peer(self, peer, 11, flags); }
HRESULT STDMETHODCALLTYPE Overlay(IDirectDrawSurface7* self, LPRECT, LPDIRECTDRAWSURFACE7 peer, LPRECT, DWORD flags, LPDDOVERLAYFX) { return Peer(self, peer, 33, flags); }
HRESULT STDMETHODCALLTYPE OverlayZ(IDirectDrawSurface7* self, DWORD flags, LPDIRECTDRAWSURFACE7 peer) { return Peer(self, peer, 35, flags); }
HRESULT STDMETHODCALLTYPE SurfaceInitialize(IDirectDrawSurface7* self, LPDIRECTDRAW draw, LPDDSURFACEDESC2) {
    auto* value = Native(self, 23);
    if (!value) return E_FAIL;
    if (!IsNativeDraw(draw)) InterlockedIncrement(&badNativeArguments);
    value->seenDraw = draw;
    SetLastError(nativeError);
    return S_FALSE;
}
HRESULT STDMETHODCALLTYPE Attached(IDirectDrawSurface7* self, LPDDSCAPS2, LPDIRECTDRAWSURFACE7* output) {
    auto* value = Native(self, 12);
    if (!value) return E_FAIL;
    if (!output) return E_POINTER;
    *output = value->attached ? &value->attached->iface : nullptr;
    if (*output) SurfaceAddRef(*output);
    SetLastError(nativeError);
    return *output ? S_OK : E_FAIL;
}
HRESULT STDMETHODCALLTYPE GetDraw(IDirectDrawSurface7* self, LPVOID* output) {
    auto* value = Native(self, 36);
    if (!value) return E_FAIL;
    if (!output) return E_POINTER;
    *output = value->parent ? &value->parent->iface : nullptr;
    if (*output) DrawAddRef(&value->parent->iface);
    SetLastError(nativeError);
    return *output ? S_OK : E_FAIL;
}
HRESULT STDMETHODCALLTYPE GetLOD(IDirectDrawSurface7* self, LPDWORD output) {
    if (!Native(self, 48)) return E_FAIL;
    if (output) *output = 73;
    SetLastError(nativeError);
    return S_FALSE;
}
HRESULT STDMETHODCALLTYPE PrivateData(IDirectDrawSurface7* self, REFGUID id, LPVOID, DWORD, DWORD) {
    auto* value = Native(self, 40);
    if (!value) return E_FAIL;
    value->seenGuid = id;
    SetLastError(nativeError);
    return S_FALSE;
}
ULONG STDMETHODCALLTYPE ClipperAddRef(IDirectDrawClipper* self) {
    auto* value = Native(self, 1);
    return value ? static_cast<ULONG>(InterlockedIncrement(&value->references)) : 0;
}
ULONG STDMETHODCALLTYPE ClipperRelease(IDirectDrawClipper* self) {
    auto* value = Native(self, 2);
    if (!value) return 0;
    const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&value->references));
    if (!remaining) value->destroyed = true;
    return remaining;
}
HRESULT STDMETHODCALLTYPE ClipperWindow(IDirectDrawClipper* self, HWND* output) {
    auto* value = Native(self, 4);
    if (!value) return E_FAIL;
    if (output && SUCCEEDED(value->getResult)) *output = value->window;
    SetLastError(clipError);
    return value->getResult;
}
HRESULT STDMETHODCALLTYPE SetClipperWindow(IDirectDrawClipper* self, DWORD flags, HWND window) {
    auto* value = Native(self, 8);
    if (!value) return E_FAIL;
    value->seenFlags = flags;
    value->seenWindow = window;
    if (SUCCEEDED(value->setResult)) value->window = window;
    SetLastError(clipError);
    return value->setResult;
}
HRESULT STDMETHODCALLTYPE GetClipper(IDirectDrawSurface7* self, LPDIRECTDRAWCLIPPER* output) {
    auto* value = Native(self, 15);
    if (!value) return E_FAIL;
    if (!output) return E_POINTER;
    *output = nullptr;
    if (FAILED(value->getClipperResult)) return value->getClipperResult;
    if (!value->clipper) return DDERR_NOCLIPPERATTACHED;
    *output = &value->clipper->iface;
    ClipperAddRef(*output);
    SetLastError(nativeError);
    return value->getClipperResult;
}
HRESULT STDMETHODCALLTYPE SetClipper(IDirectDrawSurface7* self, LPDIRECTDRAWCLIPPER clipper) {
    auto* value = Native(self, 28);
    if (!value) return E_FAIL;
    value->seenClipper = clipper;
    value->incomingClipperError = GetLastError();
    SetLastError(nativeError);
    return value->setClipperResult;
}
HRESULT STDMETHODCALLTYPE Restore(IDirectDrawSurface7* self) {
    auto* value = Native(self, 27);
    if (!value) return E_FAIL;
    value->incomingRestoreError = GetLastError();
    const HRESULT result = value->restoreResult;
    if (SUCCEEDED(result)) value->letterboxContents = false;
    if (value->onRestore) value->onRestore(value);
    SetLastError(nativeError);
    return result;
}
HRESULT STDMETHODCALLTYPE ReleaseDC(IDirectDrawSurface7* self, HDC dc) {
    auto* value = Native(self, 26);
    if (!value) return E_FAIL;
    value->seenDC = dc; value->incomingReleaseDCError = GetLastError();
    if (SUCCEEDED(value->releaseDCResult) && value->dcDrewOverLetterbox) value->letterboxContents = false;
    const HRESULT result = value->releaseDCResult;
    if (value->onReleaseDC) value->onReleaseDC(value);
    SetLastError(nativeError);
    return result;
}
HRESULT STDMETHODCALLTYPE RestoreAll(IDirectDraw7* self) {
    auto* value = Native(self, 25);
    if (!value) return E_FAIL;
    value->incomingRestoreAllError = GetLastError();
    const HRESULT result = value->restoreAllResult;
    if (SUCCEEDED(result)) for (unsigned index = 0; index != surfaceCount; ++index)
        if (realSurfaces[index]->parent == value) realSurfaces[index]->letterboxContents = false;
    if (value->onRestoreAll) value->onRestoreAll(value);
    SetLastError(nativeError);
    return result;
}

IDirectDraw7Vtbl* NativeDrawTable() noexcept {
    static IDirectDraw7Vtbl table = []() {
        IDirectDraw7Vtbl value{};
#define STUB(name) value.name = NativeStub<decltype(value.name), offsetof(IDirectDraw7Vtbl, name) / sizeof(void*)>::Call
        STUB(Compact); STUB(CreateClipper); STUB(CreatePalette); STUB(EnumDisplayModes);
        STUB(EnumSurfaces); STUB(FlipToGDISurface); STUB(GetCaps); STUB(GetDisplayMode);
        STUB(GetFourCCCodes); STUB(GetGDISurface); STUB(GetMonitorFrequency); STUB(GetScanLine);
        STUB(GetVerticalBlankStatus); STUB(Initialize); STUB(RestoreDisplayMode); STUB(SetDisplayMode);
        STUB(WaitForVerticalBlank); STUB(GetAvailableVidMem); STUB(GetSurfaceFromDC);
        STUB(TestCooperativeLevel); STUB(GetDeviceIdentifier); STUB(StartModeTest);
#undef STUB
        value.QueryInterface = DrawQuery; value.AddRef = DrawAddRef; value.Release = DrawRelease;
        value.CreateSurface = CreateSurface; value.DuplicateSurface = Duplicate;
        value.SetCooperativeLevel = Cooperative; value.EvaluateMode = EvaluateMode;
        value.RestoreAllSurfaces = RestoreAll;
        return value;
    }();
    return &table;
}
IDirectDrawSurface7Vtbl* NativeSurfaceTable() noexcept {
    static IDirectDrawSurface7Vtbl table = []() {
        IDirectDrawSurface7Vtbl value{};
#define STUB(name) value.name = NativeStub<decltype(value.name), offsetof(IDirectDrawSurface7Vtbl, name) / sizeof(void*)>::Call
        STUB(AddOverlayDirtyRect); STUB(BltBatch); STUB(EnumAttachedSurfaces); STUB(EnumOverlayZOrders);
        STUB(GetBltStatus); STUB(GetCaps); STUB(GetColorKey); STUB(GetDC); STUB(GetFlipStatus);
        STUB(GetOverlayPosition); STUB(GetPalette); STUB(GetPixelFormat); STUB(IsLost); STUB(Lock);
        STUB(SetColorKey); STUB(SetOverlayPosition); STUB(SetPalette);
        STUB(Unlock); STUB(UpdateOverlayDisplay); STUB(PageLock); STUB(PageUnlock); STUB(SetSurfaceDesc);
        STUB(GetPrivateData); STUB(FreePrivateData); STUB(GetUniquenessValue); STUB(ChangeUniquenessValue);
        STUB(SetPriority); STUB(GetPriority); STUB(SetLOD);
#undef STUB
        value.QueryInterface = SurfaceQuery; value.AddRef = SurfaceAddRef; value.Release = SurfaceRelease;
        value.AddAttachedSurface = AddAttached; value.Blt = NativeBlt; value.BltFast = Fast;
        value.DeleteAttachedSurface = DeleteAttached; value.Flip = Flip; value.GetAttachedSurface = Attached;
        value.GetClipper = GetClipper; value.GetSurfaceDesc = Description; value.Initialize = SurfaceInitialize;
        value.SetClipper = SetClipper; value.UpdateOverlay = Overlay; value.UpdateOverlayZOrder = OverlayZ;
        value.GetDDInterface = GetDraw; value.SetPrivateData = PrivateData; value.GetLOD = GetLOD;
        value.Restore = Restore; value.ReleaseDC = ReleaseDC;
        return value;
    }();
    return &table;
}
IDirectDrawClipperVtbl* NativeClipperTable() noexcept {
    static IDirectDrawClipperVtbl table = []() {
        IDirectDrawClipperVtbl value{};
#define STUB(name) value.name = NativeStub<decltype(value.name), offsetof(IDirectDrawClipperVtbl, name) / sizeof(void*)>::Call
        STUB(QueryInterface); STUB(GetClipList); STUB(Initialize); STUB(IsClipListChanged); STUB(SetClipList);
#undef STUB
        value.AddRef = ClipperAddRef; value.Release = ClipperRelease;
        value.GetHWnd = ClipperWindow; value.SetHWnd = SetClipperWindow;
        return value;
    }();
    return &table;
}
void Initialize(FakeDraw& value) noexcept {
    value = FakeDraw{}; value.iface.lpVtbl = NativeDrawTable();
    value.alias.iface.lpVtbl = NativeAliasTable(); value.alias.draw = &value;
    Register(realDraws, drawCount, &value);
}
void Initialize(FakeSurface& value) noexcept {
    value = FakeSurface{}; value.iface.lpVtbl = NativeSurfaceTable();
    value.alias.iface.lpVtbl = NativeAliasTable(); value.alias.surface = &value;
    Register(realSurfaces, surfaceCount, &value);
}
void Initialize(FakeClipper& value) noexcept {
    value = FakeClipper{}; value.iface.lpVtbl = NativeClipperTable();
    Register(realClippers, clipperCount, &value);
}
IDirectDraw7* Wrap(FakeDraw& value) noexcept {
    void* output = &value.iface;
    return lakefix::WrapDirectDraw7(&output) ? static_cast<IDirectDraw7*>(output) : nullptr;
}
DDSURFACEDESC2 SurfaceDescription(DWORD caps = DDSCAPS_PRIMARYSURFACE) noexcept {
    DDSURFACEDESC2 value{}; value.dwSize = sizeof(value); value.dwFlags = DDSD_CAPS;
    value.ddsCaps.dwCaps = caps; return value;
}
IDirectDrawSurface7* Primary(IDirectDraw7* draw, FakeDraw& native, FakeSurface& surface) noexcept {
    surface.caps = DDSCAPS_PRIMARYSURFACE;
    native.nextSurface = &surface;
    auto description = SurfaceDescription();
    IDirectDrawSurface7* output = nullptr;
    draw->lpVtbl->CreateSurface(draw, &description, &output, nullptr);
    return output;
}
void ClearCalls(FakeSurface& value) noexcept { value.blts = value.fills = 0; }
const BltCall& LastCall(const FakeSurface& value) noexcept {
    return value.bltCalls[value.blts && value.blts <= ARRAYSIZE(value.bltCalls) ? value.blts - 1 : 0];
}

RECT fullClient{100, 200, 1700, 1100};
RECT fittedViewport{300, 200, 1500, 1100};
bool providerEnabled = true;
unsigned providerCalls = 0, activeLogs = 0, failureLogs = 0, descriptionLogs = 0, wrapFailureLogs = 0;
HWND providerWindow = nullptr;
LONG providerWidth = 0, providerHeight = 0;
HWND clipHost = nullptr, clipOwnerSeen = nullptr;
bool reenterProvider = false;
IDirectDrawSurface7* nestedPrimary = nullptr;
IDirectDrawSurface7* nestedSource = nullptr;
RECT nestedDestination{7, 8, 90, 100};
HRESULT nestedResult = E_UNEXPECTED;
bool Viewport(HWND, LONG, LONG, RECT*, RECT*) noexcept;
HWND ClipWindow(HWND) noexcept;
void Diagnostic(const wchar_t* text) noexcept {
    if (std::wcsstr(text, L"Scaled presentation active source=")) ++activeLogs;
    if (std::wcsstr(text, L"Scaled presentation failure ")) ++failureLogs;
    if (std::wcsstr(text, L"failure source GetSurfaceDesc ")) ++descriptionLogs;
    if (std::wcsstr(text, L"primary proxy allocation/capacity")) ++wrapFailureLogs;
    (void)lakefix::GetScalePresenterStats();
    lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow, nullptr, 1024, 768);
    SetLastError(0x6161);
}
bool Viewport(HWND window, LONG width, LONG height, RECT* full, RECT* fitted) noexcept {
    ++providerCalls; providerWindow = window; providerWidth = width; providerHeight = height;
    *full = fullClient; *fitted = fittedViewport;
    (void)lakefix::GetScalePresenterStats();
    lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow, nullptr, 1024, 768);
    if (reenterProvider && nestedPrimary && nestedSource) {
        reenterProvider = false;
        nestedResult = nestedPrimary->lpVtbl->Blt(nestedPrimary, &nestedDestination, nestedSource, nullptr, DDBLT_WAIT, nullptr);
    }
    SetLastError(0x7171);
    return providerEnabled;
}
HWND ClipWindow(HWND owner) noexcept {
    clipOwnerSeen = owner;
    (void)lakefix::GetScalePresenterStats();
    lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow, nullptr, 1024, 768);
    SetLastError(0x7172);
    return clipHost;
}
unsigned previousLoggerCalls = 0, updatedLoggerCalls = 0, nativeReconfigurations = 0;
bool loggerReentryObserved = false;
lakefix::ScalePresenterLog nativeReplacementLog = nullptr;
void PreviousDiagnostic(const wchar_t* text) noexcept {
    ++previousLoggerCalls;
    Diagnostic(text);
}
void UpdatedDiagnostic(const wchar_t* text) noexcept {
    ++updatedLoggerCalls;
    loggerReentryObserved = StatsAre(1, 0, 1, 1);
    Diagnostic(text); // Reenters stats/configuration and changes LastError.
}
void ReconfigureFromEvaluate(FakeDraw*) noexcept {
    ++nativeReconfigurations;
    lakefix::ConfigureScalePresenter(nullptr, nativeReplacementLog);
    SetLastError(0x7173);
}
bool IsBlackStrip(const BltCall& call, const RECT& expected) noexcept {
    return SameRect(call.destination, expected) && !call.source && !call.sourceRect &&
           call.flags == (DDBLT_COLORFILL | DDBLT_WAIT) && call.fillSize == sizeof(DDBLTFX) &&
           call.fillColor == 0 && call.destination.left >= fullClient.left && call.destination.top >= fullClient.top &&
           call.destination.right <= fullClient.right && call.destination.bottom <= fullClient.bottom;
}

void TestNativeIdentityAndTransfer() {
    ResetFixtures(); lakefix::ConfigureScalePresenter(nullptr, nullptr, nullptr);
    FakeDraw real{}; Initialize(real);
    void* before = nullptr;
    Check(real.iface.lpVtbl->QueryInterface(&real.iface, d3d7Id, &before) == S_OK,
          "native identity-sensitive QI succeeds before wrapping");
    static_cast<IUnknown*>(before)->lpVtbl->Release(static_cast<IUnknown*>(before));
    void* nothing = nullptr;
    SetLastError(0x1234);
    Check(!lakefix::WrapDirectDraw7(nullptr) && !lakefix::WrapDirectDraw7(&nothing) &&
          !nothing && GetLastError() == 0x1234, "invalid outpointer is rejected without mutation or LastError changes");
    void* owned = &real.iface;
    Check(lakefix::WrapDirectDraw7(&owned) && owned != &real.iface && real.iface.lpVtbl == NativeDrawTable() &&
          real.references == 1 && GetLastError() == 0x1234 && StatsAre(1, 0, 1, 1),
          "Wrap transfers exactly the existing reference and never changes native lpVtbl");
    auto* proxy = static_cast<IDirectDraw7*>(owned);
    Check(lakefix::WrapDirectDraw7(&owned) && owned == proxy && real.references == 1 && StatsAre(1, 0, 1, 1),
          "wrapping an already-owned proxy reference is idempotent");
    void* after = nullptr;
    Check(proxy->lpVtbl->QueryInterface(proxy, d3d7Id, &after) == S_OK && after == before &&
          proxy->lpVtbl->SetCooperativeLevel(proxy, nullptr, DDSCL_NORMAL) == S_OK &&
          !real.seenWindow && real.seenFlags == DDSCL_NORMAL && real.iface.lpVtbl == NativeDrawTable(),
          "native D3D7 query and null/NORMAL cooperation still work after wrapping");
    void* identity = nullptr;
    Check(proxy->lpVtbl->QueryInterface(proxy, unknownId, &identity) == S_OK && identity == proxy &&
          real.references == 3, "DD7 IUnknown returns the same proxy and AddRefs real DD7");
    auto* unknown = static_cast<IUnknown*>(identity);
    void* identity7 = nullptr;
    Check(unknown->lpVtbl->QueryInterface(unknown, draw7Id, &identity7) == S_OK && identity7 == proxy &&
          real.references == 4, "DD7 QI through proxy IUnknown returns the same DD7 proxy");
    static_cast<IDirectDraw7*>(identity7)->lpVtbl->Release(static_cast<IDirectDraw7*>(identity7));
    unknown->lpVtbl->Release(unknown);
    void* unsupported = reinterpret_cast<void*>(static_cast<UINT_PTR>(1));
    Check(proxy->lpVtbl->QueryInterface(proxy, otherSurfaceId, &unsupported) == E_NOINTERFACE && !unsupported &&
          real.references == 2, "unsupported interfaces preserve native QI error and output");
    DrawAddRef(&real.iface); // A separately owned native reference to be transferred.
    const LONG addRefs = real.calls[1];
    void* second = &real.iface;
    Check(lakefix::WrapDirectDraw7(&second) && second == proxy && real.calls[1] == addRefs && real.references == 3,
          "second owned native reference adopts existing proxy without an extra native AddRef");
    static_cast<IDirectDraw7*>(second)->lpVtbl->Release(static_cast<IDirectDraw7*>(second));
    Check(proxy->lpVtbl->Release(proxy) == 1 && GetLastError() == 0x3141 && StatsAre(0, 0, 0, 0) &&
          real.references == 1 && !real.destroyed,
          "last proxy Release cleans up even when native ULONG remains positive for an alias");
    Check(static_cast<IUnknown*>(after)->lpVtbl->Release(static_cast<IUnknown*>(after)) == 0 && real.destroyed,
          "native alias survives proxy and independently owns the remaining reference");
}

template<class T> decltype(auto) EmptyArgument() {
    if constexpr (std::is_reference_v<T>) {
        static std::remove_reference_t<T> value{};
        return (value);
    } else return T{};
}
template<class Signature> struct Invoke;
template<class R, class Self, class... Args>
struct Invoke<R (STDMETHODCALLTYPE*)(Self*, Args...)> {
    using Function = R (STDMETHODCALLTYPE*)(Self*, Args...);
    static void Call(Function function, Self* self) { (void)function(self, EmptyArgument<Args>()...); }
};
template<size_t N> bool EverySlotCalled(const volatile LONG (&calls)[N]) noexcept {
    for (size_t index = 0; index != N; ++index) if (!calls[index]) return false;
    return true;
}

void TestAllForwardersAndArguments() {
    ResetFixtures(); lakefix::ConfigureScalePresenter(nullptr, nullptr, nullptr);
    FakeDraw real{}; FakeSurface surface{}, peer{}, auxiliary{};
    Initialize(real); Initialize(surface); Initialize(peer); Initialize(auxiliary);
    auto* draw = Wrap(real);
    auto* primary = Primary(draw, real, surface);
    auto* peerProxy = Primary(draw, real, peer);
    void* alias = nullptr;
    draw->lpVtbl->QueryInterface(draw, d3d7Id, &alias);
    static_cast<IUnknown*>(alias)->lpVtbl->Release(static_cast<IUnknown*>(alias));
    draw->lpVtbl->AddRef(draw); draw->lpVtbl->Release(draw);
    primary->lpVtbl->QueryInterface(primary, otherSurfaceId, &alias);
    static_cast<IUnknown*>(alias)->lpVtbl->Release(static_cast<IUnknown*>(alias));
    primary->lpVtbl->AddRef(primary); primary->lpVtbl->Release(primary);
#define CALL_DD(name) Invoke<decltype(draw->lpVtbl->name)>::Call(draw->lpVtbl->name, draw)
    CALL_DD(Compact); CALL_DD(CreateClipper); CALL_DD(CreatePalette); CALL_DD(CreateSurface);
    CALL_DD(DuplicateSurface); CALL_DD(EnumDisplayModes); CALL_DD(EnumSurfaces); CALL_DD(FlipToGDISurface);
    CALL_DD(GetCaps); CALL_DD(GetDisplayMode); CALL_DD(GetFourCCCodes); CALL_DD(GetGDISurface);
    CALL_DD(GetMonitorFrequency); CALL_DD(GetScanLine); CALL_DD(GetVerticalBlankStatus); CALL_DD(Initialize);
    CALL_DD(RestoreDisplayMode); CALL_DD(SetCooperativeLevel); CALL_DD(SetDisplayMode); CALL_DD(WaitForVerticalBlank);
    CALL_DD(GetAvailableVidMem); CALL_DD(GetSurfaceFromDC); CALL_DD(RestoreAllSurfaces); CALL_DD(TestCooperativeLevel);
    CALL_DD(GetDeviceIdentifier); CALL_DD(StartModeTest); CALL_DD(EvaluateMode);
#undef CALL_DD
#define CALL_SURFACE(name) Invoke<decltype(primary->lpVtbl->name)>::Call(primary->lpVtbl->name, primary)
    CALL_SURFACE(AddAttachedSurface); CALL_SURFACE(AddOverlayDirtyRect); CALL_SURFACE(Blt); CALL_SURFACE(BltBatch);
    CALL_SURFACE(BltFast); CALL_SURFACE(DeleteAttachedSurface); CALL_SURFACE(EnumAttachedSurfaces); CALL_SURFACE(EnumOverlayZOrders);
    CALL_SURFACE(Flip); CALL_SURFACE(GetAttachedSurface); CALL_SURFACE(GetBltStatus); CALL_SURFACE(GetCaps);
    CALL_SURFACE(GetClipper); CALL_SURFACE(GetColorKey); CALL_SURFACE(GetDC); CALL_SURFACE(GetFlipStatus);
    CALL_SURFACE(GetOverlayPosition); CALL_SURFACE(GetPalette); CALL_SURFACE(GetPixelFormat); CALL_SURFACE(GetSurfaceDesc);
    CALL_SURFACE(Initialize); CALL_SURFACE(IsLost); CALL_SURFACE(Lock); CALL_SURFACE(ReleaseDC); CALL_SURFACE(Restore);
    CALL_SURFACE(SetClipper); CALL_SURFACE(SetColorKey); CALL_SURFACE(SetOverlayPosition); CALL_SURFACE(SetPalette);
    CALL_SURFACE(Unlock); CALL_SURFACE(UpdateOverlay); CALL_SURFACE(UpdateOverlayDisplay); CALL_SURFACE(UpdateOverlayZOrder);
    CALL_SURFACE(GetDDInterface); CALL_SURFACE(PageLock); CALL_SURFACE(PageUnlock); CALL_SURFACE(SetSurfaceDesc);
    CALL_SURFACE(SetPrivateData); CALL_SURFACE(GetPrivateData); CALL_SURFACE(FreePrivateData); CALL_SURFACE(GetUniquenessValue);
    CALL_SURFACE(ChangeUniquenessValue); CALL_SURFACE(SetPriority); CALL_SURFACE(GetPriority); CALL_SURFACE(SetLOD); CALL_SURFACE(GetLOD);
#undef CALL_SURFACE
    Check(EverySlotCalled(real.calls) && EverySlotCalled(surface.calls) && !badNativeThis,
          "all 30 DD7 and all 49 surface7 slots dispatch with original native this and vtable identity");
    DWORD lod = 0, timeout = 0;
    Check(primary->lpVtbl->GetLOD(primary, &lod) == S_FALSE && lod == 73 &&
          draw->lpVtbl->EvaluateMode(draw, 9, &timeout) == S_FALSE && timeout == 26,
          "final slots keep exact signatures, outputs and native HRESULTs");
    void* identity = nullptr;
    primary->lpVtbl->QueryInterface(primary, unknownId, &identity);
    void* identity7 = nullptr;
    static_cast<IUnknown*>(identity)->lpVtbl->QueryInterface(static_cast<IUnknown*>(identity), surface7Id, &identity7);
    Check(identity == primary && identity7 == primary, "surface IUnknown and Surface7 QI preserve proxy identity");
    static_cast<IUnknown*>(identity)->lpVtbl->Release(static_cast<IUnknown*>(identity));
    static_cast<IDirectDrawSurface7*>(identity7)->lpVtbl->Release(static_cast<IDirectDrawSurface7*>(identity7));
    primary->lpVtbl->AddAttachedSurface(primary, peerProxy);
    bool mapped = surface.seenSurface == &peer.iface;
    primary->lpVtbl->DeleteAttachedSurface(primary, 17, peerProxy); mapped = mapped && surface.seenSurface == &peer.iface && surface.seenFlags == 17;
    primary->lpVtbl->BltFast(primary, 1, 2, peerProxy, nullptr, 19); mapped = mapped && surface.seenSurface == &peer.iface;
    primary->lpVtbl->Flip(primary, peerProxy, 23); mapped = mapped && surface.seenSurface == &peer.iface;
    primary->lpVtbl->UpdateOverlay(primary, nullptr, peerProxy, nullptr, 29, nullptr); mapped = mapped && surface.seenSurface == &peer.iface;
    primary->lpVtbl->UpdateOverlayZOrder(primary, 31, peerProxy); mapped = mapped && surface.seenSurface == &peer.iface;
    primary->lpVtbl->UpdateOverlayZOrder(primary, 0, nullptr); mapped = mapped && !surface.seenSurface;
    primary->lpVtbl->Initialize(primary, reinterpret_cast<IDirectDraw*>(draw), nullptr);
    mapped = mapped && surface.seenDraw == reinterpret_cast<IDirectDraw*>(&real.iface);
    IDirectDrawSurface7* duplicate = nullptr;
    draw->lpVtbl->DuplicateSurface(draw, peerProxy, &duplicate);
    mapped = mapped && real.seenSurface == &peer.iface && duplicate == &peer.iface;
    duplicate->lpVtbl->Release(duplicate);
    Check(mapped && !badNativeArguments, "direct nullable DD/surface arguments unwrap across typed forwarders");
    int data = 42;
    primary->lpVtbl->SetPrivateData(primary, otherSurfaceId, &data, sizeof(data), 0);
    Check(SameId(surface.seenGuid, otherSurfaceId), "REFGUID references survive typed argument remapping");
    surface.attached = &auxiliary;
    IDirectDrawSurface7* attached = nullptr;
    primary->lpVtbl->GetAttachedSurface(primary, nullptr, &attached);
    void* nativeDraw = nullptr;
    primary->lpVtbl->GetDDInterface(primary, &nativeDraw);
    Check(attached == &auxiliary.iface && nativeDraw == &real.iface && attached->lpVtbl == NativeSurfaceTable(),
          "documented GetAttachedSurface/GetDDInterface aliases remain native");
    attached->lpVtbl->Release(attached);
    static_cast<IDirectDraw7*>(nativeDraw)->lpVtbl->Release(static_cast<IDirectDraw7*>(nativeDraw));
    auto offscreen = SurfaceDescription(DDSCAPS_OFFSCREENPLAIN);
    real.nextSurface = &auxiliary;
    IDirectDrawSurface7* offscreenOutput = nullptr;
    draw->lpVtbl->CreateSurface(draw, &offscreen, &offscreenOutput, nullptr);
    Check(offscreenOutput == &auxiliary.iface && offscreenOutput->lpVtbl == NativeSurfaceTable(),
          "offscreen output for native D3D interoperability is never wrapped");
    offscreenOutput->lpVtbl->Release(offscreenOutput);
    peerProxy->lpVtbl->Release(peerProxy); primary->lpVtbl->Release(primary); draw->lpVtbl->Release(draw);
    Check(StatsAre(0, 0, 0, 0), "complete-forwarder test leaves no owned proxy objects");
}

void TestForwarderLoggingConfiguration() {
    ResetFixtures(); lakefix::ConfigureScalePresenter(nullptr, PreviousDiagnostic);
    FakeDraw real{}; Initialize(real); auto* draw = Wrap(real);
    real.onEvaluateMode = ReconfigureFromEvaluate;
    previousLoggerCalls = updatedLoggerCalls = nativeReconfigurations = 0;
    loggerReentryObserved = false;
    nativeReplacementLog = UpdatedDiagnostic;
    DWORD timeout = 0;
    SetLastError(0x8381);
    Check(draw->lpVtbl->EvaluateMode(draw, 31, &timeout) == S_FALSE && timeout == 48 && real.seenFlags == 31 &&
          real.incomingEvaluateError == 0x8381 && GetLastError() == nativeError &&
          nativeReconfigurations == 1 && !previousLoggerCalls && !updatedLoggerCalls,
          "successful forwarder preserves native arguments/LastError across reentrant configuration without logging");
    lakefix::ConfigureScalePresenter(nullptr, PreviousDiagnostic);
    nativeReplacementLog = nullptr; real.evaluateResult = E_FAIL;
    SetLastError(0x8382);
    Check(draw->lpVtbl->EvaluateMode(draw, 37, &timeout) == E_FAIL && timeout == 54 && real.seenFlags == 37 &&
          real.incomingEvaluateError == 0x8382 && GetLastError() == nativeError &&
          nativeReconfigurations == 2 && !previousLoggerCalls && !updatedLoggerCalls,
          "forwarder failure uses a logger disabled inside the native call rather than the prior snapshot");
    nativeReplacementLog = UpdatedDiagnostic;
    SetLastError(0x8383);
    Check(draw->lpVtbl->EvaluateMode(draw, 41, &timeout) == E_FAIL && timeout == 58 && real.seenFlags == 41 &&
          real.incomingEvaluateError == 0x8383 && GetLastError() == nativeError && nativeReconfigurations == 3 &&
          !previousLoggerCalls && updatedLoggerCalls == 1 && failureLogs == 1 && loggerReentryObserved,
          "forwarder failure uses the new logger outside the registry lock and preserves native HRESULT/LastError");
    real.evaluateResult = E_INVALIDARG;
    SetLastError(0x8384);
    Check(draw->lpVtbl->EvaluateMode(draw, 43, &timeout) == E_INVALIDARG && timeout == 60 &&
          real.incomingEvaluateError == 0x8384 && GetLastError() == nativeError &&
          nativeReconfigurations == 4 && !previousLoggerCalls && updatedLoggerCalls == 2,
          "a different forwarded HRESULT has its own bounded diagnostic rather than a global one-shot");
    draw->lpVtbl->Release(draw);
    lakefix::ConfigureScalePresenter(nullptr, nullptr);
    Check(StatsAre(0, 0, 0, 0), "reentrant forwarder logging test releases all proxy allocations");
}

void TestPresentationAndClipping() {
    ResetFixtures(); lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow, nullptr, 1024, 768);
    FakeDraw real{}; FakeSurface surface{}, source{}, auxiliary{}, peer{}; FakeClipper clipper{};
    Initialize(real); Initialize(surface); Initialize(source); Initialize(auxiliary); Initialize(peer); Initialize(clipper);
    auto* draw = Wrap(real);
    const DWORD coopFlags = DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE | DDSCL_ALLOWREBOOT;
    Check(draw->lpVtbl->SetCooperativeLevel(draw, Window(0x1234), coopFlags) == S_OK &&
          real.seenWindow == Window(0x1234) && real.seenFlags == coopFlags && GetLastError() == coopError,
          "cooperative level forwards flags and captures client HWND only on native success");
    auto description = SurfaceDescription(); const auto originalDescription = description;
    IDirectDrawSurface7* primary = nullptr;
    real.nextSurface = &surface; real.createResult = S_FALSE;
    auto* outer = &real.alias.iface;
    Check(draw->lpVtbl->CreateSurface(draw, &description, &primary, outer) == S_FALSE && primary != &surface.iface &&
          real.seenDescription == &description && real.seenOutput == &primary && real.seenOuter == outer &&
          GetLastError() == createError && std::memcmp(&description, &originalDescription, sizeof(description)) == 0 &&
          surface.iface.lpVtbl == NativeSurfaceTable(), "primary creation preserves native arguments/HRESULT and wraps only returned ownership");
    real.coopResult = E_ACCESSDENIED;
    Check(draw->lpVtbl->SetCooperativeLevel(draw, Window(0x9999), DDSCL_NORMAL) == E_ACCESSDENIED,
          "native cooperative failure is propagated without changing owner");
    RECT destination{100, 200, 1124, 968}; const RECT originalDestination = destination;
    DDBLTFX effects{}; effects.dwSize = sizeof(effects); effects.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
    const DWORD flags = DDBLT_WAIT | DDBLT_DDFX;
    SetLastError(0x8181);
    const HRESULT result = primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    const DWORD returnedError = GetLastError();
    Check(result == S_FALSE && surface.blts == 3 && surface.fills == 2 &&
          SameRect(LastCall(surface).destination, fittedViewport) && LastCall(surface).source == &source.iface &&
          !LastCall(surface).sourceRect && LastCall(surface).flags == flags && LastCall(surface).effects == &effects &&
          SameRect(destination, originalDestination), "full-source primary Blt scales destination and preserves source, flags, fx and HRESULT");
    Check(IsBlackStrip(surface.bltCalls[0], RECT{100, 200, 300, 1100}) &&
          IsBlackStrip(surface.bltCalls[1], RECT{1500, 200, 1700, 1100}), "real color fills remain inside host client letterbox strips");
    Check(source.descriptions == 1 && source.descriptionSizeCorrect && providerWidth == 1024 && providerHeight == 768 &&
          providerWindow == Window(0x1234) && LastCall(surface).incomingError == 0x8181 && returnedError == bltError &&
          activeLogs == 1 && failureLogs >= 2, "actual source query, client HWND and reentrant diagnostics preserve native LastError");
    ClearCalls(surface);
    primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    Check(surface.blts == 1 && !surface.fills && activeLogs == 1, "unchanged geometry avoids repeated fills and activation logs");
    source.width = 1920; source.height = 1080; fittedViewport = fullClient; ClearCalls(surface);
    const LONG creates = real.calls[6];
    const unsigned previousProviders = providerCalls, previousDescriptions = source.descriptions;
    primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    Check(providerCalls == previousProviders && source.descriptions == previousDescriptions + 1 &&
          LastCall(surface).destinationPointer == &destination && surface.blts == 1 && real.calls[6] == creates,
          "fresh nonconfigured dimensions remain native without recreating a surface or publishing geometry");
    source.width = 1024; source.height = 768;
    RECT crop{10, 20, 300, 400}; unsigned queries = source.descriptions, providers = providerCalls;
    ClearCalls(surface); primary->lpVtbl->Blt(primary, &destination, &source.iface, &crop, flags, &effects);
    Check(surface.blts == 1 && LastCall(surface).destinationPointer == &destination && LastCall(surface).sourceRect == &crop &&
          source.descriptions == queries && providerCalls == providers, "non-null sourceRect is unscaled exact argument pass-through");
    lakefix::ConfigureScalePresenter(nullptr, nullptr, nullptr); ClearCalls(surface);
    primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    Check(surface.blts == 1 && LastCall(surface).destinationPointer == &destination &&
          source.descriptions == queries && providerCalls == providers,
          "unconfigured presenter forwards full-source Blt without a descriptor query");
    lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow, nullptr, 1024, 768);
    providerEnabled = false; fittedViewport = RECT{-1000, -1000, 9000, 9000}; ClearCalls(surface);
    SetLastError(0x8282);
    Check(primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects) == S_FALSE &&
          surface.blts == 1 && !surface.fills && LastCall(surface).destinationPointer == &destination &&
          LastCall(surface).flags == flags && LastCall(surface).effects == &effects &&
          LastCall(surface).incomingError == 0x8282 && GetLastError() == bltError,
          "provider false forwards native Blt without geometry or LastError alterations");
    providerEnabled = true; ClearCalls(surface);
    primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    Check(surface.blts == 1 && !surface.fills && LastCall(surface).destinationPointer == &destination,
          "out-of-host fitted rectangle is rejected before painting");
    source.descriptionResult = E_FAIL; surface.presentationResult = DDERR_SURFACELOST; ClearCalls(surface);
    lakefix::ConfigureScalePresenter(Viewport, nullptr, ClipWindow, nullptr, 1024, 768);
    SetLastError(0x8283);
    Check(primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects) == DDERR_SURFACELOST &&
          surface.blts == 1 && !surface.fills && LastCall(surface).destinationPointer == &destination &&
          LastCall(surface).source == &source.iface && LastCall(surface).flags == flags && LastCall(surface).effects == &effects &&
          LastCall(surface).incomingError == 0x8283 && GetLastError() == bltError && !descriptionLogs,
          "unlogged descriptor failure preserves native Blt and does not consume its diagnostic");
    lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow, nullptr, 1024, 768); ClearCalls(surface);
    Check(primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects) == DDERR_SURFACELOST &&
          surface.blts == 1 && LastCall(surface).destinationPointer == &destination && descriptionLogs == 1 && GetLastError() == bltError,
          "GetSurfaceDesc failure logs and preserves the real fallback failure");
    ClearCalls(surface); SetLastError(0x8284);
    Check(primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects) == DDERR_SURFACELOST &&
          surface.blts == 1 && !surface.fills && LastCall(surface).destinationPointer == &destination &&
          LastCall(surface).incomingError == 0x8284 && GetLastError() == bltError && descriptionLogs == 1,
          "repeated descriptor failure remains bounded and still calls the real failing Blt");
    source.descriptionResult = S_OK; source.width = 0; ClearCalls(surface);
    // Keep the one-shot dimensions warning for the explicit replacement-logger
    // snapshot regression below; this case still checks native fail-open geometry.
    lakefix::ConfigureScalePresenter(Viewport, nullptr, ClipWindow, nullptr, 1024, 768);
    primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    Check(surface.blts == 1 && LastCall(surface).destinationPointer == &destination, "invalid source dimensions fail open to native Blt");
    lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow, nullptr, 1024, 768);
    source.width = 1024; source.height = 768; fittedViewport = fullClient; surface.presentationResult = E_INVALIDARG; ClearCalls(surface);
    Check(primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects) == E_INVALIDARG &&
          SameRect(LastCall(surface).destination, fittedViewport), "scaled presentation returns the negative native HRESULT");
    surface.presentationResult = S_FALSE; surface.fillResult = E_OUTOFMEMORY;
    fittedViewport = RECT{300, 200, 1500, 1100}; ClearCalls(surface);
    Check(primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects) == E_OUTOFMEMORY &&
          surface.blts == 1 && surface.fills == 1, "letterbox failure is not swallowed or forged into success");
    surface.fillResult = S_OK; ClearCalls(surface);
    primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    Check(surface.blts == 3 && surface.fills == 2, "failed letterbox geometry is retried");
    auto* peerProxy = Primary(draw, real, peer);
    fittedViewport = fullClient; ClearCalls(surface);
    primary->lpVtbl->Blt(primary, &destination, peerProxy, nullptr, flags, &effects);
    Check(LastCall(surface).source == &peer.iface && !peer.descriptions &&
          LastCall(surface).destinationPointer == &destination,
          "a known primary proxy source is unwrapped but never admitted as an offscreen frame");
    providerEnabled = false; ClearCalls(surface);
    Check(primary->lpVtbl->Blt(primary, &destination, peerProxy, nullptr, flags, &effects) == S_FALSE &&
          surface.blts == 1 && LastCall(surface).source == &peer.iface && LastCall(surface).destinationPointer == &destination,
          "declined presentation still unwraps a proxy source for the native call");
    providerEnabled = true;
    fittedViewport = RECT{300, 300, 1500, 1000}; nestedPrimary = primary; nestedSource = &source.iface;
    reenterProvider = true; providers = providerCalls; ClearCalls(surface);
    primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    Check(providerCalls == providers + 1 && nestedResult == S_FALSE && surface.blts == 6 &&
          surface.bltCalls[0].destinationPointer == &nestedDestination &&
          IsBlackStrip(surface.bltCalls[1], RECT{100, 200, 1700, 300}) &&
          IsBlackStrip(surface.bltCalls[2], RECT{100, 1000, 1700, 1100}) &&
          IsBlackStrip(surface.bltCalls[3], RECT{100, 300, 300, 1000}) &&
          IsBlackStrip(surface.bltCalls[4], RECT{1500, 300, 1700, 1000}),
          "reentrant provider Blt bypasses scaling and all four strips use native this");
    nestedPrimary = nestedSource = nullptr;
    real.coopResult = S_OK; draw->lpVtbl->SetCooperativeLevel(draw, Window(0x2345), DDSCL_NORMAL);
    ClearCalls(surface); primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    Check(providerWindow == Window(0x2345) && surface.fills == 4, "existing primary uses later successful client cooperation HWND");

    surface.clipper = &clipper; clipper.window = Window(0x2345); clipHost = Window(0x4567);
    SetLastError(0x8383);
    Check(primary->lpVtbl->SetClipper(primary, &clipper.iface) == S_OK && clipper.window == clipHost &&
          clipper.seenFlags == 0 && clipOwnerSeen == Window(0x2345) && surface.seenClipper == &clipper.iface &&
          surface.incomingClipperError == 0x8383 && GetLastError() == nativeError,
          "native clipper HWND moves from cooperative client to host without proxying clipper");
    IDirectDrawClipper* returnedClipper = nullptr;
    Check(primary->lpVtbl->GetClipper(primary, &returnedClipper) == S_OK && returnedClipper == &clipper.iface &&
          returnedClipper->lpVtbl == NativeClipperTable(), "GetClipper forwards native this, clipper and HRESULT");
    returnedClipper->lpVtbl->Release(returnedClipper);
    surface.getClipperResult = E_ACCESSDENIED; returnedClipper = nullptr;
    Check(primary->lpVtbl->GetClipper(primary, &returnedClipper) == E_ACCESSDENIED,
          "GetClipper failure remains the real HRESULT");
    surface.setClipperResult = E_INVALIDARG;
    Check(primary->lpVtbl->SetClipper(primary, nullptr) == E_INVALIDARG && !surface.seenClipper,
          "SetClipper forwards null and native failure unchanged");
    surface.setClipperResult = S_OK; clipper.window = Window(0x2345); clipper.setResult = E_ACCESSDENIED;
    LONG sets = surface.calls[28];
    Check(primary->lpVtbl->SetClipper(primary, &clipper.iface) == E_ACCESSDENIED && surface.calls[28] == sets &&
          GetLastError() == clipError, "native SetHWnd failure is propagated before surface SetClipper");
    clipper.setResult = S_OK; clipper.window = Window(0x7777); LONG hwndSets = clipper.calls[8];
    primary->lpVtbl->SetClipper(primary, &clipper.iface);
    Check(clipper.calls[8] == hwndSets && clipper.window == Window(0x7777), "foreign clipper window is never retargeted");
    clipper.window = Window(0x2345); clipper.getResult = E_FAIL;
    Check(primary->lpVtbl->SetClipper(primary, &clipper.iface) == S_OK && clipper.calls[8] == hwndSets,
          "failed clipper query leaves clipper untouched and forwards native SetClipper");
    clipper.getResult = S_OK; clipHost = nullptr;
    auto offscreen = SurfaceDescription(DDSCAPS_OFFSCREENPLAIN); real.nextSurface = &auxiliary;
    IDirectDrawSurface7* output = nullptr; draw->lpVtbl->CreateSurface(draw, &offscreen, &output, nullptr);
    providers = providerCalls; queries = source.descriptions;
    output->lpVtbl->Blt(output, &destination, &source.iface, nullptr, flags, &effects);
    Check(output == &auxiliary.iface && providerCalls == providers && source.descriptions == queries &&
          LastCall(auxiliary).destinationPointer == &destination, "offscreen Blt bypasses every presentation hook");
    real.createResult = E_FAIL;
    Check(draw->lpVtbl->CreateSurface(draw, &description, &output, nullptr) == E_FAIL && !output,
          "CreateSurface failure remains native and produces no proxy");
    peerProxy->lpVtbl->Release(peerProxy);
    draw->lpVtbl->Release(draw);
    ClearCalls(surface); primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    Check(providerWindow == Window(0x2345) && StatsAre(0, 1, 1, 1), "primary keeps small owner context after DD proxy reference lifetime ends");
    primary->lpVtbl->Release(primary); source.iface.lpVtbl->Release(&source.iface);
    auxiliary.iface.lpVtbl->Release(&auxiliary.iface); clipper.iface.lpVtbl->Release(&clipper.iface);
    Check(StatsAre(0, 0, 0, 0), "presentation and clipper fixtures release all proxy allocations");
}

bool fallbackPaintSuccess = true;
unsigned fallbackPaintCalls = 0;
bool FallbackViewport(HWND, LONG, LONG, RECT* full, RECT* fitted) noexcept {
    *full = fullClient; *fitted = fittedViewport; return true;
}
bool FallbackPaint(HWND window, const RECT* strips, UINT count) noexcept {
    ++fallbackPaintCalls;
    if (!window || !strips || count != 4) return false;
    for (UINT i = 0; i < count; ++i)
        if (strips[i].left < fullClient.left || strips[i].top < fullClient.top ||
            strips[i].right > fullClient.right || strips[i].bottom > fullClient.bottom) return false;
    SetLastError(0x9191);
    return fallbackPaintSuccess;
}
void TestLetterboxFallback() {
    ResetFixtures();
    FakeDraw native{}; FakeSurface realPrimary{}, source{};
    Initialize(native); Initialize(realPrimary); Initialize(source);
    auto* draw = Wrap(native);
    draw->lpVtbl->SetCooperativeLevel(draw, Window(0x1234), DDSCL_NORMAL);
    auto* primary = Primary(draw, native, realPrimary);
    fullClient = {100, 200, 1700, 1100}; fittedViewport = {300, 200, 1500, 1100};
    fallbackPaintSuccess = true; fallbackPaintCalls = 0;
    realPrimary.fillResult = DDERR_EXCEPTION;
    lakefix::ConfigureScalePresenter(FallbackViewport, nullptr, nullptr, FallbackPaint, 1024, 768);
    RECT original{0, 0, 1024, 768};
    Check(primary->lpVtbl->Blt(primary, &original, &source.iface, nullptr, DDBLT_WAIT, nullptr) == S_FALSE &&
          fallbackPaintCalls == 1 && realPrimary.fills == 1 && realPrimary.blts == 2 &&
          SameRect(LastCall(realPrimary).destination, fittedViewport),
          "owned-window fallback handles COLORFILL exception and returns real presentation result");
    OffsetRect(&fullClient, 10, 10); OffsetRect(&fittedViewport, 10, 10);
    ClearCalls(realPrimary);
    Check(primary->lpVtbl->Blt(primary, &original, &source.iface, nullptr, DDBLT_WAIT, nullptr) == S_FALSE &&
          fallbackPaintCalls == 2 && !realPrimary.fills && realPrimary.blts == 1,
          "subsequent geometry changes avoid unsupported native primary fills");
    const RECT letterboxClient = fullClient, letterboxViewport = fittedViewport, originalDestination = original;
    DDBLTFX effects{}; effects.dwSize = sizeof(effects); effects.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
    const DWORD flags = DDBLT_WAIT | DDBLT_DDFX;
    const unsigned queries = source.descriptions;
    fittedViewport = fullClient; fallbackPaintSuccess = false; ClearCalls(realPrimary);
    SetLastError(0x9291);
    Check(primary->lpVtbl->Blt(primary, &original, &source.iface, nullptr, flags, &effects) == S_FALSE &&
          fallbackPaintCalls == 2 && !realPrimary.fills && realPrimary.blts == 1 && source.descriptions == queries + 1 &&
          SameRect(LastCall(realPrimary).destination, fullClient) && LastCall(realPrimary).source == &source.iface &&
          !LastCall(realPrimary).sourceRect && LastCall(realPrimary).flags == flags && LastCall(realPrimary).effects == &effects &&
          SameRect(original, originalDestination) && LastCall(realPrimary).incomingError == 0x9291 && GetLastError() == bltError,
          "latched fallback with empty strips skips painter/fills but presents with real arguments and LastError");
    OffsetRect(&fullClient, 5, 5); fittedViewport = fullClient;
    realPrimary.presentationResult = E_FAIL; ClearCalls(realPrimary); SetLastError(0x9292);
    Check(primary->lpVtbl->Blt(primary, &original, &source.iface, nullptr, flags, &effects) == E_FAIL &&
          fallbackPaintCalls == 2 && !realPrimary.fills && realPrimary.blts == 1 && source.descriptions == queries + 2 &&
          SameRect(LastCall(realPrimary).destination, fullClient) && LastCall(realPrimary).source == &source.iface &&
          !LastCall(realPrimary).sourceRect && LastCall(realPrimary).flags == flags && LastCall(realPrimary).effects == &effects &&
          LastCall(realPrimary).incomingError == 0x9292 && GetLastError() == bltError,
          "empty strips after geometry change never mask the real presentation E_FAIL or LastError");
    lakefix::ConfigureScalePresenter(FallbackViewport, nullptr, nullptr, nullptr, 1024, 768);
    realPrimary.presentationResult = S_FALSE; ClearCalls(realPrimary); SetLastError(0x9293);
    Check(primary->lpVtbl->Blt(primary, &original, &source.iface, nullptr, flags, &effects) == S_FALSE &&
          fallbackPaintCalls == 2 && !realPrimary.fills && realPrimary.blts == 1 && source.descriptions == queries + 3 &&
          SameRect(LastCall(realPrimary).destination, fullClient) && LastCall(realPrimary).source == &source.iface &&
          !LastCall(realPrimary).sourceRect && LastCall(realPrimary).flags == flags && LastCall(realPrimary).effects == &effects &&
          LastCall(realPrimary).incomingError == 0x9293 && GetLastError() == bltError,
          "latched fallback with no painter and empty strips cannot manufacture a fill failure");
    OffsetRect(&fullClient, 5, 5); fittedViewport = fullClient;
    realPrimary.presentationResult = E_FAIL; ClearCalls(realPrimary); SetLastError(0x9294);
    Check(primary->lpVtbl->Blt(primary, &original, &source.iface, nullptr, flags, &effects) == E_FAIL &&
          fallbackPaintCalls == 2 && !realPrimary.fills && realPrimary.blts == 1 && source.descriptions == queries + 4 &&
          SameRect(LastCall(realPrimary).destination, fullClient) && LastCall(realPrimary).source == &source.iface &&
          !LastCall(realPrimary).sourceRect && LastCall(realPrimary).flags == flags && LastCall(realPrimary).effects == &effects &&
          LastCall(realPrimary).incomingError == 0x9294 && GetLastError() == bltError,
          "missing painter with empty strips still returns the actual native E_FAIL and LastError");
    fullClient = letterboxClient; fittedViewport = letterboxViewport;
    lakefix::ConfigureScalePresenter(FallbackViewport, nullptr, nullptr, FallbackPaint, 1024, 768);
    fallbackPaintSuccess = true; realPrimary.presentationResult = E_ACCESSDENIED; ClearCalls(realPrimary);
    Check(primary->lpVtbl->Blt(primary, &original, &source.iface, nullptr, DDBLT_WAIT, nullptr) == E_ACCESSDENIED &&
          fallbackPaintCalls == 3 && !realPrimary.fills && realPrimary.blts == 1,
          "letterbox fallback never hides a real presentation failure");
    fallbackPaintSuccess = false; ClearCalls(realPrimary);
    Check(primary->lpVtbl->Blt(primary, &original, &source.iface, nullptr, DDBLT_WAIT, nullptr) == DDERR_EXCEPTION &&
          fallbackPaintCalls == 5 && realPrimary.fills == 1 && realPrimary.blts == 1,
          "failed owned-window fallback preserves actual fill failure");
    primary->lpVtbl->Release(primary); draw->lpVtbl->Release(draw);
    source.iface.lpVtbl->Release(&source.iface);
    lakefix::ConfigureScalePresenter(nullptr, nullptr);
    Check(StatsAre(0, 0, 0, 0), "fallback test releases every proxy/context");
}

void TestRestoredLetterbox() {
    ResetFixtures(); lakefix::ConfigureScalePresenter(FallbackViewport, nullptr, nullptr, nullptr, 1024, 768);
    FakeDraw real{}, other{}; FakeSurface surface{}, source{}, sibling{}, otherSurface{};
    Initialize(real); Initialize(other); Initialize(surface); Initialize(source); Initialize(sibling); Initialize(otherSurface);
    auto* draw = Wrap(real); auto* otherDraw = Wrap(other);
    draw->lpVtbl->SetCooperativeLevel(draw, Window(0x1234), DDSCL_NORMAL);
    otherDraw->lpVtbl->SetCooperativeLevel(otherDraw, Window(0x2345), DDSCL_NORMAL);
    auto* primary = Primary(draw, real, surface); auto* otherPrimary = Primary(otherDraw, other, otherSurface);
    auto* siblingPrimary = Primary(draw, real, sibling);
    fullClient = {100, 200, 1700, 1100}; fittedViewport = {300, 200, 1500, 1100};
    RECT destination{0, 0, 1024, 768};
    const auto present = [&](IDirectDrawSurface7* output) {
        return output->lpVtbl->Blt(output, &destination, &source.iface, nullptr, DDBLT_WAIT, nullptr);
    };
    present(primary); present(siblingPrimary); present(otherPrimary);
    ClearCalls(surface); ClearCalls(sibling); ClearCalls(otherSurface);
    SetLastError(0xb101);
    Check(primary->lpVtbl->Restore(primary) == S_OK && surface.incomingRestoreError == 0xb101 &&
          GetLastError() == nativeError && !surface.letterboxContents,
          "Restore forwards native this/HRESULT/LastError and the fixture loses previous pixel contents");
    present(primary);
    Check(surface.fills == 2 && surface.letterboxContents,
          "REGRESSION successful Restore repaints unchanged letterbox geometry");
    present(siblingPrimary);
    Check(!sibling.fills && sibling.letterboxContents, "surface Restore does not invalidate an explicitly independent sibling surface");
    ClearCalls(surface); surface.restoreResult = DDERR_WRONGMODE;
    Check(primary->lpVtbl->Restore(primary) == DDERR_WRONGMODE, "Restore failure preserves exact native HRESULT");
    present(primary);
    Check(!surface.fills, "failed Restore does not discard a valid unchanged letterbox cache");
    ClearCalls(surface); SetLastError(0xb102);
    Check(draw->lpVtbl->RestoreAllSurfaces(draw) == S_OK && real.incomingRestoreAllError == 0xb102 &&
          GetLastError() == nativeError && !surface.letterboxContents && !sibling.letterboxContents && otherSurface.letterboxContents,
          "RestoreAllSurfaces forwards exactly and loses only its owner's surface contents");
    present(primary); present(siblingPrimary); present(otherPrimary);
    Check(surface.fills == 2 && sibling.fills == 2 && surface.letterboxContents && sibling.letterboxContents && !otherSurface.fills,
          "REGRESSION successful RestoreAllSurfaces invalidates only owner letterbox caches");
    ClearCalls(surface); real.restoreAllResult = DDERR_INVALIDOBJECT;
    Check(draw->lpVtbl->RestoreAllSurfaces(draw) == DDERR_INVALIDOBJECT, "RestoreAllSurfaces failure remains native");
    present(primary); Check(!surface.fills, "failed RestoreAllSurfaces retains unchanged valid cache");
    surface.dcDrewOverLetterbox = true; ClearCalls(surface);
    auto dc = reinterpret_cast<HDC>(static_cast<UINT_PTR>(0x9876)); SetLastError(0xb103);
    Check(primary->lpVtbl->ReleaseDC(primary, dc) == S_OK && surface.seenDC == dc &&
          surface.incomingReleaseDCError == 0xb103 && GetLastError() == nativeError && !surface.letterboxContents,
          "ReleaseDC forwards the exact HDC/result/LastError after synthetic DC drawing over the bars");
    present(primary);
    Check(surface.fills == 2 && surface.letterboxContents,
          "REGRESSION successful ReleaseDC repaints bars changed through the primary DC");
    ClearCalls(surface); surface.releaseDCResult = E_INVALIDARG;
    Check(primary->lpVtbl->ReleaseDC(primary, dc) == E_INVALIDARG, "ReleaseDC failure is not hidden");
    present(primary); Check(!surface.fills, "failed ReleaseDC keeps valid letterbox cache");
    primary->lpVtbl->Release(primary); siblingPrimary->lpVtbl->Release(siblingPrimary); otherPrimary->lpVtbl->Release(otherPrimary);
    draw->lpVtbl->Release(draw); otherDraw->lpVtbl->Release(otherDraw); source.iface.lpVtbl->Release(&source.iface);
    Check(StatsAre(0, 0, 0, 0), "restore/DC cache regressions release every proxy and owner");
}

void TestFrameAdmission() {
    ResetFixtures(); lakefix::ConfigureScalePresenter(Viewport, nullptr, nullptr, nullptr, 1024, 768);
    FakeDraw real{}; FakeSurface surface{}, source{};
    Initialize(real); Initialize(surface); Initialize(source);
    auto* draw = Wrap(real); draw->lpVtbl->SetCooperativeLevel(draw, Window(0x1234), DDSCL_NORMAL);
    auto* primary = Primary(draw, real, surface);
    providerEnabled = true; fullClient = {100, 200, 1700, 1100}; fittedViewport = {300, 200, 1500, 1100};
    RECT destination{0, 0, 1024, 768}, partial{0, 0, 640, 480};
    const auto nativeOnly = [&](RECT* target, const char* message) {
        ClearCalls(surface); const auto providers = providerCalls; SetLastError(0xb201);
        const HRESULT result = primary->lpVtbl->Blt(primary, target, &source.iface, nullptr, DDBLT_WAIT, nullptr);
        Check(result == S_FALSE && surface.blts == 1 && !surface.fills && providerCalls == providers &&
              LastCall(surface).destinationPointer == target && LastCall(surface).source == &source.iface &&
              LastCall(surface).incomingError == 0xb201 && GetLastError() == bltError, message);
    };
    nativeOnly(&partial, "REGRESSION partial primary destination remains native without consulting viewport");
    nativeOnly(nullptr, "REGRESSION unknown whole-primary destination role remains native");
    source.width = 640; source.height = 480;
    nativeOnly(&destination, "REGRESSION auxiliary offscreen dimensions cannot be treated as a configured frame");
    source.width = 1024; source.height = 768; source.caps = DDSCAPS_PRIMARYSURFACE;
    nativeOnly(&destination, "REGRESSION a primary source is not an offscreen presentation frame");
    source.caps = DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE;
    nativeOnly(&destination, "REGRESSION a same-size texture source is forwarded without scaling");
    source.caps = DDSCAPS_OFFSCREENPLAIN; source.descriptionFlags = DDSD_WIDTH | DDSD_HEIGHT;
    nativeOnly(&destination, "REGRESSION missing native source role/caps is not guessed");
    source.descriptionFlags |= DDSD_CAPS; source.caps |= DDSCAPS_MIPMAP;
    nativeOnly(&destination, "unknown extra native source capabilities keep exact native forwarding");
    source.caps = DDSCAPS_OFFSCREENPLAIN; source.caps2 = DDSCAPS2_TEXTUREMANAGE;
    nativeOnly(&destination, "unknown secondary source role is not treated as an admitted plain frame");
    source.caps2 = 0;
    ClearCalls(surface); const auto providers = providerCalls, queries = source.descriptions;
    RECT fullSource{0, 0, 1024, 768};
    primary->lpVtbl->Blt(primary, &destination, &source.iface, &fullSource, DDBLT_WAIT, nullptr);
    Check(LastCall(surface).sourceRect == &fullSource && LastCall(surface).destinationPointer == &destination &&
          providerCalls == providers && source.descriptions == queries,
          "even explicit full sourceRect preserves the established exact native non-null-rectangle contract");
    ClearCalls(surface);
    primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, DDBLT_KEYSRC | DDBLT_WAIT, nullptr);
    Check(LastCall(surface).flags == (DDBLT_KEYSRC | DDBLT_WAIT) && LastCall(surface).destinationPointer == &destination &&
          providerCalls == providers && source.descriptions == queries,
          "unadmitted keyed/auxiliary Blt flags never change presentation or publish geometry");
    primary->lpVtbl->Release(primary); draw->lpVtbl->Release(draw); source.iface.lpVtbl->Release(&source.iface);
    Check(StatsAre(0, 0, 0, 0), "frame-admission regressions leave no proxy/owner allocations");
}

void TestSurfaceIndependentLifetime() {
    ResetFixtures(); lakefix::ConfigureScalePresenter(nullptr, nullptr, nullptr);
    FakeDraw real{}; FakeSurface surface{}; Initialize(real); Initialize(surface);
    auto* draw = Wrap(real); auto* primary = Primary(draw, real, surface);
    void* alias = nullptr;
    primary->lpVtbl->QueryInterface(primary, otherSurfaceId, &alias);
    Check(primary->lpVtbl->AddRef(primary) == 3 && primary->lpVtbl->Release(primary) == 2,
          "surface AddRef/Release expose native ULONG while counting proxy API references separately");
    Check(primary->lpVtbl->Release(primary) == 1 && surface.references == 1 && !surface.destroyed && StatsAre(1, 0, 1, 1),
          "surface proxy is destroyed at zero proxy refs despite surviving native alias count");
    Check(static_cast<IUnknown*>(alias)->lpVtbl->Release(static_cast<IUnknown*>(alias)) == 0 && surface.destroyed,
          "native surface alias remains valid after primary proxy destruction");
    draw->lpVtbl->Release(draw);
    Check(StatsAre(0, 0, 0, 0), "independent surface-reference test cleans up");
}

IDirectDraw7* rebornDraw = nullptr;
IDirectDrawSurface7* rebornSurface = nullptr;
IDirectDraw7* rebirthOwner = nullptr;
FakeDraw* rebirthNativeOwner = nullptr;
bool pinnedDrawObserved = false, pinnedSurfaceObserved = false;
void RebirthDraw(FakeDraw* real) noexcept {
    Initialize(*real); rebornDraw = Wrap(*real);
    pinnedDrawObserved = StatsAre(1, 0, 2, 2);
    rebornDraw->lpVtbl->SetCooperativeLevel(rebornDraw, Window(0x3456), DDSCL_NORMAL);
}
void RebirthSurface(FakeSurface* real) noexcept {
    Initialize(*real); rebornSurface = Primary(rebirthOwner, *rebirthNativeOwner, *real);
    pinnedSurfaceObserved = StatsAre(1, 1, 3, 1);
}
IDirectDrawSurface7* releaseInsideBlt = nullptr;
bool activeBltPinObserved = false;
void ReleaseFromBlt(FakeSurface* real) noexcept {
    real->onBlt = nullptr;
    releaseInsideBlt->lpVtbl->Release(releaseInsideBlt);
    activeBltPinObserved = StatsAre(1, 0, 2, 1);
}
void TestReentrancyAndReuse() {
    ResetFixtures(); lakefix::ConfigureScalePresenter(nullptr, nullptr, nullptr);
    FakeDraw real{}; FakeSurface surface{}; Initialize(real); Initialize(surface);
    auto* draw = Wrap(real); real.onFinalRelease = RebirthDraw;
    Check(draw->lpVtbl->Release(draw) == 0 && rebornDraw && pinnedDrawObserved && StatsAre(1, 0, 1, 1),
          "native address reused during final Release cannot retire the new DD proxy generation");
    draw = rebornDraw; auto* primary = Primary(draw, real, surface);
    rebirthOwner = draw; rebirthNativeOwner = &real; surface.onFinalRelease = RebirthSurface;
    Check(primary->lpVtbl->Release(primary) == 0 && rebornSurface && pinnedSurfaceObserved && StatsAre(1, 1, 2, 1),
          "native surface reuse during Release keeps old call pin and new proxy independent");
    primary = rebornSurface;
    // A native alias holds the real object while its API proxy is released from
    // inside an in-flight native method; metadata must survive until return.
    void* alias = nullptr; primary->lpVtbl->QueryInterface(primary, otherSurfaceId, &alias);
    releaseInsideBlt = primary; surface.onBlt = ReleaseFromBlt;
    Check(primary->lpVtbl->Blt(primary, nullptr, nullptr, nullptr, 0, nullptr) == S_FALSE && activeBltPinObserved &&
          StatsAre(1, 0, 1, 1), "active native-call pin survives reentrant final proxy Release");
    static_cast<IUnknown*>(alias)->lpVtbl->Release(static_cast<IUnknown*>(alias));
    draw->lpVtbl->Release(draw);
    bool clean = StatsAre(0, 0, 0, 0);
    for (unsigned index = 0; index != 64; ++index) {
        Initialize(real); Initialize(surface); draw = Wrap(real); primary = Primary(draw, real, surface);
        primary->lpVtbl->Release(primary); draw->lpVtbl->Release(draw);
        clean = StatsAre(0, 0, 0, 0) && clean;
    }
    Check(clean, "repeated native-address reuse retains no dead proxies or owner contexts");
}

void TestBounds() {
    ResetFixtures(); lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow, nullptr, 1024, 768);
    lakefix::ScalePresenterResources excessive{};
    excessive.drawCapacity = excessive.primaryCapacity = ~0UL;
    lakefix::ConfigureScalePresenterResources(excessive); // Resource injection cannot widen production hard bounds.
    FakeDraw native[65]{}; IDirectDraw7* outputs[65]{};
    bool success = true;
    for (unsigned index = 0; index != 65; ++index) {
        Initialize(native[index]); void* owned = &native[index].iface;
        const bool wrapped = lakefix::WrapDirectDraw7(&owned);
        outputs[index] = static_cast<IDirectDraw7*>(owned);
        success = (index < 64 ? wrapped && owned != &native[index].iface : !wrapped && owned == &native[index].iface) && success;
    }
    Check(success && StatsAre(64, 0, 64, 64) && native[64].references == 1,
          "low-level DD transfer bound cannot be widened and preserves caller-owned failure output/reference");
    for (auto* output : outputs) output->lpVtbl->Release(output);
    Check(StatsAre(0, 0, 0, 0), "DD bound failure frees unused candidates");
    ResetFixtures(); FakeDraw real{}; Initialize(real); auto* draw = Wrap(real);
    std::unique_ptr<FakeSurface[]> nativeSurfaces(new (std::nothrow) FakeSurface[257]);
    Check(nativeSurfaces != nullptr, "primary capacity fixtures allocate without GPU");
    if (!nativeSurfaces) { draw->lpVtbl->Release(draw); return; }
    IDirectDrawSurface7* surfaceOutputs[257]{}; success = true;
    real.createResult = S_FALSE;
    auto description = SurfaceDescription();
    for (unsigned index = 0; index != 257; ++index) {
        Initialize(nativeSurfaces[index]);
        real.nextSurface = &nativeSurfaces[index];
        const HRESULT created = draw->lpVtbl->CreateSurface(draw, &description, &surfaceOutputs[index], nullptr);
        success = (index < 256 ? created == S_FALSE && surfaceOutputs[index] != &nativeSurfaces[index].iface :
                   FAILED(created) && !surfaceOutputs[index]) && GetLastError() == createError && success;
    }
    Check(success && StatsAre(1, 256, 257, 1) && nativeSurfaces[256].references == 0 && nativeSurfaces[256].destroyed,
          "REGRESSION primary proxy capacity failure returns failure/null and releases unpresentable native ownership");
    for (auto* output : surfaceOutputs) if (output) output->lpVtbl->Release(output);
    draw->lpVtbl->Release(draw);
    lakefix::ConfigureScalePresenterResources({});
    Check(StatsAre(0, 0, 0, 0), "primary capacity failure leaves no orphan allocations");
}

struct WrapTask { FakeDraw* real; IDirectDraw7* expected; volatile LONG failed = 0; };
DWORD WINAPI WrapWorker(LPVOID argument) {
    auto* task = static_cast<WrapTask*>(argument);
    for (unsigned index = 0; index != 100; ++index) {
        DrawAddRef(&task->real->iface);
        void* owned = &task->real->iface;
        const bool wrapped = lakefix::WrapDirectDraw7(&owned);
        auto* proxy = static_cast<IDirectDraw7*>(owned);
        if (!wrapped || proxy != task->expected || proxy->lpVtbl->Compact(proxy) != S_FALSE) InterlockedIncrement(&task->failed);
        proxy->lpVtbl->Release(proxy);
    }
    return 0;
}
void TestConcurrentTransfers() {
    ResetFixtures(); lakefix::ConfigureScalePresenter(nullptr, nullptr, nullptr);
    FakeDraw real{}; Initialize(real); auto* draw = Wrap(real); WrapTask task{&real, draw};
    HANDLE threads[4]{}; bool joined = true;
    for (HANDLE& thread : threads) { thread = CreateThread(nullptr, 0, WrapWorker, &task, 0, nullptr); if (!thread) joined = false; }
    for (HANDLE thread : threads) if (thread) {
        if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) joined = false;
        CloseHandle(thread);
    }
    Check(joined && !task.failed && real.references == 1 && StatsAre(1, 0, 1, 1),
          "concurrent owned-native transfers share one proxy and balance independent native references");
    draw->lpVtbl->Release(draw);
    Check(StatsAre(0, 0, 0, 0), "concurrent transfer test releases registry ownership");
}

void TestBltSnapshotArguments() {
    ResetFixtures(); lakefix::ConfigureScalePresenter(nullptr, nullptr, nullptr);
    FakeDraw real{}; FakeSurface destinationSurface{}, source{};
    Initialize(real); Initialize(destinationSurface); Initialize(source);
    auto* draw = Wrap(real);
    auto* primary = Primary(draw, real, destinationSurface);
    auto* sourceProxy = Primary(draw, real, source);
    RECT destination{17, 29, 1041, 797}, crop{3, 5, 101, 203};
    const RECT originalDestination = destination, originalCrop = crop;
    DDBLTFX effects{}; effects.dwSize = sizeof(effects); effects.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
    const auto originalEffects = effects;
    const DWORD flags = DDBLT_WAIT | DDBLT_DDFX;
    const LONG destinationAdds = destinationSurface.calls[1], sourceAdds = source.calls[1];
    for (unsigned mode = 0; mode != 4; ++mode) {
        auto* input = mode & 1 ? sourceProxy : &source.iface;
        LPRECT inputRect = mode & 2 ? &crop : nullptr;
        const HRESULT expected = mode & 2 ? E_INVALIDARG : S_FALSE;
        const DWORD incomingError = 0xa101 + mode;
        destinationSurface.presentationResult = expected; ClearCalls(destinationSurface);
        SetLastError(incomingError);
        const HRESULT result = primary->lpVtbl->Blt(primary, &destination, input, inputRect, flags, &effects);
        const DWORD returnedError = GetLastError();
        Check(result == expected && destinationSurface.blts == 1 && !destinationSurface.fills &&
              LastCall(destinationSurface).destinationPointer == &destination &&
              LastCall(destinationSurface).source == &source.iface && LastCall(destinationSurface).sourceRect == inputRect &&
              LastCall(destinationSurface).flags == flags && LastCall(destinationSurface).effects == &effects &&
              LastCall(destinationSurface).incomingError == incomingError && returnedError == bltError &&
              SameRect(destination, originalDestination) && SameRect(crop, originalCrop) &&
              std::memcmp(&effects, &originalEffects, sizeof(effects)) == 0,
              "Blt snapshot preserves all native/proxy source arguments, native success/failure and LastError");
        Check(!source.descriptions && !destinationSurface.descriptions && destinationSurface.calls[1] == destinationAdds &&
              source.calls[1] == sourceAdds && source.references == 1 && destinationSurface.references == 1 &&
              StatsAre(1, 2, 3, 1), "disabled Blt snapshots neither query dimensions nor acquire hidden native references");
    }
    lakefix::ConfigureScalePresenter(Viewport, nullptr, nullptr, nullptr, 1024, 768);
    const unsigned providers = providerCalls;
    ClearCalls(destinationSurface); SetLastError(0xa105);
    Check(primary->lpVtbl->Blt(primary, &destination, sourceProxy, &crop, flags, &effects) == E_INVALIDARG &&
          destinationSurface.blts == 1 && LastCall(destinationSurface).destinationPointer == &destination &&
          LastCall(destinationSurface).source == &source.iface && LastCall(destinationSurface).sourceRect == &crop &&
          LastCall(destinationSurface).flags == flags && LastCall(destinationSurface).effects == &effects &&
          LastCall(destinationSurface).incomingError == 0xa105 && GetLastError() == bltError &&
          !source.descriptions && providerCalls == providers,
          "configured cropped Blt keeps exact proxy-unwrapped pass-through without descriptor/provider calls");

    // Call the known proxy entry point, never an unregistered object's table.
    // Its source is deliberately unreadable: invalid self must reject before
    // dereferencing either input or forwarding anything to a native fixture.
    const auto blt = primary->lpVtbl->Blt;
    IDirectDrawSurface7 unregistered{};
    auto* unreadableSource = reinterpret_cast<IDirectDrawSurface7*>(static_cast<UINT_PTR>(1));
    const LONG nativeBlts = destinationSurface.calls[5];
    SetLastError(0xa106);
    Check(blt(&unregistered, &destination, unreadableSource, nullptr, flags, &effects) == E_UNEXPECTED &&
          GetLastError() == 0xa106 && !unregistered.lpVtbl && destinationSurface.calls[5] == nativeBlts &&
          !source.descriptions && providerCalls == providers && StatsAre(1, 2, 3, 1),
          "unknown Blt self is rejected without reading its source, native dispatch or LastError changes");
    SetLastError(0xa107);
    Check(blt(nullptr, nullptr, unreadableSource, &crop, flags, &effects) == E_UNEXPECTED &&
          GetLastError() == 0xa107 && destinationSurface.calls[5] == nativeBlts && StatsAre(1, 2, 3, 1),
          "null Blt self cannot dispatch or retain any snapshot pins");
    sourceProxy->lpVtbl->Release(sourceProxy); primary->lpVtbl->Release(primary); draw->lpVtbl->Release(draw);
    lakefix::ConfigureScalePresenter(nullptr, nullptr, nullptr);
    Check(StatsAre(0, 0, 0, 0), "Blt argument and invalid-self snapshots release every proxy/context");
}

struct BltPinObservation {
    IDirectDrawSurface7* destination = nullptr;
    IDirectDrawSurface7* source = nullptr;
    FakeSurface* nativeDestination = nullptr;
    FakeSurface* nativeSource = nullptr;
    ULONG destinationRelease = 0, sourceRelease = 0;
    bool pinsAlive = false, nativesAlive = false;
} bltPinObservation;
void ReleaseSnapshotInputsFromBlt(FakeSurface* real) noexcept {
    real->onBlt = nullptr;
    const auto destination = bltPinObservation.destination, source = bltPinObservation.source;
    const bool same = destination == source;
    bltPinObservation.destinationRelease = destination->lpVtbl->Release(destination);
    bltPinObservation.sourceRelease = same ? bltPinObservation.destinationRelease : source->lpVtbl->Release(source);
    bltPinObservation.pinsAlive = StatsAre(1, 0, same ? 2 : 3, 1);
    bltPinObservation.nativesAlive = bltPinObservation.nativeDestination->references == 1 &&
        bltPinObservation.nativeSource->references == 1 && !bltPinObservation.nativeDestination->destroyed &&
        !bltPinObservation.nativeSource->destroyed;
    SetLastError(0xa210);
}
bool SnapshotPinViewport(HWND, LONG, LONG, RECT* full, RECT* fitted) noexcept {
    *full = *fitted = RECT{40, 50, 840, 650};
    (void)lakefix::GetScalePresenterStats();
    SetLastError(0xa211);
    return true;
}
void TestBltSnapshotPins() {
    for (unsigned mode = 0; mode != 2; ++mode) {
        const bool same = mode != 0;
        ResetFixtures(); lakefix::ConfigureScalePresenter(SnapshotPinViewport, nullptr, nullptr, nullptr, 1024, 768);
        FakeDraw real{}; FakeSurface destinationSurface{}, source{};
        Initialize(real); Initialize(destinationSurface); if (!same) Initialize(source);
        auto* draw = Wrap(real);
        draw->lpVtbl->SetCooperativeLevel(draw, Window(0x4567), DDSCL_NORMAL);
        auto* primary = Primary(draw, real, destinationSurface);
        auto* sourceProxy = same ? primary : Primary(draw, real, source);
        auto& nativeSource = same ? destinationSurface : source;
        // Metadata pins do NOT own native references. These separate native
        // references make both reentrant final API Releases valid COM usage.
        destinationSurface.iface.lpVtbl->AddRef(&destinationSurface.iface);
        if (!same) source.iface.lpVtbl->AddRef(&source.iface);
        const LONG destinationAdds = destinationSurface.calls[1], sourceAdds = nativeSource.calls[1];
        bltPinObservation = {};
        bltPinObservation.destination = primary; bltPinObservation.source = sourceProxy;
        bltPinObservation.nativeDestination = &destinationSurface; bltPinObservation.nativeSource = &nativeSource;
        destinationSurface.onBlt = ReleaseSnapshotInputsFromBlt;
        RECT destination{11, 13, 1035, 781}; const RECT originalDestination = destination;
        DDBLTFX effects{}; effects.dwSize = sizeof(effects); effects.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
        const DWORD flags = DDBLT_WAIT | DDBLT_DDFX, incomingError = 0xa201 + mode;
        SetLastError(incomingError);
        const HRESULT result = primary->lpVtbl->Blt(primary, &destination, sourceProxy, nullptr, flags, &effects);
        const DWORD returnedError = GetLastError();
        Check(bltPinObservation.destinationRelease == 1 && bltPinObservation.sourceRelease == 1 &&
              bltPinObservation.pinsAlive && bltPinObservation.nativesAlive,
              "snapshot retains destination and mapped-source metadata across reentrant final API Releases, including self-source");
        Check(result == S_FALSE && destinationSurface.blts == 1 && !destinationSurface.fills &&
              LastCall(destinationSurface).destinationPointer == &destination &&
              LastCall(destinationSurface).source == &nativeSource.iface && !LastCall(destinationSurface).sourceRect &&
              LastCall(destinationSurface).flags == flags && LastCall(destinationSurface).effects == &effects &&
              LastCall(destinationSurface).incomingError == incomingError && returnedError == bltError &&
              SameRect(destination, originalDestination) && !nativeSource.descriptions &&
              destinationSurface.calls[1] == destinationAdds && nativeSource.calls[1] == sourceAdds && StatsAre(1, 0, 1, 1),
              "returning from native Blt safely drops all snapshot pins without native AddRefs or double-dropping self-source");
        const ULONG sourceRemaining = same ? 0 : source.iface.lpVtbl->Release(&source.iface);
        const ULONG destinationRemaining = destinationSurface.iface.lpVtbl->Release(&destinationSurface.iface);
        Check(!sourceRemaining && !destinationRemaining && destinationSurface.destroyed && nativeSource.destroyed &&
              StatsAre(1, 0, 1, 1), "independent native references survive snapshot retirement and release exactly once");
        draw->lpVtbl->Release(draw);
        lakefix::ConfigureScalePresenter(nullptr, nullptr, nullptr);
        Check(StatsAre(0, 0, 0, 0), "distinct-source and self-source snapshot retirement leave no proxy/owner leaks");
    }
}

constexpr RECT tupleClient{20, 30, 820, 630};
constexpr RECT tupleOldViewport{120, 30, 720, 630};
constexpr RECT tupleNewViewport{20, 90, 820, 570};
struct SnapshotTupleObservation {
    unsigned descriptions = 0, oldProviders = 0, newProviders = 0;
    unsigned oldAdmissions = 0, newAdmissions = 0;
    unsigned oldPainters = 0, newPainters = 0, oldLogs = 0, newLogs = 0, oldFallbackLogs = 0, newDimensionsLogs = 0;
    unsigned oldClips = 0, newClips = 0;
    bool statsCorrect = true, paintArgumentsCorrect = true;
    HWND oldWindow = nullptr, newWindow = nullptr;
    LONG oldWidth = 0, oldHeight = 0, newWidth = 0, newHeight = 0;
    DWORD oldIncomingError = 0, newIncomingError = 0;
    DWORD oldAdmissionError = 0, newAdmissionError = 0;
} tupleObservation;
bool NewTupleViewport(HWND, LONG, LONG, RECT*, RECT*) noexcept;
void NewTupleDiagnostic(const wchar_t*) noexcept;
HWND NewTupleClip(HWND) noexcept;
bool NewTuplePaint(HWND, const RECT*, UINT) noexcept;
HRESULT NewTupleAdmission(HWND) noexcept;
void ReenterNewTupleConfiguration() noexcept {
    tupleObservation.statsCorrect = StatsAre(1, 2, 3, 1) && tupleObservation.statsCorrect;
    lakefix::ConfigureScalePresenter(NewTupleViewport, NewTupleDiagnostic, NewTupleClip, NewTuplePaint,
                                     1024, 768, nullptr, NewTupleAdmission);
    SetLastError(0xa310);
}
void ReconfigureFromDescription(FakeSurface* source) noexcept {
    source->onDescription = nullptr;
    ++tupleObservation.descriptions;
    ReenterNewTupleConfiguration();
}
HRESULT OldTupleAdmission(HWND window) noexcept {
    ++tupleObservation.oldAdmissions; tupleObservation.oldAdmissionError = GetLastError();
    tupleObservation.statsCorrect = window == Window(0x6789) && tupleObservation.statsCorrect;
    ReenterNewTupleConfiguration();
    return S_FALSE;
}
HRESULT NewTupleAdmission(HWND window) noexcept {
    ++tupleObservation.newAdmissions; tupleObservation.newAdmissionError = GetLastError();
    tupleObservation.statsCorrect = window == Window(0x6789) && tupleObservation.statsCorrect;
    ReenterNewTupleConfiguration();
    return S_OK;
}
bool OldTupleViewport(HWND window, LONG width, LONG height, RECT* full, RECT* fitted) noexcept {
    ++tupleObservation.oldProviders;
    tupleObservation.oldWindow = window; tupleObservation.oldWidth = width; tupleObservation.oldHeight = height;
    tupleObservation.oldIncomingError = GetLastError();
    *full = tupleClient; *fitted = tupleOldViewport;
    ReenterNewTupleConfiguration();
    return true;
}
bool NewTupleViewport(HWND window, LONG width, LONG height, RECT* full, RECT* fitted) noexcept {
    ++tupleObservation.newProviders;
    tupleObservation.newWindow = window; tupleObservation.newWidth = width; tupleObservation.newHeight = height;
    tupleObservation.newIncomingError = GetLastError();
    *full = tupleClient; *fitted = tupleNewViewport;
    ReenterNewTupleConfiguration();
    return true;
}
void OldTupleDiagnostic(const wchar_t* text) noexcept {
    ++tupleObservation.oldLogs;
    if (std::wcsstr(text, L"owned-host GDI letterbox fallback active")) ++tupleObservation.oldFallbackLogs;
    ReenterNewTupleConfiguration();
}
void NewTupleDiagnostic(const wchar_t* text) noexcept {
    ++tupleObservation.newLogs;
    if (std::wcsstr(text, L"invalid source dimensions")) ++tupleObservation.newDimensionsLogs;
    ReenterNewTupleConfiguration();
}
HWND OldTupleClip(HWND) noexcept {
    ++tupleObservation.oldClips;
    ReenterNewTupleConfiguration();
    return nullptr;
}
HWND NewTupleClip(HWND) noexcept {
    ++tupleObservation.newClips;
    ReenterNewTupleConfiguration();
    return nullptr;
}
bool CheckTuplePaint(HWND window, const RECT* strips, UINT count, const RECT& fitted) noexcept {
    const RECT expected[] = {
        {tupleClient.left, tupleClient.top, tupleClient.right, fitted.top},
        {tupleClient.left, fitted.bottom, tupleClient.right, tupleClient.bottom},
        {tupleClient.left, fitted.top, fitted.left, fitted.bottom},
        {fitted.right, fitted.top, tupleClient.right, fitted.bottom}
    };
    bool correct = window == Window(0x6789) && strips && count == ARRAYSIZE(expected);
    if (correct) for (UINT index = 0; index != count; ++index) correct = SameRect(strips[index], expected[index]) && correct;
    tupleObservation.paintArgumentsCorrect = correct && tupleObservation.paintArgumentsCorrect;
    ReenterNewTupleConfiguration();
    return correct;
}
bool OldTuplePaint(HWND window, const RECT* strips, UINT count) noexcept {
    ++tupleObservation.oldPainters;
    return CheckTuplePaint(window, strips, count, tupleOldViewport);
}
bool NewTuplePaint(HWND window, const RECT* strips, UINT count) noexcept {
    ++tupleObservation.newPainters;
    return CheckTuplePaint(window, strips, count, tupleNewViewport);
}
void TestBltSnapshotConfiguration() {
    ResetFixtures(); tupleObservation = {};
    lakefix::ConfigureScalePresenter(OldTupleViewport, OldTupleDiagnostic, OldTupleClip, OldTuplePaint,
                                     1024, 768, nullptr, OldTupleAdmission);
    FakeDraw real{}; FakeSurface destinationSurface{}, source{}, spare{};
    Initialize(real); Initialize(destinationSurface); Initialize(source); Initialize(spare);
    auto* draw = Wrap(real);
    draw->lpVtbl->SetCooperativeLevel(draw, Window(0x6789), DDSCL_NORMAL);
    auto* primary = Primary(draw, real, destinationSurface);
    auto* sourceProxy = Primary(draw, real, spare); // Keep independent proxy/lifetime inventory; source stays truly native offscreen.
    source.onDescription = ReconfigureFromDescription;
    destinationSurface.fillResult = DDERR_EXCEPTION;
    RECT destination{7, 11, 1031, 779}; const RECT originalDestination = destination;
    DDBLTFX effects{}; effects.dwSize = sizeof(effects); effects.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
    const DWORD flags = DDBLT_WAIT | DDBLT_DDFX;
    SetLastError(0xa301);
    const HRESULT firstResult = primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    const DWORD firstError = GetLastError();
    Check(tupleObservation.descriptions == 1 && tupleObservation.oldProviders == 1 && !tupleObservation.newProviders &&
          tupleObservation.oldAdmissions == 1 && !tupleObservation.newAdmissions && tupleObservation.oldAdmissionError == 0xa301 &&
          tupleObservation.oldWindow == Window(0x6789) && tupleObservation.oldWidth == 1024 && tupleObservation.oldHeight == 768 &&
          tupleObservation.oldIncomingError == 0xa301 && source.incomingDescriptionError == 0xa301,
          "Blt snapshots its old admission/provider before native GetSurfaceDesc and reentrant admission/provider reconfiguration");
    Check(tupleObservation.oldPainters == 1 && !tupleObservation.newPainters && tupleObservation.oldLogs >= 1 &&
          tupleObservation.oldFallbackLogs == 1 && !tupleObservation.newLogs && !tupleObservation.oldClips && !tupleObservation.newClips &&
          tupleObservation.statsCorrect && tupleObservation.paintArgumentsCorrect,
          "current Blt keeps a coherent old painter/logger tuple while native, provider, painter and logger callbacks reenter configuration");
    Check(firstResult == S_FALSE && firstError == bltError && destinationSurface.blts == 2 && destinationSurface.fills == 1 &&
          SameRect(LastCall(destinationSurface).destination, tupleOldViewport) && LastCall(destinationSurface).source == &source.iface &&
          !LastCall(destinationSurface).sourceRect && LastCall(destinationSurface).flags == flags &&
          LastCall(destinationSurface).effects == &effects && LastCall(destinationSurface).incomingError == 0xa301 &&
          SameRect(destination, originalDestination) && source.descriptions == 1 && source.descriptionSizeCorrect,
          "reentrant tuple changes preserve the old call's geometry, fresh source description, native result and LastError");

    // Existing fallback fixtures use no logger; its one-shot activation above
    // identifies the old logger. The reserved dimensions warning now identifies
    // the replacement logger on the very next Blt, not merely a later forwarder.
    const unsigned oldLogs = tupleObservation.oldLogs;
    source.width = 0; destinationSurface.presentationResult = E_INVALIDARG;
    ClearCalls(destinationSurface); SetLastError(0xa302);
    const HRESULT bypassResult = primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    const DWORD bypassError = GetLastError();
    Check(bypassResult == E_INVALIDARG && bypassError == bltError && destinationSurface.blts == 1 && !destinationSurface.fills &&
          LastCall(destinationSurface).destinationPointer == &destination && LastCall(destinationSurface).source == &source.iface &&
          !LastCall(destinationSurface).sourceRect && LastCall(destinationSurface).flags == flags &&
          LastCall(destinationSurface).effects == &effects && LastCall(destinationSurface).incomingError == 0xa302 &&
          source.incomingDescriptionError == 0xa302 && source.descriptions == 2 && source.descriptionSizeCorrect &&
          !tupleObservation.newProviders && !tupleObservation.newPainters && !tupleObservation.newAdmissions,
          "next Blt freshly observes invalid dimensions and keeps exact native fallback arguments/HRESULT/LastError");
    Check(tupleObservation.newLogs >= 1 && tupleObservation.newDimensionsLogs == 1 && tupleObservation.oldLogs == oldLogs &&
          tupleObservation.descriptions == 1 && tupleObservation.statsCorrect,
          "the very next Blt uses the replacement snapshot logger, which also reenters configuration without a registry lock");

    source.width = 1024; source.height = 768; destinationSurface.presentationResult = E_ACCESSDENIED;
    ClearCalls(destinationSurface); SetLastError(0xa303);
    const HRESULT replacementResult = primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    const DWORD replacementError = GetLastError();
    Check(tupleObservation.descriptions == 1 && tupleObservation.oldProviders == 1 && tupleObservation.newProviders == 1 &&
          tupleObservation.oldAdmissions == 1 && tupleObservation.newAdmissions == 1 && tupleObservation.newAdmissionError == 0xa303 &&
          tupleObservation.oldPainters == 1 && tupleObservation.newPainters == 1 && tupleObservation.newWindow == Window(0x6789) &&
          tupleObservation.newWidth == 1024 && tupleObservation.newHeight == 768 && tupleObservation.newIncomingError == 0xa303 &&
          source.incomingDescriptionError == 0xa303 && source.descriptions == 3 && source.descriptionSizeCorrect,
          "next valid Blt samples replacement admission/provider/painter with fresh dimensions, never a cached prior snapshot");
    Check(replacementResult == E_ACCESSDENIED && replacementError == bltError && destinationSurface.blts == 1 && !destinationSurface.fills &&
          SameRect(LastCall(destinationSurface).destination, tupleNewViewport) && LastCall(destinationSurface).source == &source.iface &&
          !LastCall(destinationSurface).sourceRect && LastCall(destinationSurface).flags == flags &&
          LastCall(destinationSurface).effects == &effects && LastCall(destinationSurface).incomingError == 0xa303 &&
          SameRect(destination, originalDestination) && tupleObservation.statsCorrect && tupleObservation.paintArgumentsCorrect &&
          !source.calls[1] && !destinationSurface.calls[1],
          "replacement snapshot remains reentrant and returns the actual native failure without hidden native lifetime changes");
    SetLastError(0xa304);
    Check(primary->lpVtbl->SetClipper(primary, nullptr) == S_OK && tupleObservation.newClips == 1 && !tupleObservation.oldClips &&
          !destinationSurface.seenClipper && destinationSurface.incomingClipperError == 0xa304 && GetLastError() == nativeError &&
          tupleObservation.statsCorrect, "subsequent clipper dispatch also observes the replacement tuple outside the registry lock");
    sourceProxy->lpVtbl->Release(sourceProxy); primary->lpVtbl->Release(primary); draw->lpVtbl->Release(draw);
    source.iface.lpVtbl->Release(&source.iface);
    lakefix::ConfigureScalePresenter(nullptr, nullptr, nullptr);
    Check(StatsAre(0, 0, 0, 0), "reentrant tuple snapshots release all proxy allocations and owner contexts");
}

struct CommitObservation {
    unsigned queries = 0, commits = 0, replacementCommits = 0;
    LONG publishedWidth = 0, publishedHeight = 0;
    bool queryEnabled = true, reconfigure = false, recooperate = false, argumentsCorrect = true, nativeAlreadySucceeded = true;
    FakeSurface* surface = nullptr;
    IDirectDraw7* draw = nullptr;
} commitObservation;
void ReplacementCommit(HWND, LONG, LONG, const RECT*, const RECT*) noexcept {
    ++commitObservation.replacementCommits;
}
void CommitPresented(HWND window, LONG width, LONG height, const RECT* full, const RECT* fitted) noexcept {
    ++commitObservation.commits;
    commitObservation.argumentsCorrect = window == Window(0x1234) && width == 1024 && height == 768 &&
        full && fitted && SameRect(*full, fullClient) && SameRect(*fitted, fittedViewport) && commitObservation.argumentsCorrect;
    commitObservation.nativeAlreadySucceeded = commitObservation.surface && commitObservation.surface->blts > 0 &&
        LastCall(*commitObservation.surface).source && SUCCEEDED(commitObservation.surface->presentationResult) &&
        GetLastError() == bltError && commitObservation.nativeAlreadySucceeded;
    commitObservation.publishedWidth = width; commitObservation.publishedHeight = height;
    (void)lakefix::GetScalePresenterStats(); (void)lakefix::GetScalePresenterDiagnostics();
    lakefix::ConfigureScalePresenter(nullptr, nullptr);
    SetLastError(0xc101);
}
bool QueryForCommit(HWND, LONG, LONG, RECT* full, RECT* fitted) noexcept {
    ++commitObservation.queries; *full = fullClient; *fitted = fittedViewport;
    if (commitObservation.reconfigure)
        lakefix::ConfigureScalePresenter(QueryForCommit, nullptr, nullptr, nullptr, 1024, 768, ReplacementCommit);
    if (commitObservation.recooperate)
        commitObservation.draw->lpVtbl->SetCooperativeLevel(commitObservation.draw, Window(0x2345), DDSCL_NORMAL);
    (void)lakefix::GetScalePresenterStats();
    SetLastError(0xc102);
    return commitObservation.queryEnabled;
}
void TestPresentationCommit() {
    ResetFixtures(); commitObservation = {};
    FakeDraw real{}; FakeSurface surface{}, source{}; Initialize(real); Initialize(surface); Initialize(source);
    auto* draw = Wrap(real); draw->lpVtbl->SetCooperativeLevel(draw, Window(0x1234), DDSCL_NORMAL);
    auto* primary = Primary(draw, real, surface); commitObservation.surface = &surface; commitObservation.draw = draw;
    fullClient = {100, 200, 1700, 1100}; fittedViewport = {300, 200, 1500, 1100};
    RECT destination{7, 11, 1031, 779};
    const auto configure = [&]() {
        lakefix::ConfigureScalePresenter(QueryForCommit, nullptr, nullptr, nullptr, 1024, 768, CommitPresented);
    };
    const auto present = [&]() { return primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, DDBLT_WAIT, nullptr); };
    configure(); surface.presentationResult = DDERR_SURFACELOST;
    Check(present() == DDERR_SURFACELOST && commitObservation.queries == 1 && !commitObservation.commits &&
          !commitObservation.publishedWidth && !commitObservation.publishedHeight,
          "viewport query never publishes source/input geometry before a failing image Blt");
    surface.presentationResult = S_FALSE; surface.fillResult = E_OUTOFMEMORY; ClearCalls(surface);
    Check(present() == E_OUTOFMEMORY && surface.blts == 1 && !commitObservation.commits,
          "letterbox failure never commits a frame that was not presented");
    surface.fillResult = S_OK; commitObservation.queryEnabled = false; ClearCalls(surface);
    Check(present() == S_FALSE && surface.blts == 1 && !commitObservation.commits &&
          LastCall(surface).destinationPointer == &destination, "declined query preserves native success without committing scaled geometry");
    commitObservation.queryEnabled = true; commitObservation.reconfigure = true;
    configure(); ClearCalls(surface); SetLastError(0xc103);
    Check(present() == S_FALSE && commitObservation.commits == 1 && !commitObservation.replacementCommits &&
          commitObservation.publishedWidth == 1024 && commitObservation.publishedHeight == 768 &&
          commitObservation.argumentsCorrect && commitObservation.nativeAlreadySucceeded && GetLastError() == bltError &&
          LastCall(surface).incomingError == 0xc103,
          "successful image HRESULT commits exactly once with the original coherent callback tuple outside locks");
    const auto providers = commitObservation.queries;
    lakefix::ConfigureScalePresenter(QueryForCommit, nullptr); ClearCalls(surface);
    Check(present() == S_FALSE && commitObservation.queries == providers && commitObservation.commits == 1 &&
          surface.blts == 1 && LastCall(surface).destinationPointer == &destination,
          "missing expected source dimensions disables admission rather than trusting query side effects");
    configure(); commitObservation.reconfigure = false; commitObservation.recooperate = true; ClearCalls(surface);
    Check(present() == S_FALSE && surface.blts == 1 && !surface.fills && commitObservation.commits == 1 &&
          LastCall(surface).destinationPointer == &destination,
          "reentrant cooperative-window changes reject a stale queried plan before fills or input-geometry commit");
    primary->lpVtbl->Release(primary); draw->lpVtbl->Release(draw); source.iface.lpVtbl->Release(&source.iface);
    Check(StatsAre(0, 0, 0, 0), "query/commit test releases all proxy metadata");
}

IDirectDrawSurface7* invalidationPrimary = nullptr;
IDirectDraw7* invalidationDraw = nullptr;
unsigned invalidationKind = 0;
bool invalidationCallbackReentered = false;
void InvalidateFromImage(FakeSurface* value) noexcept {
    if (!LastCall(*value).source) return; // Invalidate AFTER letterbox fills, inside the real image Blt.
    value->onBlt = nullptr;
    (void)lakefix::GetScalePresenterStats();
    lakefix::ConfigureScalePresenter(FallbackViewport, nullptr, nullptr, nullptr, 1024, 768);
    if (invalidationKind == 0) invalidationPrimary->lpVtbl->Restore(invalidationPrimary);
    else if (invalidationKind == 1) invalidationDraw->lpVtbl->RestoreAllSurfaces(invalidationDraw);
    else invalidationPrimary->lpVtbl->ReleaseDC(invalidationPrimary, reinterpret_cast<HDC>(static_cast<UINT_PTR>(0x8765)));
    invalidationCallbackReentered = true;
}
IDirectDrawSurface7* retireSurface = nullptr;
IDirectDraw7* retireDraw = nullptr;
bool invalidationPinObserved = false;
void RetireFromRestore(FakeSurface*) noexcept {
    retireSurface->lpVtbl->Release(retireSurface);
    invalidationPinObserved = StatsAre(1, 0, 2, 1);
    lakefix::ConfigureScalePresenter(nullptr, nullptr);
}
void RetireFromRestoreAll(FakeDraw*) noexcept {
    retireDraw->lpVtbl->Release(retireDraw);
    invalidationPinObserved = StatsAre(0, 1, 2, 1);
    lakefix::ConfigureScalePresenter(nullptr, nullptr);
}
void TestReentrantInvalidation() {
    for (unsigned kind = 0; kind != 3; ++kind) {
        ResetFixtures(); commitObservation = {};
        lakefix::ConfigureScalePresenter(QueryForCommit, nullptr, nullptr, nullptr, 1024, 768, CommitPresented);
        FakeDraw real{}; FakeSurface surface{}, source{}; Initialize(real); Initialize(surface); Initialize(source);
        auto* draw = Wrap(real); draw->lpVtbl->SetCooperativeLevel(draw, Window(0x1234), DDSCL_NORMAL);
        auto* primary = Primary(draw, real, surface); surface.dcDrewOverLetterbox = true;
        invalidationPrimary = primary; invalidationDraw = draw; invalidationKind = kind;
        invalidationCallbackReentered = false; surface.onBlt = InvalidateFromImage;
        fullClient = {100, 200, 1700, 1100}; fittedViewport = {300, 200, 1500, 1100};
        RECT destination{0, 0, 1024, 768};
        const auto present = [&]() { return primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, DDBLT_WAIT, nullptr); };
        Check(present() == S_FALSE && invalidationCallbackReentered && !surface.letterboxContents && !commitObservation.commits,
              "reentrant Restore/RestoreAllSurfaces/ReleaseDC cannot commit an image invalidated inside native Blt");
        ClearCalls(surface); present();
        Check(surface.fills == 2 && surface.letterboxContents,
              "invalidation generation prevents outer successful Blt from reviving a stale letterbox cache");
        void* alias = nullptr; invalidationPinObserved = false;
        if (kind == 1) {
            draw->lpVtbl->QueryInterface(draw, d3d7Id, &alias); retireDraw = draw; real.onRestoreAll = RetireFromRestoreAll;
            SetLastError(0xc201);
            Check(draw->lpVtbl->RestoreAllSurfaces(draw) == S_OK && GetLastError() == nativeError && invalidationPinObserved &&
                  StatsAre(0, 1, 1, 1), "RestoreAllSurfaces pins a retired owner across reentrant final API Release");
            primary->lpVtbl->Release(primary);
        } else {
            primary->lpVtbl->QueryInterface(primary, otherSurfaceId, &alias); retireSurface = primary;
            if (!kind) surface.onRestore = RetireFromRestore;
            else surface.onReleaseDC = RetireFromRestore;
            SetLastError(0xc202);
            const auto result = !kind ? primary->lpVtbl->Restore(primary) : primary->lpVtbl->ReleaseDC(primary, nullptr);
            Check(result == S_OK && GetLastError() == nativeError && invalidationPinObserved && StatsAre(1, 0, 1, 1),
                  "Restore/ReleaseDC retain metadata through native callbacks retiring the final surface API reference");
            draw->lpVtbl->Release(draw);
        }
        static_cast<IUnknown*>(alias)->lpVtbl->Release(static_cast<IUnknown*>(alias));
        source.iface.lpVtbl->Release(&source.iface);
        Check(StatsAre(0, 0, 0, 0), "reentrant invalidation and retirement release every owner/proxy");
    }
}

void** failureOutput = nullptr;
bool failureCleanupReentered = false;
void DrawFailureCleanup(FakeDraw*) noexcept {
    failureCleanupReentered = failureOutput && !*failureOutput && StatsAre(0, 0, 0, 0);
    (void)lakefix::GetScalePresenterDiagnostics(); lakefix::ConfigureScalePresenter(nullptr, nullptr);
    SetLastError(0xc301);
}
void SurfaceFailureCleanup(FakeSurface*) noexcept {
    failureCleanupReentered = failureOutput && !*failureOutput && StatsAre(1, 0, 1, 1);
    (void)lakefix::GetScalePresenterDiagnostics(); lakefix::ConfigureScalePresenter(nullptr, nullptr);
    SetLastError(0xc302);
}
void TestResourceFailureOwnership() {
    for (unsigned kind = 0; kind != 3; ++kind) {
        ResetFixtures(); lakefix::ConfigureScalePresenter(nullptr, nullptr);
        FakeDraw real{}; Initialize(real); real.onFinalRelease = DrawFailureCleanup;
        lakefix::ScalePresenterResources resources{};
        if (kind < 2) resources.allocationBudget = kind; else resources.drawCapacity = 0;
        lakefix::ConfigureScalePresenterResources(resources);
        void* output = &real.iface; failureOutput = &output; failureCleanupReentered = false;
        SetLastError(0xc303);
        const auto result = lakefix::CompleteScaledDirectDrawCreation(S_FALSE, &output);
        Check(result == (kind < 2 ? E_OUTOFMEMORY : HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA)) && !output &&
              real.destroyed && !real.references && real.calls[2] == 1 && failureCleanupReentered && GetLastError() == 0xc303 &&
              StatsAre(0, 0, 0, 0), "scaled factory owner/proxy/capacity failure releases native ownership and returns clear HRESULT/null");
    }
    lakefix::ConfigureScalePresenterResources({}); ResetFixtures();
    FakeDraw real{}; Initialize(real); void* output = &real.iface;
    Check(lakefix::CompleteScaledDirectDrawCreation(S_FALSE, &output) == S_FALSE && output != &real.iface && real.references == 1,
          "scaled factory finalizer preserves a positive native success and transfers exactly one reference");
    auto* draw = static_cast<IDirectDraw7*>(output);
    lakefix::ScalePresenterResources resources{}; resources.allocationBudget = 0;
    lakefix::ConfigureScalePresenterResources(resources);
    DrawAddRef(&real.iface); void* second = &real.iface;
    Check(lakefix::CompleteScaledDirectDrawCreation(S_OK, &second) == S_OK && second == draw && real.references == 2,
          "existing native reference adoption needs no allocation even when the resource budget is exhausted");
    static_cast<IDirectDraw7*>(second)->lpVtbl->Release(static_cast<IDirectDraw7*>(second));
    void* untouched = &real.iface;
    Check(lakefix::CompleteScaledDirectDrawCreation(E_ACCESSDENIED, &untouched) == E_ACCESSDENIED && untouched == &real.iface &&
          real.references == 1, "failed native factory result is untouched and is not released by the finalizer");
    Check(lakefix::CompleteScaledDirectDrawCreation(S_OK, nullptr) == E_POINTER,
          "scaled factory successful result with invalid output fails clearly");
    for (unsigned kind = 0; kind != 2; ++kind) {
        FakeSurface surface{}; Initialize(surface); surface.onFinalRelease = SurfaceFailureCleanup;
        real.nextSurface = &surface; real.createResult = S_FALSE;
        resources = {}; if (!kind) resources.allocationBudget = 0; else resources.primaryCapacity = 0;
        lakefix::ConfigureScalePresenterResources(resources);
        auto description = SurfaceDescription(); IDirectDrawSurface7* primary = nullptr;
        failureOutput = reinterpret_cast<void**>(&primary); failureCleanupReentered = false;
        SetLastError(0xc304);
        const auto result = draw->lpVtbl->CreateSurface(draw, &description, &primary, nullptr);
        Check(result == (!kind ? E_OUTOFMEMORY : HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA)) && !primary &&
              surface.destroyed && surface.calls[2] == 1 && failureCleanupReentered && GetLastError() == createError &&
              StatsAre(1, 0, 1, 1), "primary proxy allocation/capacity failure clears output before reentrant native cleanup");
    }
    FakeSurface aliased{}, offscreen{}; Initialize(aliased); Initialize(offscreen);
    SurfaceAddRef(&aliased.iface); real.nextSurface = &aliased;
    auto description = SurfaceDescription(); IDirectDrawSurface7* primary = nullptr;
    Check(FAILED(draw->lpVtbl->CreateSurface(draw, &description, &primary, nullptr)) && !primary &&
          aliased.references == 1 && !aliased.destroyed && aliased.calls[2] == 1,
          "failed primary wrapping releases only its newly returned reference, not an independent native alias");
    aliased.iface.lpVtbl->Release(&aliased.iface);
    real.nextSurface = &offscreen; description = SurfaceDescription(DDSCAPS_OFFSCREENPLAIN);
    Check(draw->lpVtbl->CreateSurface(draw, &description, &primary, nullptr) == S_FALSE && primary == &offscreen.iface &&
          offscreen.references == 1, "resource failure does not wrap or reject native auxiliary/offscreen creation");
    primary->lpVtbl->Release(primary); draw->lpVtbl->Release(draw);
    lakefix::ConfigureScalePresenterResources({}); failureOutput = nullptr;
    Check(StatsAre(0, 0, 0, 0), "all injected production resource-admission failures leave balanced ownership");
}

struct AdmissionObservation {
    HRESULT result = S_OK;
    unsigned calls = 0, queries = 0, commits = 0, failureLogs = 0, recoveryLogs = 0;
    unsigned replacementAdmissions = 0, replacementQueries = 0, replacementCommits = 0, replacementLogs = 0;
    HWND window = nullptr;
    DWORD incomingError = 0;
    bool queryEnabled = true, reconfigureLog = false, retired = false, metadataAlive = true;
    FakeSurface* surface = nullptr;
    IDirectDrawSurface7* primary = nullptr;
    IDirectDrawSurface7* source = nullptr;
    HRESULT nestedResult = S_OK;
} admissionObservation;
HRESULT FrameAdmission(HWND) noexcept;
bool AdmissionViewport(HWND, LONG, LONG, RECT*, RECT*) noexcept;
void AdmissionCommit(HWND, LONG, LONG, const RECT*, const RECT*) noexcept;
void AdmissionDiagnostic(const wchar_t*) noexcept;
void ConfigureAdmission(lakefix::ScalePresentationAdmission admission = FrameAdmission) noexcept {
    lakefix::ConfigureScalePresenter(AdmissionViewport, AdmissionDiagnostic, nullptr, nullptr,
                                     1024, 768, AdmissionCommit, admission);
}
void ObserveAdmissionPins() noexcept {
    (void)lakefix::GetScalePresenterStats(); (void)lakefix::GetScalePresenterDiagnostics();
    if (admissionObservation.retired)
        admissionObservation.metadataAlive = StatsAre(1, 0, 2, 1) && admissionObservation.metadataAlive;
}
HRESULT FrameAdmission(HWND window) noexcept {
    ++admissionObservation.calls; admissionObservation.window = window;
    admissionObservation.incomingError = GetLastError();
    ObserveAdmissionPins();
    SetLastError(0xd101);
    return admissionObservation.result;
}
bool AdmissionViewport(HWND, LONG, LONG, RECT* full, RECT* fitted) noexcept {
    ++admissionObservation.queries; *full = fullClient; *fitted = fittedViewport;
    ObserveAdmissionPins(); SetLastError(0xd102);
    return admissionObservation.queryEnabled;
}
void AdmissionCommit(HWND, LONG, LONG, const RECT*, const RECT*) noexcept {
    ++admissionObservation.commits; ObserveAdmissionPins(); SetLastError(0xd103);
}
void AdmissionDiagnostic(const wchar_t* text) noexcept {
    if (std::wcsstr(text, L"failure presentation admission ")) ++admissionObservation.failureLogs;
    if (std::wcsstr(text, L"recovered presentation admission ")) ++admissionObservation.recoveryLogs;
    ObserveAdmissionPins();
    if (admissionObservation.reconfigureLog) ConfigureAdmission();
    SetLastError(0xd104);
}
void TestPresentationAdmissionGuard() {
    ResetFixtures(); admissionObservation = {}; admissionObservation.reconfigureLog = true; ConfigureAdmission();
    FakeDraw real{}; FakeSurface surface{}, source{}; Initialize(real); Initialize(surface); Initialize(source);
    auto* draw = Wrap(real); draw->lpVtbl->SetCooperativeLevel(draw, Window(0x1234), DDSCL_NORMAL);
    auto* primary = Primary(draw, real, surface);
    fullClient = {100, 200, 1700, 1100}; fittedViewport = {300, 200, 1500, 1100};
    RECT destination{0, 0, 1024, 768};
    const auto present = [&]() { return primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, DDBLT_WAIT, nullptr); };
    SetLastError(0xd110);
    Check(present() == S_FALSE && admissionObservation.calls == 1 && admissionObservation.queries == 1 &&
          admissionObservation.commits == 1 && admissionObservation.window == Window(0x1234) &&
          admissionObservation.incomingError == 0xd110 && LastCall(surface).incomingError == 0xd110 && GetLastError() == bltError,
          "admission runs after native classification and before query, retaining the actual image HRESULT/LastError");
    admissionObservation.result = S_FALSE; admissionObservation.queryEnabled = false; ClearCalls(surface); SetLastError(0xd111);
    Check(present() == S_FALSE && admissionObservation.calls == 2 && admissionObservation.queries == 2 &&
          admissionObservation.commits == 1 && surface.blts == 1 && !surface.fills &&
          LastCall(surface).destinationPointer == &destination && LastCall(surface).incomingError == 0xd111 && GetLastError() == bltError,
          "admission success with provider=false preserves legitimate minimized/native bypass without a commit");
    surface.presentationResult = DDERR_SURFACELOST; ClearCalls(surface); SetLastError(0xd112);
    Check(present() == DDERR_SURFACELOST && surface.blts == 1 && admissionObservation.calls == 3 &&
          admissionObservation.queries == 3 && admissionObservation.commits == 1 &&
          LastCall(surface).incomingError == 0xd112 && GetLastError() == bltError,
          "a positive admission HRESULT never replaces an actual native-forward failure or its LastError");
    admissionObservation.queryEnabled = true; admissionObservation.result = S_OK; surface.presentationResult = S_FALSE;
    ClearCalls(surface); present(); // Warm unchanged geometry before a layout-prerequisite rejection.
    const auto queries = admissionObservation.queries, commits = admissionObservation.commits;
    const auto before = lakefix::GetScalePresenterDiagnostics();
    admissionObservation.result = E_UNEXPECTED; admissionObservation.failureLogs = admissionObservation.recoveryLogs = 0;
    ClearCalls(surface); SetLastError(0xd113);
    Check(present() == E_UNEXPECTED && !surface.blts && !surface.fills && admissionObservation.queries == queries &&
          admissionObservation.commits == commits && admissionObservation.incomingError == 0xd113 && GetLastError() == 0xd113,
          "missing layout admission returns its exact failing HRESULT without provider, native Blt or commit and preserves caller LastError");
    bool rejectionResults = true;
    for (unsigned index = 1; index != 1000; ++index) {
        SetLastError(0xd113);
        rejectionResults = present() == E_UNEXPECTED && GetLastError() == 0xd113 && rejectionResults;
    }
    const auto rejected = lakefix::GetScalePresenterDiagnostics();
    Check(rejectionResults && !surface.blts && admissionObservation.queries == queries && admissionObservation.commits == commits &&
          admissionObservation.failureLogs == 3 && !admissionObservation.recoveryLogs && rejected.failures - before.failures == 1000,
          "admission has its own bounded operation/HRESULT diagnostics, never native presentation during a rejection storm");
    admissionObservation.result = S_FALSE; surface.presentationResult = E_NOTIMPL; ClearCalls(surface); SetLastError(0xd114);
    Check(present() == E_NOTIMPL && surface.blts == 3 && surface.fills == 2 && admissionObservation.commits == commits &&
          admissionObservation.recoveryLogs == 1 && LastCall(surface).incomingError == 0xd114 && GetLastError() == bltError,
          "admission recovery retries invalidated letterbox geometry but still returns the real failing image HRESULT");
    surface.presentationResult = S_FALSE; ClearCalls(surface);
    Check(present() == S_FALSE && surface.fills == 2 && admissionObservation.commits == commits + 1 &&
          admissionObservation.failureLogs + admissionObservation.recoveryLogs == 4,
          "a later admitted frame succeeds without a sticky global error or unlimited recovery logs");

    admissionObservation.result = E_UNEXPECTED; surface.presentationResult = E_ACCESSDENIED;
    const auto nativeOnly = [&](RECT* target, IDirectDrawSurface7* input, RECT* crop, const char* name, DWORD flags = DDBLT_WAIT) {
        const auto calls = admissionObservation.calls, providers = admissionObservation.queries, published = admissionObservation.commits;
        ClearCalls(surface); SetLastError(0xd115);
        Check(primary->lpVtbl->Blt(primary, target, input, crop, flags, nullptr) == E_ACCESSDENIED && surface.blts == 1 &&
              !surface.fills && admissionObservation.calls == calls && admissionObservation.queries == providers &&
              admissionObservation.commits == published && LastCall(surface).destinationPointer == target &&
              LastCall(surface).sourceRect == crop && LastCall(surface).flags == flags &&
              LastCall(surface).incomingError == 0xd115 && GetLastError() == bltError, name);
    };
    source.width = 640;
    nativeOnly(&destination, &source.iface, nullptr, "auxiliary dimensions never invoke admission or replace native-forward failure");
    source.width = 1024; source.caps = DDSCAPS_TEXTURE;
    nativeOnly(&destination, &source.iface, nullptr, "unknown/texture source role bypasses admission and retains native HRESULT/LastError");
    source.caps = DDSCAPS_OFFSCREENPLAIN; source.descriptionResult = E_FAIL;
    nativeOnly(&destination, &source.iface, nullptr, "unclassified descriptor failure stays native without invoking admission");
    source.descriptionResult = S_OK;
    RECT partial{0, 0, 640, 480}, crop{1, 2, 100, 200};
    nativeOnly(&partial, &source.iface, nullptr, "partial destination does not invoke the full-frame admission guard");
    nativeOnly(nullptr, &source.iface, nullptr, "unknown whole-primary destination does not invoke admission");
    nativeOnly(&destination, &source.iface, &crop, "non-null sourceRect bypasses admission without mutating native arguments");
    nativeOnly(&destination, &source.iface, nullptr, "unsupported auxiliary Blt flags bypass admission and stay exactly native",
               DDBLT_KEYSRC | DDBLT_WAIT);
    nativeOnly(&destination, primary, nullptr, "known primary proxy source stays native/unwrapped without invoking admission");
    const auto admittedCalls = admissionObservation.calls, published = admissionObservation.commits;
    admissionObservation.result = S_OK; surface.presentationResult = S_FALSE; ClearCalls(surface);
    Check(present() == S_FALSE && admissionObservation.calls == admittedCalls + 1 && admissionObservation.commits == published + 1 &&
          SameRect(LastCall(surface).destination, fittedViewport),
          "auxiliary native failures cannot poison a subsequent valid admitted presentation");
    const auto callsWithGuard = admissionObservation.calls;
    ConfigureAdmission(nullptr); admissionObservation.result = E_UNEXPECTED; ClearCalls(surface);
    Check(present() == S_FALSE && admissionObservation.calls == callsWithGuard && admissionObservation.commits == published + 2,
          "default/null admission remains compatible with standalone and low-level graphics-check presentation");
    lakefix::ConfigureScalePresenter(nullptr, AdmissionDiagnostic, nullptr, nullptr, 1024, 768, AdmissionCommit, FrameAdmission);
    ClearCalls(surface); SetLastError(0xd116);
    Check(present() == E_UNEXPECTED && !surface.blts && admissionObservation.calls == callsWithGuard + 1 && GetLastError() == 0xd116,
          "an explicit admission guard cannot be skipped merely because the viewport callback is absent");
    primary->lpVtbl->Release(primary); draw->lpVtbl->Release(draw); source.iface.lpVtbl->Release(&source.iface);
    lakefix::ConfigureScalePresenter(nullptr, nullptr);
    Check(StatsAre(0, 0, 0, 0), "admission rejection, recovery and native bypass retain balanced proxy/owner lifetime");
}

HRESULT ReplacementAdmission(HWND) noexcept { ++admissionObservation.replacementAdmissions; return E_ABORT; }
bool ReplacementAdmissionViewport(HWND, LONG, LONG, RECT*, RECT*) noexcept {
    ++admissionObservation.replacementQueries; return false;
}
void ReplacementAdmissionCommit(HWND, LONG, LONG, const RECT*, const RECT*) noexcept { ++admissionObservation.replacementCommits; }
void ReplacementAdmissionDiagnostic(const wchar_t*) noexcept { ++admissionObservation.replacementLogs; }
HRESULT RetiringAdmission(HWND window) noexcept {
    const auto result = FrameAdmission(window);
    lakefix::ConfigureScalePresenter(ReplacementAdmissionViewport, ReplacementAdmissionDiagnostic, nullptr, nullptr,
                                     1024, 768, ReplacementAdmissionCommit, ReplacementAdmission);
    admissionObservation.primary->lpVtbl->Release(admissionObservation.primary);
    admissionObservation.retired = true; ObserveAdmissionPins(); SetLastError(0xd120);
    return result;
}
HRESULT NestedAdmission(HWND window) noexcept {
    const auto result = FrameAdmission(window);
    admissionObservation.surface->presentationResult = E_INVALIDARG;
    RECT target{3, 4, 1027, 772};
    admissionObservation.nestedResult = admissionObservation.primary->lpVtbl->Blt(admissionObservation.primary,
        &target, admissionObservation.source, nullptr, DDBLT_WAIT, nullptr);
    admissionObservation.surface->presentationResult = S_FALSE;
    SetLastError(0xd121);
    return result;
}
void TestPresentationAdmissionReentrancy() {
    for (unsigned reject = 0; reject != 2; ++reject) {
        ResetFixtures(); admissionObservation = {};
        FakeDraw real{}; FakeSurface surface{}, source{}; Initialize(real); Initialize(surface); Initialize(source);
        auto* draw = Wrap(real); draw->lpVtbl->SetCooperativeLevel(draw, Window(0x1234), DDSCL_NORMAL);
        auto* primary = Primary(draw, real, surface); admissionObservation.primary = primary;
        admissionObservation.result = reject ? HRESULT_FROM_WIN32(ERROR_NOT_READY) : S_FALSE;
        void* alias = nullptr; primary->lpVtbl->QueryInterface(primary, otherSurfaceId, &alias);
        const auto adds = surface.calls[1], sourceAdds = source.calls[1];
        ConfigureAdmission(RetiringAdmission);
        fullClient = {100, 200, 1700, 1100}; fittedViewport = {300, 200, 1500, 1100};
        RECT destination{0, 0, 1024, 768}; SetLastError(0xd122);
        const auto result = primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, DDBLT_WAIT, nullptr);
        const auto error = GetLastError();
        Check(admissionObservation.calls == 1 && admissionObservation.retired && admissionObservation.metadataAlive &&
              surface.references == 1 && surface.calls[1] == adds && source.calls[1] == sourceAdds && StatsAre(1, 0, 1, 1),
              "admission can retire the final API surface reference outside locks while the snapshot metadata remains pinned");
        Check(result == (reject ? HRESULT_FROM_WIN32(ERROR_NOT_READY) : S_FALSE) && error == (reject ? 0xd122 : bltError) &&
              admissionObservation.queries == (reject ? 0U : 1U) && admissionObservation.commits == (reject ? 0U : 1U) &&
              surface.blts == (reject ? 0U : 3U) && !admissionObservation.replacementAdmissions &&
              !admissionObservation.replacementQueries && !admissionObservation.replacementCommits && !admissionObservation.replacementLogs &&
              (!reject || admissionObservation.failureLogs == 1),
              "current Blt retains coherent admission/query/commit/logger after admission reconfigures callbacks and releases its proxy");
        static_cast<IUnknown*>(alias)->lpVtbl->Release(static_cast<IUnknown*>(alias));
        draw->lpVtbl->Release(draw); source.iface.lpVtbl->Release(&source.iface);
        lakefix::ConfigureScalePresenter(nullptr, nullptr);
        Check(StatsAre(0, 0, 0, 0), "retiring admission drops snapshot pins and independent native references exactly once");
    }
    ResetFixtures(); admissionObservation = {};
    FakeDraw real{}; FakeSurface surface{}, source{}; Initialize(real); Initialize(surface); Initialize(source);
    auto* draw = Wrap(real); draw->lpVtbl->SetCooperativeLevel(draw, Window(0x1234), DDSCL_NORMAL);
    auto* primary = Primary(draw, real, surface);
    admissionObservation.primary = primary; admissionObservation.source = &source.iface; admissionObservation.surface = &surface;
    ConfigureAdmission(NestedAdmission);
    RECT destination{0, 0, 1024, 768}; SetLastError(0xd123);
    Check(primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, DDBLT_WAIT, nullptr) == S_FALSE &&
          admissionObservation.nestedResult == E_INVALIDARG && admissionObservation.calls == 1 && admissionObservation.queries == 1 &&
          admissionObservation.commits == 1 && source.descriptions == 1 && surface.blts == 4 &&
          LastCall(surface).incomingError == 0xd123 && GetLastError() == bltError,
          "Blt nested inside admission forwards its real failure without recursively invoking admission or poisoning the outer success");
    primary->lpVtbl->Release(primary); draw->lpVtbl->Release(draw); source.iface.lpVtbl->Release(&source.iface);
    lakefix::ConfigureScalePresenter(nullptr, nullptr);
    Check(StatsAre(0, 0, 0, 0), "nested admission leaves no proxy/owner pins behind");
}

unsigned boundedLogCalls = 0;
bool boundedLogArguments = true, sawRepeatedCount = false, sawRecovery = false;
void BoundedDiagnostic(const wchar_t* text) noexcept {
    ++boundedLogCalls;
    boundedLogArguments = text && std::wcsstr(text, L"HRESULT=0x") && std::wcsstr(text, L"failures=") &&
        std::wcsstr(text, L"recoveries=") && boundedLogArguments;
    if (std::wcsstr(text, L"failures=16 ")) sawRepeatedCount = true;
    if (std::wcsstr(text, L"recovered ")) sawRecovery = true;
    (void)lakefix::GetScalePresenterStats(); (void)lakefix::GetScalePresenterDiagnostics();
    lakefix::ConfigureScalePresenter(nullptr, BoundedDiagnostic);
    SetLastError(0xc401);
}
void TestBoundedDiagnostics() {
    ResetFixtures(); lakefix::ConfigureScalePresenter(nullptr, BoundedDiagnostic);
    FakeDraw real{}; FakeSurface surface{}; Initialize(real); Initialize(surface);
    auto* draw = Wrap(real); auto* primary = Primary(draw, real, surface);
    draw->lpVtbl->EvaluateMode(draw, 0, nullptr); primary->lpVtbl->Restore(primary); // Settle prior suite recoveries.
    const auto before = lakefix::GetScalePresenterDiagnostics();
    const HRESULT first = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x601);
    const HRESULT second = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x602);
    boundedLogCalls = 0; boundedLogArguments = true; sawRepeatedCount = sawRecovery = false;
    real.evaluateResult = first;
    for (unsigned index = 0; index != 1000; ++index) draw->lpVtbl->EvaluateMode(draw, 0, nullptr);
    real.evaluateResult = S_FALSE; draw->lpVtbl->EvaluateMode(draw, 0, nullptr);
    Check(boundedLogCalls == 4 && sawRepeatedCount && sawRecovery && boundedLogArguments,
          "diagnostics aggregate 1000 failures plus recovery with at most four messages per operation/HRESULT");
    for (unsigned index = 0; index != 100; ++index) {
        real.evaluateResult = first; draw->lpVtbl->EvaluateMode(draw, 0, nullptr);
        real.evaluateResult = S_FALSE; draw->lpVtbl->EvaluateMode(draw, 0, nullptr);
    }
    const auto after = lakefix::GetScalePresenterDiagnostics();
    Check(boundedLogCalls == 4 && after.failures - before.failures == 1100 && after.recoveries - before.recoveries == 101 &&
          after.suppressedMessages > before.suppressedMessages,
          "oscillating failure/recovery remains counted without an unlimited recovery-log stream");
    real.evaluateResult = second; SetLastError(0xc402);
    const auto result = draw->lpVtbl->EvaluateMode(draw, 0, nullptr);
    surface.restoreResult = first; const auto restore = primary->lpVtbl->Restore(primary);
    Check(result == second && restore == first && boundedLogCalls == 6 && GetLastError() == nativeError,
          "different HRESULT and different native operation each retain independent budgets and exact native results/LastError");
    for (unsigned index = 0; index != 160; ++index) {
        real.evaluateResult = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x700 + index);
        draw->lpVtbl->EvaluateMode(draw, 0, nullptr);
        real.evaluateResult = S_FALSE; draw->lpVtbl->EvaluateMode(draw, 0, nullptr);
    }
    const auto full = lakefix::GetScalePresenterDiagnostics(); const auto logs = boundedLogCalls;
    real.evaluateResult = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x7fff);
    for (unsigned index = 0; index != 1000; ++index) draw->lpVtbl->EvaluateMode(draw, 0, nullptr);
    const auto overflow = lakefix::GetScalePresenterDiagnostics();
    Check(full.trackedPairs == 96 && full.untrackedFailures > 0 && boundedLogCalls == logs &&
          overflow.untrackedFailures - full.untrackedFailures == 1000 && boundedLogCalls <= 96 * 4 && boundedLogArguments,
          "fixed diagnostic capacity counts excess pairs without evicting entries or producing unbounded logs");
    primary->lpVtbl->Release(primary); draw->lpVtbl->Release(draw); lakefix::ConfigureScalePresenter(nullptr, nullptr);
    Check(StatsAre(0, 0, 0, 0), "bounded logger can reenter stats/configuration with no locks held and leaves no proxies");
}
void TestOpDiag96Budget() {
    // The preceding diagnostic suite deliberately fills the process-lifetime table; no test-only reset/bypass.
    ResetFixtures(); admissionObservation = {}; ConfigureAdmission();
    FakeDraw real{}; FakeSurface surface{}, source{}; Initialize(real); Initialize(surface); Initialize(source);
    auto* draw = Wrap(real); draw->lpVtbl->SetCooperativeLevel(draw, Window(0x1234), DDSCL_NORMAL);
    auto* primary = Primary(draw, real, surface);
    const auto full = lakefix::GetScalePresenterDiagnostics();
    Check(full.trackedPairs == 96, "diagnostic 96-pair budget precondition is reached through production operations");
    const HRESULT rejected = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x6abc);
    const HRESULT nativeFailure = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x6abd);
    admissionObservation.result = rejected;
    RECT destination{0, 0, 1024, 768}; SetLastError(0xd130);
    Check(primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, DDBLT_WAIT, nullptr) == rejected &&
          GetLastError() == 0xd130 && !surface.blts && admissionObservation.calls == 1 &&
          !admissionObservation.queries && !admissionObservation.commits && !admissionObservation.failureLogs,
          "full diagnostic table never converts an untracked admission rejection into a native Blt or success");
    source.width = 640; surface.presentationResult = nativeFailure; SetLastError(0xd131);
    Check(primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, DDBLT_WAIT, nullptr) == nativeFailure &&
          GetLastError() == bltError && surface.blts == 1 && admissionObservation.calls == 1 &&
          LastCall(surface).destinationPointer == &destination && LastCall(surface).incomingError == 0xd131,
          "diagnostic 96-pair saturation preserves actual native-forward HRESULT/LastError for unclassified auxiliary Blt");
    admissionObservation.result = S_FALSE; source.width = 1024; ClearCalls(surface); SetLastError(0xd132);
    Check(primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, DDBLT_WAIT, nullptr) == nativeFailure &&
          GetLastError() == bltError && admissionObservation.calls == 2 && admissionObservation.queries == 1 &&
          !admissionObservation.commits && LastCall(surface).incomingError == 0xd132,
          "untracked image failure keeps its real HRESULT after successful admission even when diagnostics have no remaining entries");
    surface.presentationResult = S_FALSE; ClearCalls(surface);
    Check(primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, DDBLT_WAIT, nullptr) == S_FALSE &&
          admissionObservation.calls == 3 && admissionObservation.queries == 2 && admissionObservation.commits == 1,
          "untracked auxiliary/admission/image failures do not create sticky global refusal for subsequent valid frames");
    const auto before = lakefix::GetScalePresenterDiagnostics();
    real.evaluateResult = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x6abe);
    bool nativeResults = true;
    for (unsigned index = 0; index != 1000; ++index) {
        SetLastError(0xd133); DWORD timeout = 0;
        nativeResults = draw->lpVtbl->EvaluateMode(draw, 7, &timeout) == real.evaluateResult && timeout == 24 &&
            real.incomingEvaluateError == 0xd133 && GetLastError() == nativeError && nativeResults;
    }
    const auto after = lakefix::GetScalePresenterDiagnostics();
    Check(nativeResults && after.trackedPairs == 96 && after.untrackedFailures - before.untrackedFailures == 1000 &&
          after.untrackedFailures >= full.untrackedFailures + 1003 && !admissionObservation.failureLogs,
          "diagnostic 96-pair overflow counts errors without changing native forwarded arguments/results or allocating new log budgets");
    primary->lpVtbl->Release(primary); draw->lpVtbl->Release(draw); source.iface.lpVtbl->Release(&source.iface);
    lakefix::ConfigureScalePresenter(nullptr, nullptr);
    Check(StatsAre(0, 0, 0, 0), "diagnostic budget exhaustion retains balanced presenter lifetime");
}
} // namespace

int wmain() {
    Check(lakefix::profile::Initialize() == lakefix::profile::kEnabled,
          "presenter exercises the selected profiling mode after explicit initialization");
    const auto drawTable = *NativeDrawTable(); const auto surfaceTable = *NativeSurfaceTable();
    const auto clipperTable = *NativeClipperTable();
    static_assert(sizeof(drawTable) == 30 * sizeof(void*), "DD7 completeness");
    static_assert(sizeof(surfaceTable) == 49 * sizeof(void*), "Surface7 completeness");
    Check(StatsAre(0, 0, 0, 0), "presenter starts empty");
    TestNativeIdentityAndTransfer(); TestAllForwardersAndArguments(); TestForwarderLoggingConfiguration();
    TestPresentationAndClipping();
    TestSurfaceIndependentLifetime(); TestLetterboxFallback(); TestReentrancyAndReuse(); TestBounds(); TestConcurrentTransfers();
    TestBltSnapshotArguments(); TestBltSnapshotPins(); TestBltSnapshotConfiguration();
    TestRestoredLetterbox();
    TestFrameAdmission();
    TestPresentationCommit(); TestReentrantInvalidation(); TestResourceFailureOwnership();
    TestPresentationAdmissionGuard(); TestPresentationAdmissionReentrancy();
    TestBoundedDiagnostics(); TestOpDiag96Budget();
    lakefix::ConfigureScalePresenter(nullptr, nullptr, nullptr);
    Check(!badNativeThis && !badNativeArguments, "every native fake received real this, original table and native interface arguments");
    Check(std::memcmp(&drawTable, NativeDrawTable(), sizeof(drawTable)) == 0 &&
          std::memcmp(&surfaceTable, NativeSurfaceTable(), sizeof(surfaceTable)) == 0 &&
          std::memcmp(&clipperTable, NativeClipperTable(), sizeof(clipperTable)) == 0,
          "all native vtables remain byte-for-byte unchanged");
    Check(StatsAre(0, 0, 0, 0), "presenter ends empty");
    std::printf("%s presenter fake-COM tests\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
