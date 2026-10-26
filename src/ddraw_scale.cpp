#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
#include "ddraw_scale.h"
#include <ddraw.h>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <new>
#include <tuple>
#include <type_traits>

static_assert(sizeof(void*) == 4, "The presenter requires Windows x86");
static_assert(sizeof(IDirectDraw7Vtbl) == 30 * sizeof(void*), "Complete DD7 interface");
static_assert(sizeof(IDirectDrawSurface7Vtbl) == 49 * sizeof(void*), "Complete surface7 interface");
static_assert(sizeof(IDirectDrawClipperVtbl) == 9 * sizeof(void*), "Native clipper interface");
static_assert(offsetof(IDirectDraw7Vtbl, CreateSurface) == 6 * sizeof(void*), "CreateSurface slot");
static_assert(offsetof(IDirectDraw7Vtbl, SetCooperativeLevel) == 20 * sizeof(void*), "Cooperative slot");
static_assert(offsetof(IDirectDrawSurface7Vtbl, Blt) == 5 * sizeof(void*), "Blt slot");
static_assert(offsetof(IDirectDrawSurface7Vtbl, GetSurfaceDesc) == 22 * sizeof(void*), "Description slot");
static_assert(offsetof(IDirectDrawSurface7Vtbl, SetClipper) == 28 * sizeof(void*), "Clipper slot");
static_assert(offsetof(IDirectDrawSurface7Vtbl, GetLOD) == 48 * sizeof(void*), "Final surface slot");

