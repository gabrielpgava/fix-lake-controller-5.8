#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
#include "../src/ddraw_scale.h"
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
    HRESULT descriptionResult = S_OK;
    HRESULT presentationResult = S_FALSE;
    HRESULT fillResult = S_OK;
    HRESULT setClipperResult = S_OK;
    HRESULT getClipperResult = S_OK;
    unsigned descriptions = 0;
    bool descriptionSizeCorrect = true;
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
    void (*onBlt)(FakeSurface*) noexcept = nullptr;
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
    LPDDSURFACEDESC2 seenDescription = nullptr;
    LPDIRECTDRAWSURFACE7* seenOutput = nullptr;
    IUnknown* seenOuter = nullptr;
    HWND seenWindow = nullptr;
    DWORD seenFlags = 0;
    IDirectDrawSurface7* seenSurface = nullptr;
    void (*onFinalRelease)(FakeDraw*) noexcept = nullptr;
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
    if (!Native(self, 29)) return E_FAIL;
    if (timeout) *timeout = flags + 17;
    SetLastError(nativeError);
    return S_FALSE;
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
    ++value->descriptions;
    if (!output) return E_POINTER;
    value->descriptionSizeCorrect = value->descriptionSizeCorrect && output->dwSize == sizeof(*output);
    if (SUCCEEDED(value->descriptionResult)) {
        output->dwFlags = DDSD_WIDTH | DDSD_HEIGHT;
        output->dwWidth = value->width;
        output->dwHeight = value->height;
    }
    SetLastError(0x5151);
    return value->descriptionResult;
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

IDirectDraw7Vtbl* NativeDrawTable() noexcept {
    static IDirectDraw7Vtbl table = []() {
        IDirectDraw7Vtbl value{};
#define STUB(name) value.name = NativeStub<decltype(value.name), offsetof(IDirectDraw7Vtbl, name) / sizeof(void*)>::Call
        STUB(Compact); STUB(CreateClipper); STUB(CreatePalette); STUB(EnumDisplayModes);
        STUB(EnumSurfaces); STUB(FlipToGDISurface); STUB(GetCaps); STUB(GetDisplayMode);
        STUB(GetFourCCCodes); STUB(GetGDISurface); STUB(GetMonitorFrequency); STUB(GetScanLine);
        STUB(GetVerticalBlankStatus); STUB(Initialize); STUB(RestoreDisplayMode); STUB(SetDisplayMode);
        STUB(WaitForVerticalBlank); STUB(GetAvailableVidMem); STUB(GetSurfaceFromDC);
        STUB(RestoreAllSurfaces); STUB(TestCooperativeLevel); STUB(GetDeviceIdentifier); STUB(StartModeTest);
#undef STUB
        value.QueryInterface = DrawQuery; value.AddRef = DrawAddRef; value.Release = DrawRelease;
        value.CreateSurface = CreateSurface; value.DuplicateSurface = Duplicate;
        value.SetCooperativeLevel = Cooperative; value.EvaluateMode = EvaluateMode;
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
        STUB(ReleaseDC); STUB(Restore); STUB(SetColorKey); STUB(SetOverlayPosition); STUB(SetPalette);
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
    if (std::wcsstr(text, L"first failure")) ++failureLogs;
    if (std::wcsstr(text, L"bypassed: GetSurfaceDesc")) ++descriptionLogs;
    if (std::wcsstr(text, L"primary proxy allocation/capacity")) ++wrapFailureLogs;
    (void)lakefix::GetScalePresenterStats();
    lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow);
    SetLastError(0x6161);
}
bool Viewport(HWND window, LONG width, LONG height, RECT* full, RECT* fitted) noexcept {
    ++providerCalls; providerWindow = window; providerWidth = width; providerHeight = height;
    *full = fullClient; *fitted = fittedViewport;
    (void)lakefix::GetScalePresenterStats();
    lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow);
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
    lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow);
    SetLastError(0x7172);
    return clipHost;
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