namespace lakefix {
namespace {

// Local SDK interface identifiers avoid requiring dxguid/uuid import libraries.
constexpr GUID unknownId{0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID draw7Id{0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0, 0x60, 0x97, 0x97, 0xea, 0x5b}};
constexpr GUID surface7Id{0x06675a80, 0x3b9b, 0x11d2, {0xb9, 0x2f, 0, 0x60, 0x97, 0x97, 0xea, 0x5b}};

bool SameId(REFIID a, const GUID& b) noexcept { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

struct LastErrorState {
    DWORD value = GetLastError();
    ~LastErrorState() { SetLastError(value); }
    void Restore() const noexcept { SetLastError(value); }
    void Capture() noexcept { value = GetLastError(); }
};

struct Owner {
    std::atomic<unsigned long> references{1};
    std::atomic<HWND> window{nullptr};
};

// Only these owned interface objects carry our tables. Windows validates the
// native DirectDraw table identity, so NO native object/table is ever patched.
struct DrawProxy {
    using Interface = IDirectDraw7;
    IDirectDraw7 iface{};
    std::atomic<unsigned long> holds{1}; // Registry ownership plus active-call pins.
    unsigned long apiReferences = 1;     // Guarded by registryLock, NOT a native ULONG.
    DrawProxy* next = nullptr;
    IDirectDraw7* real = nullptr;
    Owner* owner = nullptr;
};

struct SurfaceProxy {
    using Interface = IDirectDrawSurface7;
    IDirectDrawSurface7 iface{};
    std::atomic<unsigned long> holds{1};
    unsigned long apiReferences = 1;
    SurfaceProxy* next = nullptr;
    IDirectDrawSurface7* real = nullptr;
    Owner* owner = nullptr;
    // Geometry fields also use registryLock, never a lock around a COM call.
    bool geometryValid = false;
    std::atomic<bool> fallbackFill{false};
    HWND filledWindow = nullptr;
    RECT filledClient{};
    RECT filledViewport{};
};

static_assert(offsetof(DrawProxy, iface) == 0, "Owned interface identity");
static_assert(offsetof(SurfaceProxy, iface) == 0, "Owned interface identity");
SRWLOCK registryLock = SRWLOCK_INIT;
DrawProxy* draws = nullptr;
SurfaceProxy* surfaces = nullptr;
std::atomic<unsigned long> liveProxies{0};
std::atomic<unsigned long> liveOwners{0};
constexpr unsigned long maxDrawObjects = 64;
constexpr unsigned long maxPrimarySurfaces = 256;
ScaleViewportProvider viewportProvider = nullptr;
ScalePresenterLog presenterLog = nullptr;
ScaleClipWindowProvider clipWindowProvider = nullptr;
ScaleLetterboxPainter letterboxPainter = nullptr;
std::atomic<bool> activeLogged{false};
std::atomic<bool> failureLogged{false};
std::atomic<bool> descriptionFailureLogged{false};
std::atomic<bool> badDimensionsLogged{false};
std::atomic<bool> badViewportLogged{false};
std::atomic<bool> primaryWrapFailureLogged{false};
std::atomic<bool> fallbackLogged{false};
thread_local bool insidePresentation = false;

struct Configuration {
    ScaleViewportProvider provider;
    ScalePresenterLog log;
    ScaleClipWindowProvider clipWindow;
    ScaleLetterboxPainter painter;
};

Configuration GetConfiguration() noexcept {
    AcquireSRWLockShared(&registryLock);
    const Configuration result{viewportProvider, presenterLog, clipWindowProvider, letterboxPainter};
    ReleaseSRWLockShared(&registryLock);
    return result;
}

void DropOwner(Owner* owner) noexcept {
    if (owner->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete owner;
        liveOwners.fetch_sub(1, std::memory_order_relaxed);
    }
}

template<class Proxy>
void DropProxy(Proxy* proxy) noexcept {
    if (proxy && proxy->holds.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        DropOwner(proxy->owner);
        // Every transferred/API native reference is released by ProxyRelease,
        // not here. This destructor must neither call COM nor inspect real.
        delete proxy;
        liveProxies.fetch_sub(1, std::memory_order_relaxed);
    }
}

DrawProxy*& ProxyList(DrawProxy*) noexcept { return draws; }
SurfaceProxy*& ProxyList(SurfaceProxy*) noexcept { return surfaces; }

template<class Proxy>
Proxy* FindProxy(typename Proxy::Interface* self) noexcept {
    AcquireSRWLockShared(&registryLock);
    Proxy* found = nullptr;
    for (auto* entry = ProxyList(static_cast<Proxy*>(nullptr)); entry; entry = entry->next) {
        if (&entry->iface == self) {
            entry->holds.fetch_add(1, std::memory_order_relaxed);
            found = entry;
            break;
        }
    }
    ReleaseSRWLockShared(&registryLock);
    return found;
}

template<class Proxy>
struct Pin {
    Proxy* value;
    explicit Pin(Proxy* proxy) noexcept : value(proxy) {}
    ~Pin() { DropProxy(value); }
    Pin(const Pin&) = delete;
    Pin& operator=(const Pin&) = delete;
    Proxy* operator->() const noexcept { return value; }
};

template<class Proxy>
bool AddApiReference(Proxy* proxy) noexcept {
    AcquireSRWLockExclusive(&registryLock);
    const bool alive = proxy->apiReferences != 0;
    if (alive) ++proxy->apiReferences;
    ReleaseSRWLockExclusive(&registryLock);
    return alive;
}

template<class Proxy>
bool RemoveApiReference(Proxy* proxy) noexcept {
    bool retired = false;
    AcquireSRWLockExclusive(&registryLock);
    const bool alive = proxy->apiReferences != 0;
    if (alive && --proxy->apiReferences == 0) {
        auto** link = &ProxyList(static_cast<Proxy*>(nullptr));
        while (*link && *link != proxy) link = &(*link)->next;
        if (*link) {
            *link = proxy->next;
            retired = true;
        }
    }
    ReleaseSRWLockExclusive(&registryLock);
    // Retire BEFORE the real Release can destroy/recreate the native object.
    // The current method's Pin, and any other active pins, keep our data alive.
    if (retired) DropProxy(proxy);
    return alive;
}

void LogOnce(ScalePresenterLog log, std::atomic<bool>& once, const wchar_t* text) noexcept {
    if (!log || once.load(std::memory_order_relaxed) || once.exchange(true, std::memory_order_relaxed)) return;
    const DWORD saved = GetLastError();
    log(text);
    SetLastError(saved);
}

void LogFailure(ScalePresenterLog log, const wchar_t* operation, HRESULT result) noexcept {
    if (!FAILED(result) || !log || failureLogged.load(std::memory_order_relaxed)) return;
    wchar_t text[192]{};
    std::swprintf(text, ARRAYSIZE(text), L"Scaled presentation first failure %ls HRESULT=0x%08lX",
                  operation, static_cast<unsigned long>(result));
    LogOnce(log, failureLogged, text);
}

void LogDescriptionFailure(ScalePresenterLog log, HRESULT result) noexcept {
    if (log && !descriptionFailureLogged.load(std::memory_order_relaxed)) {
        wchar_t text[192]{};
        std::swprintf(text, ARRAYSIZE(text), L"Scaled presentation bypassed: GetSurfaceDesc HRESULT=0x%08lX",
                      static_cast<unsigned long>(result));
        LogOnce(log, descriptionFailureLogged, text);
    }
    // The generic failure diagnostic has its own once flag.
    LogFailure(log, L"GetSurfaceDesc", result);
}

void LogActive(ScalePresenterLog log, LONG width, LONG height, const RECT& viewport) noexcept {
    if (!log || activeLogged.load(std::memory_order_relaxed)) return;
    wchar_t text[224]{};
    std::swprintf(text, ARRAYSIZE(text),
                  L"Scaled presentation active source=%ldx%ld viewport=[%ld,%ld,%ld,%ld]",
                  width, height, viewport.left, viewport.top, viewport.right, viewport.bottom);
    LogOnce(log, activeLogged, text);
}

// Lookup uses only OWN registry identities. A native argument is not dereferenced
// at all. Pins keep mapped proxy metadata alive during the native invocation;
// callers must hold valid COM references for the duration of their calls.
struct UnwrapScope {
    struct Held { DrawProxy* draw = nullptr; SurfaceProxy* surface = nullptr; } held[8]{};
    unsigned count = 0;
    ~UnwrapScope() {
        for (unsigned index = 0; index != count; ++index) {
            DropProxy(held[index].draw);
            DropProxy(held[index].surface);
        }
    }
    void* Lookup(const void* input) noexcept {
        if (!input) return nullptr;
        void* result = const_cast<void*>(input);
        AcquireSRWLockShared(&registryLock);
        for (auto* entry = draws; entry; entry = entry->next) {
            if (&entry->iface == input) {
                entry->holds.fetch_add(1, std::memory_order_relaxed);
                held[count++].draw = entry;
                result = entry->real;
                break;
            }
        }
        if (result == input) {
            for (auto* entry = surfaces; entry; entry = entry->next) {
                if (&entry->iface == input) {
                    entry->holds.fetch_add(1, std::memory_order_relaxed);
                    held[count++].surface = entry;
                    result = entry->real;
                    break;
                }
            }
        }
        ReleaseSRWLockShared(&registryLock);
        return result;
    }
    template<class T>
    decltype(auto) Map(T& value) noexcept {
        using Pointee = std::remove_cv_t<std::remove_pointer_t<T>>;
        if constexpr (std::is_pointer_v<T> &&
                      (std::is_same_v<Pointee, IDirectDraw7> || std::is_same_v<Pointee, IDirectDrawSurface7> ||
                       std::is_same_v<Pointee, IDirectDraw> || std::is_same_v<Pointee, IUnknown>)) {
            return static_cast<T>(Lookup(value));
        } else {
            // Preserve references (notably REFGUID) and all output pointers.
            return (value);
        }
    }
};

// Signature deduction preserves every SDK stdcall argument and return type.
// Unlike copying native function pointers, this always substitutes REAL this.
template<class Proxy, class Signature, auto Member> struct Forwarder;
template<class Proxy, auto Member, class Return, class Self, class... Args>
struct Forwarder<Proxy, Return (STDMETHODCALLTYPE*)(Self*, Args...), Member> {
    static Return STDMETHODCALLTYPE Call(Self* self, Args... args) {
        static_assert(std::is_same_v<Return, HRESULT>, "IUnknown uses dedicated lifetime methods");
        static_assert(sizeof...(Args) <= 8, "Unwrap pin capacity");
        LastErrorState error;
        Pin<Proxy> proxy(FindProxy<Proxy>(self));
        if (!proxy.value) return E_UNEXPECTED;
        UnwrapScope inputs;
        std::tuple<Args...> mapped{inputs.Map(args)...};
        auto* real = proxy->real;
        error.Restore();
        const Return result = std::apply([&](auto&&... nativeArgs) {
            return (real->lpVtbl->*Member)(real, nativeArgs...);
        }, mapped);
        error.Capture();
        // Logging-only paths sample the logger after native failure, so a
        // reentrant ConfigureScalePresenter in that call selects the logger.
        if (FAILED(result)) LogFailure(GetConfiguration().log, L"forwarded COM method", result);
        return result;
    }
};

template<class Proxy>
ULONG STDMETHODCALLTYPE ProxyAddRef(typename Proxy::Interface* self) {
    LastErrorState error;
    Pin<Proxy> proxy(FindProxy<Proxy>(self));
    if (!proxy.value || !AddApiReference(proxy.value)) return 0;
    error.Restore();
    const ULONG result = proxy->real->lpVtbl->AddRef(proxy->real);
    error.Capture();
    return result;
}

template<class Proxy>
ULONG STDMETHODCALLTYPE ProxyRelease(typename Proxy::Interface* self) {
    LastErrorState error;
    Pin<Proxy> proxy(FindProxy<Proxy>(self));
    if (!proxy.value || !RemoveApiReference(proxy.value)) return 0;
    error.Restore();
    const ULONG result = proxy->real->lpVtbl->Release(proxy->real);
    error.Capture();
    // No real-object access here, including when native Release returns zero.
    // Even a NONZERO native result must not keep a zero-API-ref proxy alive.
    return result;
}

template<class Proxy>
HRESULT STDMETHODCALLTYPE ProxyQueryInterface(typename Proxy::Interface* self, REFIID id, void** output) {
    LastErrorState error;
    Pin<Proxy> proxy(FindProxy<Proxy>(self));
    if (!proxy.value) return E_UNEXPECTED;
    const GUID& ownId = std::is_same_v<Proxy, DrawProxy> ? draw7Id : surface7Id;
    if (SameId(id, unknownId) || SameId(id, ownId)) {
        if (!output) return E_POINTER;
        if (!AddApiReference(proxy.value)) return E_UNEXPECTED;
        error.Restore();
        proxy->real->lpVtbl->AddRef(proxy->real);
        error.Capture();
        *output = &proxy->iface;
        return S_OK; // These two interfaces are implemented by this proxy.
    }
    // D3D7, old DirectDraw versions, etc. stay completely native.
    error.Restore();
    const HRESULT result = proxy->real->lpVtbl->QueryInterface(proxy->real, id, output);
    error.Capture();
    if (FAILED(result)) LogFailure(GetConfiguration().log, L"QueryInterface", result);
    return result;
}

IDirectDraw7Vtbl* DrawTable() noexcept;
IDirectDrawSurface7Vtbl* SurfaceTable() noexcept;

template<class Proxy>
typename Proxy::Interface* ExistingLocked(typename Proxy::Interface* input) noexcept {
    for (auto* entry = ProxyList(static_cast<Proxy*>(nullptr)); entry; entry = entry->next) {
        if (&entry->iface == input) return input; // Already-owned proxy: no new ref.
        if (entry->real == input) {
            ++entry->apiReferences; // Adopt the NEW native reference, no extra AddRef.
            return &entry->iface;
        }
    }
    return nullptr;
}

template<class Proxy>
bool AdoptExisting(typename Proxy::Interface* input, typename Proxy::Interface** output) noexcept {
    AcquireSRWLockExclusive(&registryLock);
    auto* existing = ExistingLocked<Proxy>(input);
    if (existing) *output = existing;
    ReleaseSRWLockExclusive(&registryLock);
    return existing != nullptr;
}

template<class Proxy, class Table>
bool Adopt(typename Proxy::Interface* real, Owner* owner, Table* table,
           typename Proxy::Interface** output) noexcept {
    if (AdoptExisting<Proxy>(real, output)) return true;
    auto* candidate = new (std::nothrow) Proxy;
    if (!candidate) return false;
    candidate->iface.lpVtbl = table;
    candidate->real = real;
    candidate->owner = owner;
    owner->references.fetch_add(1, std::memory_order_relaxed);
    liveProxies.fetch_add(1, std::memory_order_relaxed);
    bool inserted = false;
    bool success = false;
    AcquireSRWLockExclusive(&registryLock);
    if (auto* existing = ExistingLocked<Proxy>(real)) {
        *output = existing;
        success = true;
    } else {
        unsigned long count = 0;
        auto*& head = ProxyList(static_cast<Proxy*>(nullptr));
        for (auto* entry = head; entry; entry = entry->next) ++count;
        constexpr auto maximum = std::is_same_v<Proxy, DrawProxy> ? maxDrawObjects : maxPrimarySurfaces;
        if (count < maximum) {
            candidate->next = head;
            head = candidate;
            *output = &candidate->iface;
            inserted = success = true;
        }
    }
    ReleaseSRWLockExclusive(&registryLock);
    if (!inserted) DropProxy(candidate); // Never release the caller's native ref on failure.
    return success;
}

HRESULT STDMETHODCALLTYPE DrawCreateSurface(IDirectDraw7* self, LPDDSURFACEDESC2 description,
                                             LPDIRECTDRAWSURFACE7* output, IUnknown* outer) {
    LastErrorState error;
    Pin<DrawProxy> proxy(FindProxy<DrawProxy>(self));
    if (!proxy.value) return E_UNEXPECTED;
    const bool primary = description && description->dwSize == sizeof(DDSURFACEDESC2) &&
                         (description->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) != 0;
    UnwrapScope inputs;
    auto* nativeOuter = inputs.Map(outer);
    error.Restore();
    const HRESULT result = proxy->real->lpVtbl->CreateSurface(proxy->real, description, output, nativeOuter);
    error.Capture();
    if (SUCCEEDED(result) && primary && output && *output) {
        auto* replacement = *output;
        if (Adopt<SurfaceProxy>(*output, proxy->owner, SurfaceTable(), &replacement)) *output = replacement;
        else LogOnce(GetConfiguration().log, primaryWrapFailureLogged,
                     L"Scaled presentation bypassed: primary proxy allocation/capacity unavailable; native output retained");
    }
    if (FAILED(result)) LogFailure(GetConfiguration().log, L"CreateSurface", result);
    return result;
}

HRESULT STDMETHODCALLTYPE DrawSetCooperativeLevel(IDirectDraw7* self, HWND window, DWORD flags) {
    LastErrorState error;
    Pin<DrawProxy> proxy(FindProxy<DrawProxy>(self));
    if (!proxy.value) return E_UNEXPECTED;
    error.Restore();
    const HRESULT result = proxy->real->lpVtbl->SetCooperativeLevel(proxy->real, window, flags);
    error.Capture();
    if (SUCCEEDED(result)) proxy->owner->window.store(window, std::memory_order_release);
    if (FAILED(result)) LogFailure(GetConfiguration().log, L"SetCooperativeLevel", result);
    return result;
}

bool SameRect(const RECT& a, const RECT& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
bool HasArea(const RECT& r) noexcept { return r.left < r.right && r.top < r.bottom; }
bool ValidViewport(const RECT& full, const RECT& fitted) noexcept {
    return HasArea(full) && HasArea(fitted) && fitted.left >= full.left && fitted.top >= full.top &&
           fitted.right <= full.right && fitted.bottom <= full.bottom;
}
void InvalidateGeometry(SurfaceProxy* proxy) noexcept {
    AcquireSRWLockExclusive(&registryLock);
    proxy->geometryValid = false;
    ReleaseSRWLockExclusive(&registryLock);
}
bool NeedLetterbox(SurfaceProxy* proxy, HWND window, const RECT& full, const RECT& fitted) noexcept {
    AcquireSRWLockShared(&registryLock);
    const bool result = !proxy->geometryValid || proxy->filledWindow != window ||
                        !SameRect(proxy->filledClient, full) || !SameRect(proxy->filledViewport, fitted);
    ReleaseSRWLockShared(&registryLock);
    return result;
}
void RememberGeometry(SurfaceProxy* proxy, HWND window, const RECT& full, const RECT& fitted) noexcept {
    AcquireSRWLockExclusive(&registryLock);
    proxy->filledWindow = window;
    proxy->filledClient = full;
    proxy->filledViewport = fitted;
    proxy->geometryValid = true;
    ReleaseSRWLockExclusive(&registryLock);
}

HRESULT STDMETHODCALLTYPE SurfaceGetClipper(IDirectDrawSurface7* self, LPDIRECTDRAWCLIPPER* output) {
    LastErrorState error;
    Pin<SurfaceProxy> proxy(FindProxy<SurfaceProxy>(self));
    if (!proxy.value) return E_UNEXPECTED;
    error.Restore();
    const HRESULT result = proxy->real->lpVtbl->GetClipper(proxy->real, output);
    error.Capture();
    if (FAILED(result)) LogFailure(GetConfiguration().log, L"GetClipper", result);
    return result;
}

HRESULT STDMETHODCALLTYPE SurfaceSetClipper(IDirectDrawSurface7* self, LPDIRECTDRAWCLIPPER clipper) {
    LastErrorState error;
    Pin<SurfaceProxy> proxy(FindProxy<SurfaceProxy>(self));
    if (!proxy.value) return E_UNEXPECTED;
    const auto config = GetConfiguration();
    const HWND owner = proxy->owner->window.load(std::memory_order_acquire);
    error.Restore();
    const HWND target = config.clipWindow ? config.clipWindow(owner) : nullptr;
    if (clipper && owner && target && target != owner) {
        HWND current = nullptr;
        error.Restore();
        const HRESULT queried = clipper->lpVtbl->GetHWnd(clipper, &current);
        LogFailure(config.log, L"clipper GetHWnd", queried);
        if (SUCCEEDED(queried) && current == owner) {
            error.Restore();
            const HRESULT changed = clipper->lpVtbl->SetHWnd(clipper, 0, target);
            if (FAILED(changed)) {
                error.Capture();
                LogFailure(config.log, L"clipper SetHWnd", changed);
                return changed;
            }
        }
    }
    // Retargeting/query/provider LastError must not leak to the actual SetClipper.
    error.Restore();
    const HRESULT result = proxy->real->lpVtbl->SetClipper(proxy->real, clipper);
    error.Capture();
    if (SUCCEEDED(result)) InvalidateGeometry(proxy.value);
    LogFailure(config.log, L"SetClipper", result);
    return result;
}

HRESULT CallBlt(SurfaceProxy* proxy, LPRECT destination, LPDIRECTDRAWSURFACE7 source,
                LPRECT sourceRect, DWORD flags, LPDDBLTFX effects, LastErrorState& error,
                ScalePresenterLog log, const wchar_t* operation) noexcept {
    error.Restore();
    const HRESULT result = proxy->real->lpVtbl->Blt(proxy->real, destination, source, sourceRect, flags, effects);
    error.Capture();
    LogFailure(log, operation, result);
    return result;
}

struct PresentationScope {
    bool nested = insidePresentation;
    PresentationScope() noexcept { insidePresentation = true; }
    ~PresentationScope() { insidePresentation = nested; }
};

HRESULT STDMETHODCALLTYPE SurfaceBlt(IDirectDrawSurface7* self, LPRECT destination,
                                     LPDIRECTDRAWSURFACE7 source, LPRECT sourceRect,
                                     DWORD flags, LPDDBLTFX effects) {
    LastErrorState error;
    Pin<SurfaceProxy> proxy(FindProxy<SurfaceProxy>(self));
    if (!proxy.value) return E_UNEXPECTED;
    UnwrapScope inputs;
    auto* realSource = inputs.Map(source);
    const auto config = GetConfiguration();
    PresentationScope scope;
    const auto forward = [&]() noexcept {
        InvalidateGeometry(proxy.value);
        return CallBlt(proxy.value, destination, realSource, sourceRect, flags, effects, error, config.log, L"Blt");
    };
    if (scope.nested || !realSource || sourceRect || !config.provider) return forward();
    DDSURFACEDESC2 description{};
    description.dwSize = sizeof(description);
    error.Restore();
    const HRESULT described = realSource->lpVtbl->GetSurfaceDesc(realSource, &description);
    if (FAILED(described)) {
        LogDescriptionFailure(config.log, described);
        return forward();
    }
    constexpr DWORD maxDimension = 0x7fffffffUL;
    if (!description.dwWidth || !description.dwHeight || description.dwWidth > maxDimension ||
        description.dwHeight > maxDimension) {
        LogOnce(config.log, badDimensionsLogged, L"Scaled presentation bypassed: invalid source dimensions");
        return forward();
    }
    const LONG width = static_cast<LONG>(description.dwWidth);
    const LONG height = static_cast<LONG>(description.dwHeight);
    const HWND window = proxy->owner->window.load(std::memory_order_acquire);
    RECT full{};
    RECT fitted{};
    error.Restore();
    if (!config.provider(window, width, height, &full, &fitted)) return forward();
    if (!window || !ValidViewport(full, fitted)) {
        LogOnce(config.log, badViewportLogged, L"Scaled presentation bypassed: invalid owner viewport");
        return forward();
    }
    const DWORD callerError = error.value;
    if (NeedLetterbox(proxy.value, window, full, fitted)) {
        RECT strips[] = {
            {full.left, full.top, full.right, fitted.top},
            {full.left, fitted.bottom, full.right, full.bottom},
            {full.left, fitted.top, fitted.left, fitted.bottom},
            {fitted.right, fitted.top, full.right, fitted.bottom}
        };
        // Empty strips need neither native fills nor the latched fallback
        // painter. The real image Blt below still decides success/geometry.
        bool painted = !HasArea(strips[0]) && !HasArea(strips[1]) && !HasArea(strips[2]) && !HasArea(strips[3]);
        if (!painted && proxy->fallbackFill.load() && config.painter)
            painted = config.painter(window, strips, ARRAYSIZE(strips));
        DDBLTFX fill{};
        fill.dwSize = sizeof(fill);
        fill.dwFillColor = 0;
        for (RECT& strip : strips) {
            if (painted) break;
            if (!HasArea(strip)) continue;
            error.value = callerError;
            const HRESULT result = CallBlt(proxy.value, &strip, nullptr, nullptr,
                                           DDBLT_COLORFILL | DDBLT_WAIT, &fill, error, config.log, L"letterbox Blt");
            if (FAILED(result)) {
                if (config.painter && config.painter(window, strips, ARRAYSIZE(strips))) {
                    proxy->fallbackFill.store(true);
                    painted = true;
                    LogOnce(config.log, fallbackLogged,
                        L"Primary COLORFILL unavailable: owned-host GDI letterbox fallback active; presentation HRESULT remains real");
                    break;
                }
                InvalidateGeometry(proxy.value);
                return result;
            }
        }
    }
    error.value = callerError;
    const HRESULT result = CallBlt(proxy.value, &fitted, realSource, nullptr, flags, effects,
                                   error, config.log, L"presentation Blt");
    if (SUCCEEDED(result)) {
        RememberGeometry(proxy.value, window, full, fitted);
        LogActive(config.log, width, height, fitted);
    } else InvalidateGeometry(proxy.value);
    return result;
}

IDirectDraw7Vtbl* DrawTable() noexcept {
    static IDirectDraw7Vtbl table = []() {
        IDirectDraw7Vtbl value{};
#define DD_FORWARD(method) value.method = Forwarder<DrawProxy, decltype(value.method), &IDirectDraw7Vtbl::method>::Call
        DD_FORWARD(Compact);
        DD_FORWARD(CreateClipper);
        DD_FORWARD(CreatePalette);
        DD_FORWARD(DuplicateSurface);
        DD_FORWARD(EnumDisplayModes);
        DD_FORWARD(EnumSurfaces);
        DD_FORWARD(FlipToGDISurface);
        DD_FORWARD(GetCaps);
        DD_FORWARD(GetDisplayMode);
        DD_FORWARD(GetFourCCCodes);
        DD_FORWARD(GetGDISurface);
        DD_FORWARD(GetMonitorFrequency);
        DD_FORWARD(GetScanLine);
        DD_FORWARD(GetVerticalBlankStatus);
        DD_FORWARD(Initialize);
        DD_FORWARD(RestoreDisplayMode);
        DD_FORWARD(SetDisplayMode);
        DD_FORWARD(WaitForVerticalBlank);
        DD_FORWARD(GetAvailableVidMem);
        DD_FORWARD(GetSurfaceFromDC);
        DD_FORWARD(RestoreAllSurfaces);
        DD_FORWARD(TestCooperativeLevel);
        DD_FORWARD(GetDeviceIdentifier);
        DD_FORWARD(StartModeTest);
        DD_FORWARD(EvaluateMode);
#undef DD_FORWARD
        value.QueryInterface = ProxyQueryInterface<DrawProxy>;
        value.AddRef = ProxyAddRef<DrawProxy>;
        value.Release = ProxyRelease<DrawProxy>;
        value.CreateSurface = DrawCreateSurface;
        value.SetCooperativeLevel = DrawSetCooperativeLevel;
        return value;
    }();
    return &table;
}

IDirectDrawSurface7Vtbl* SurfaceTable() noexcept {
    static IDirectDrawSurface7Vtbl table = []() {
        IDirectDrawSurface7Vtbl value{};
#define SURFACE_FORWARD(method) value.method = Forwarder<SurfaceProxy, decltype(value.method), &IDirectDrawSurface7Vtbl::method>::Call
        SURFACE_FORWARD(AddAttachedSurface);
        SURFACE_FORWARD(AddOverlayDirtyRect);
        SURFACE_FORWARD(BltBatch); // Native array/effect pointers, no recursive remapping.
        SURFACE_FORWARD(BltFast);
        SURFACE_FORWARD(DeleteAttachedSurface);
        SURFACE_FORWARD(EnumAttachedSurfaces);
        SURFACE_FORWARD(EnumOverlayZOrders);
        SURFACE_FORWARD(Flip); // Forwarded, not a scaling presentation path.
        SURFACE_FORWARD(GetAttachedSurface); // Native returned aliases, deliberately not wrapped.
        SURFACE_FORWARD(GetBltStatus);
        SURFACE_FORWARD(GetCaps);
        SURFACE_FORWARD(GetColorKey);
        SURFACE_FORWARD(GetDC);
        SURFACE_FORWARD(GetFlipStatus);
        SURFACE_FORWARD(GetOverlayPosition);
        SURFACE_FORWARD(GetPalette);
        SURFACE_FORWARD(GetPixelFormat);
        SURFACE_FORWARD(GetSurfaceDesc);
        SURFACE_FORWARD(Initialize);
        SURFACE_FORWARD(IsLost);
        SURFACE_FORWARD(Lock);
        SURFACE_FORWARD(ReleaseDC);
        SURFACE_FORWARD(Restore);
        SURFACE_FORWARD(SetColorKey);
        SURFACE_FORWARD(SetOverlayPosition);
        SURFACE_FORWARD(SetPalette);
        SURFACE_FORWARD(Unlock);
        SURFACE_FORWARD(UpdateOverlay);
        SURFACE_FORWARD(UpdateOverlayDisplay);
        SURFACE_FORWARD(UpdateOverlayZOrder);
        SURFACE_FORWARD(GetDDInterface); // Native alias, not universal COM identity emulation.
        SURFACE_FORWARD(PageLock);
        SURFACE_FORWARD(PageUnlock);
        SURFACE_FORWARD(SetSurfaceDesc);
        SURFACE_FORWARD(SetPrivateData);
        SURFACE_FORWARD(GetPrivateData);
        SURFACE_FORWARD(FreePrivateData);
        SURFACE_FORWARD(GetUniquenessValue);
        SURFACE_FORWARD(ChangeUniquenessValue);
        SURFACE_FORWARD(SetPriority);
        SURFACE_FORWARD(GetPriority);
        SURFACE_FORWARD(SetLOD);
        SURFACE_FORWARD(GetLOD);
#undef SURFACE_FORWARD
        value.QueryInterface = ProxyQueryInterface<SurfaceProxy>;
        value.AddRef = ProxyAddRef<SurfaceProxy>;
        value.Release = ProxyRelease<SurfaceProxy>;
        value.Blt = SurfaceBlt;
        value.GetClipper = SurfaceGetClipper;
        value.SetClipper = SurfaceSetClipper;
        return value;
    }();
    return &table;
}

} // namespace

void ConfigureScalePresenter(ScaleViewportProvider provider, ScalePresenterLog log,
                              ScaleClipWindowProvider clipWindow, ScaleLetterboxPainter painter) noexcept {
    LastErrorState error;
    AcquireSRWLockExclusive(&registryLock);
    viewportProvider = provider;
    presenterLog = log;
    clipWindowProvider = clipWindow;
    letterboxPainter = painter;
    ReleaseSRWLockExclusive(&registryLock);
}

bool WrapDirectDraw7(void** object) noexcept {
    LastErrorState error;
    if (!object || !*object) return false;
    auto* real = static_cast<IDirectDraw7*>(*object);
    auto* replacement = real;
    if (AdoptExisting<DrawProxy>(real, &replacement)) {
        *object = replacement;
        return true;
    }
    auto* owner = new (std::nothrow) Owner;
    if (!owner) return false;
    liveOwners.fetch_add(1, std::memory_order_relaxed);
    const bool success = Adopt<DrawProxy>(real, owner, DrawTable(), &replacement);
    DropOwner(owner);
    if (success) *object = replacement;
    return success;
}

ScalePresenterStats GetScalePresenterStats() noexcept {
    LastErrorState error;
    ScalePresenterStats result{};
    AcquireSRWLockShared(&registryLock);
    for (auto* entry = draws; entry; entry = entry->next) ++result.directDrawObjects;
    for (auto* entry = surfaces; entry; entry = entry->next) ++result.primarySurfaces;
    result.liveProxyObjects = liveProxies.load(std::memory_order_relaxed);
    result.ownerContexts = liveOwners.load(std::memory_order_relaxed);
    ReleaseSRWLockShared(&registryLock);
    return result;
}

} // namespace lakefix