void TestPresentationAndClipping() {
    ResetFixtures(); lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow);
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
          activeLogs == 1 && failureLogs == 1, "actual source query, client HWND and reentrant diagnostics preserve native LastError");
    ClearCalls(surface);
    primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    Check(surface.blts == 1 && !surface.fills && activeLogs == 1, "unchanged geometry avoids repeated fills and activation logs");
    source.width = 1920; source.height = 1080; fittedViewport = fullClient; ClearCalls(surface);
    const LONG creates = real.calls[6];
    primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    Check(providerWidth == 1920 && providerHeight == 1080 && surface.blts == 1 && real.calls[6] == creates,
          "dimensions are freshly queried without recreating any native surface");
    RECT crop{10, 20, 300, 400}; unsigned queries = source.descriptions, providers = providerCalls;
    ClearCalls(surface); primary->lpVtbl->Blt(primary, &destination, &source.iface, &crop, flags, &effects);
    Check(surface.blts == 1 && LastCall(surface).destinationPointer == &destination && LastCall(surface).sourceRect == &crop &&
          source.descriptions == queries && providerCalls == providers, "non-null sourceRect is unscaled exact argument pass-through");
    lakefix::ConfigureScalePresenter(nullptr, nullptr, nullptr); ClearCalls(surface);
    primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    Check(surface.blts == 1 && LastCall(surface).destinationPointer == &destination &&
          source.descriptions == queries && providerCalls == providers,
          "unconfigured presenter forwards full-source Blt without a descriptor query");
    lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow);
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
    Check(primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects) == DDERR_SURFACELOST &&
          surface.blts == 1 && LastCall(surface).destinationPointer == &destination && descriptionLogs == 1 && GetLastError() == bltError,
          "GetSurfaceDesc failure logs and preserves the real fallback failure");
    source.descriptionResult = S_OK; source.width = 0; ClearCalls(surface);
    primary->lpVtbl->Blt(primary, &destination, &source.iface, nullptr, flags, &effects);
    Check(surface.blts == 1 && LastCall(surface).destinationPointer == &destination, "invalid source dimensions fail open to native Blt");
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
    Check(LastCall(surface).source == &peer.iface && peer.descriptions == 1 && peer.descriptionSizeCorrect,
          "proxy source is unwrapped before source GetSurfaceDesc and real primary Blt");
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
    lakefix::ConfigureScalePresenter(FallbackViewport, nullptr, nullptr, FallbackPaint);
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
    realPrimary.presentationResult = E_ACCESSDENIED;
    Check(primary->lpVtbl->Blt(primary, &original, &source.iface, nullptr, DDBLT_WAIT, nullptr) == E_ACCESSDENIED,
          "letterbox fallback never hides a real presentation failure");
    fallbackPaintSuccess = false;
    Check(primary->lpVtbl->Blt(primary, &original, &source.iface, nullptr, DDBLT_WAIT, nullptr) == DDERR_EXCEPTION,
          "failed owned-window fallback preserves actual fill failure");
    primary->lpVtbl->Release(primary); draw->lpVtbl->Release(draw);
    source.iface.lpVtbl->Release(&source.iface);
    lakefix::ConfigureScalePresenter(nullptr, nullptr);
    Check(StatsAre(0, 0, 0, 0), "fallback test releases every proxy/context");
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
    ResetFixtures(); lakefix::ConfigureScalePresenter(Viewport, Diagnostic, ClipWindow);
    FakeDraw native[65]{}; IDirectDraw7* outputs[65]{};
    bool success = true;
    for (unsigned index = 0; index != 65; ++index) {
        Initialize(native[index]); void* owned = &native[index].iface;
        const bool wrapped = lakefix::WrapDirectDraw7(&owned);
        outputs[index] = static_cast<IDirectDraw7*>(owned);
        success = (index < 64 ? wrapped && owned != &native[index].iface : !wrapped && owned == &native[index].iface) && success;
    }
    Check(success && StatsAre(64, 0, 64, 64) && native[64].references == 1,
          "DD proxy bound fails open without changing native output or its reference");
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
        success = created == S_FALSE && GetLastError() == createError && success;
        success = (index < 256 ? surfaceOutputs[index] != &nativeSurfaces[index].iface :
                   surfaceOutputs[index] == &nativeSurfaces[index].iface) && success;
    }
    Check(success && StatsAre(1, 256, 257, 1) && wrapFailureLogs == 1 && nativeSurfaces[256].references == 1,
          "primary bound preserves real creation success/native output and logs fail-open warning");
    for (auto* output : surfaceOutputs) output->lpVtbl->Release(output);
    draw->lpVtbl->Release(draw);
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
} // namespace

int wmain() {
    const auto drawTable = *NativeDrawTable(); const auto surfaceTable = *NativeSurfaceTable();
    const auto clipperTable = *NativeClipperTable();
    static_assert(sizeof(drawTable) == 30 * sizeof(void*), "DD7 completeness");
    static_assert(sizeof(surfaceTable) == 49 * sizeof(void*), "Surface7 completeness");
    Check(StatsAre(0, 0, 0, 0), "presenter starts empty");
    TestNativeIdentityAndTransfer(); TestAllForwardersAndArguments(); TestPresentationAndClipping();
    TestSurfaceIndependentLifetime(); TestLetterboxFallback(); TestReentrancyAndReuse(); TestBounds(); TestConcurrentTransfers();
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
