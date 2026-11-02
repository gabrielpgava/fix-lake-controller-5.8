#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
#include "ddraw_scale.h"
#include "profile.h"
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
static_assert(offsetof(IDirectDraw7Vtbl, RestoreAllSurfaces) == 25 * sizeof(void*), "Restore owner slot");
static_assert(offsetof(IDirectDrawSurface7Vtbl, Blt) == 5 * sizeof(void*), "Blt slot");
static_assert(offsetof(IDirectDrawSurface7Vtbl, GetSurfaceDesc) == 22 * sizeof(void*), "Description slot");
static_assert(offsetof(IDirectDrawSurface7Vtbl, SetClipper) == 28 * sizeof(void*), "Clipper slot");
static_assert(offsetof(IDirectDrawSurface7Vtbl, ReleaseDC) == 26 * sizeof(void*), "ReleaseDC slot");
static_assert(offsetof(IDirectDrawSurface7Vtbl, Restore) == 27 * sizeof(void*), "Restore slot");
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
    unsigned long long geometryVersion = 0; // registryLock, including retired pinned owners.
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
    unsigned long long geometryVersion = 0;
    unsigned long long filledOwnerVersion = 0;
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
ScalePresentationCommit presentationCommit = nullptr;
ScalePresentationAdmission presentationAdmission = nullptr;
LONG expectedWidth = 0, expectedHeight = 0;
unsigned long drawCapacity = maxDrawObjects, primaryCapacity = maxPrimarySurfaces;
std::atomic<unsigned long> allocationBudget{~0UL};
std::atomic<bool> activeLogged{false};
std::atomic<bool> badDimensionsLogged{false};
std::atomic<bool> badViewportLogged{false};
std::atomic<bool> fallbackLogged{false};
thread_local bool insidePresentation = false;

struct Configuration {
    ScaleViewportProvider provider;
    ScalePresenterLog log;
    ScaleClipWindowProvider clipWindow;
    ScaleLetterboxPainter painter;
    LONG width;
    LONG height;
    ScalePresentationCommit commit;
    ScalePresentationAdmission admission;
};

Configuration GetConfiguration() noexcept {
    AcquireSRWLockShared(&registryLock);
    const Configuration result{viewportProvider, presenterLog, clipWindowProvider, letterboxPainter,
                               expectedWidth, expectedHeight, presentationCommit, presentationAdmission};
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
Proxy* FindProxyLocked(typename Proxy::Interface* self) noexcept {
    Proxy* found = nullptr;
    for (auto* entry = ProxyList(static_cast<Proxy*>(nullptr)); entry; entry = entry->next) {
        if (&entry->iface == self) {
            entry->holds.fetch_add(1, std::memory_order_relaxed);
            found = entry;
            break;
        }
    }
    return found;
}

template<class Proxy>
Proxy* FindProxy(typename Proxy::Interface* self) noexcept {
    AcquireSRWLockShared(&registryLock);
    auto* found = FindProxyLocked<Proxy>(self);
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

constexpr unsigned surfaceOperationBase = 30;
constexpr unsigned sourceDescriptionOperation = 79, presentationOperation = 80, letterboxOperation = 81;
constexpr unsigned clipperGetWindowOperation = 82, clipperSetWindowOperation = 83;
constexpr unsigned wrapDrawOperation = 84, wrapPrimaryOperation = 85, admissionOperation = 86, operationCount = 87;
const wchar_t* const operationNames[] = {
    L"DD7.QueryInterface", L"DD7.AddRef", L"DD7.Release", L"DD7.Compact", L"DD7.CreateClipper",
    L"DD7.CreatePalette", L"DD7.CreateSurface", L"DD7.DuplicateSurface", L"DD7.EnumDisplayModes",
    L"DD7.EnumSurfaces", L"DD7.FlipToGDISurface", L"DD7.GetCaps", L"DD7.GetDisplayMode",
    L"DD7.GetFourCCCodes", L"DD7.GetGDISurface", L"DD7.GetMonitorFrequency", L"DD7.GetScanLine",
    L"DD7.GetVerticalBlankStatus", L"DD7.Initialize", L"DD7.RestoreDisplayMode", L"DD7.SetCooperativeLevel",
    L"DD7.SetDisplayMode", L"DD7.WaitForVerticalBlank", L"DD7.GetAvailableVidMem", L"DD7.GetSurfaceFromDC",
    L"DD7.RestoreAllSurfaces", L"DD7.TestCooperativeLevel", L"DD7.GetDeviceIdentifier", L"DD7.StartModeTest", L"DD7.EvaluateMode",
    L"Surface7.QueryInterface", L"Surface7.AddRef", L"Surface7.Release", L"Surface7.AddAttachedSurface",
    L"Surface7.AddOverlayDirtyRect", L"Surface7.Blt", L"Surface7.BltBatch", L"Surface7.BltFast",
    L"Surface7.DeleteAttachedSurface", L"Surface7.EnumAttachedSurfaces", L"Surface7.EnumOverlayZOrders",
    L"Surface7.Flip", L"Surface7.GetAttachedSurface", L"Surface7.GetBltStatus", L"Surface7.GetCaps",
    L"Surface7.GetClipper", L"Surface7.GetColorKey", L"Surface7.GetDC", L"Surface7.GetFlipStatus",
    L"Surface7.GetOverlayPosition", L"Surface7.GetPalette", L"Surface7.GetPixelFormat", L"Surface7.GetSurfaceDesc",
    L"Surface7.Initialize", L"Surface7.IsLost", L"Surface7.Lock", L"Surface7.ReleaseDC", L"Surface7.Restore",
    L"Surface7.SetClipper", L"Surface7.SetColorKey", L"Surface7.SetOverlayPosition", L"Surface7.SetPalette",
    L"Surface7.Unlock", L"Surface7.UpdateOverlay", L"Surface7.UpdateOverlayDisplay", L"Surface7.UpdateOverlayZOrder",
    L"Surface7.GetDDInterface", L"Surface7.PageLock", L"Surface7.PageUnlock", L"Surface7.SetSurfaceDesc",
    L"Surface7.SetPrivateData", L"Surface7.GetPrivateData", L"Surface7.FreePrivateData", L"Surface7.GetUniquenessValue",
    L"Surface7.ChangeUniquenessValue", L"Surface7.SetPriority", L"Surface7.GetPriority", L"Surface7.SetLOD", L"Surface7.GetLOD",
    L"source GetSurfaceDesc", L"presentation Blt", L"letterbox Blt", L"clipper GetHWnd", L"clipper SetHWnd",
    L"WrapDirectDraw7", L"primary proxy allocation/capacity", L"presentation admission"
};
static_assert(ARRAYSIZE(operationNames) == operationCount, "Every diagnostic operation has a name");
constexpr unsigned diagnosticCapacity = 96, messagesPerPair = 4;
struct DiagnosticEntry {
    unsigned operation = 0;
    HRESULT result = S_OK;
    unsigned long failures = 0, recoveries = 0, messages = 0;
    bool outstanding = false;
};
SRWLOCK diagnosticLock = SRWLOCK_INIT;
DiagnosticEntry diagnosticEntries[diagnosticCapacity]{};
unsigned diagnosticCount = 0;
ScalePresenterDiagnostics diagnostics{};
std::atomic<bool> pendingFailure[operationCount]{};
void Increment(unsigned long& count) noexcept { if (count != ~0UL) ++count; }

void LogResult(ScalePresenterLog log, unsigned operation, HRESULT result) noexcept {
    if (SUCCEEDED(result) && !pendingFailure[operation].load(std::memory_order_relaxed)) return;
    LastErrorState error;
    struct Notification { DiagnosticEntry entry; bool recovered; } notifications[diagnosticCapacity];
    unsigned count = 0;
    AcquireSRWLockExclusive(&diagnosticLock);
    const auto notify = [&](DiagnosticEntry& entry, bool recovered, bool eligible) noexcept {
        if (!log || !eligible) return;
        if (entry.messages == messagesPerPair) { Increment(diagnostics.suppressedMessages); return; }
        ++entry.messages;
        notifications[count++] = {entry, recovered};
    };
    if (FAILED(result)) {
        Increment(diagnostics.failures);
        DiagnosticEntry* found = nullptr;
        for (unsigned index = 0; index != diagnosticCount; ++index)
            if (diagnosticEntries[index].operation == operation && diagnosticEntries[index].result == result)
                found = &diagnosticEntries[index];
        if (!found && diagnosticCount != diagnosticCapacity) {
            found = &diagnosticEntries[diagnosticCount++];
            found->operation = operation; found->result = result;
            diagnostics.trackedPairs = diagnosticCount;
        }
        if (found) {
            const bool wasOutstanding = found->outstanding;
            Increment(found->failures); found->outstanding = true;
            pendingFailure[operation].store(true, std::memory_order_relaxed);
            notify(*found, false, !found->messages || !wasOutstanding || found->failures == 16 || found->failures == 256);
        } else Increment(diagnostics.untrackedFailures);
    } else {
        for (unsigned index = 0; index != diagnosticCount; ++index) {
            auto& entry = diagnosticEntries[index];
            if (entry.operation != operation || !entry.outstanding) continue;
            entry.outstanding = false; Increment(entry.recoveries); Increment(diagnostics.recoveries);
            notify(entry, true, true);
        }
        pendingFailure[operation].store(false, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&diagnosticLock);
    for (unsigned index = 0; index != count; ++index) {
        const auto& notification = notifications[index];
        wchar_t text[256]{};
        std::swprintf(text, ARRAYSIZE(text),
            L"Scaled presentation %ls %ls HRESULT=0x%08lX failures=%lu recoveries=%lu message=%lu/%u",
            notification.recovered ? L"recovered" : L"failure", operationNames[operation],
            static_cast<unsigned long>(notification.entry.result), notification.entry.failures,
            notification.entry.recoveries, notification.entry.messages, messagesPerPair);
        error.Restore(); log(text); // No presenter lock, even for reentrant loggers.
    }
}

bool AdmitAllocation() noexcept {
    auto remaining = allocationBudget.load(std::memory_order_relaxed);
    while (remaining != ~0UL) {
        if (!remaining) return false;
        if (allocationBudget.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) break;
    }
    return true;
}

void InvalidateGeometry(SurfaceProxy*) noexcept;
void InvalidateOwnerGeometry(DrawProxy* proxy) noexcept {
    AcquireSRWLockExclusive(&registryLock);
    ++proxy->owner->geometryVersion;
    ReleaseSRWLockExclusive(&registryLock);
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
    void* LookupLocked(const void* input, bool* knownPrimary = nullptr) noexcept {
        if (!input) return nullptr;
        void* result = const_cast<void*>(input);
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
                if (knownPrimary && entry->real == input) *knownPrimary = true;
                if (&entry->iface == input) {
                    if (knownPrimary) *knownPrimary = true;
                    entry->holds.fetch_add(1, std::memory_order_relaxed);
                    held[count++].surface = entry;
                    result = entry->real;
                    break;
                }
            }
        }
        return result;
    }
    void* Lookup(const void* input) noexcept {
        if (!input) return nullptr;
        AcquireSRWLockShared(&registryLock);
        void* result = LookupLocked(input);
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

// Capture identities/pins and the callback tuple at one registry read point.
// No native pointers are dereferenced, and no COM/callback/destructor runs
// while holding the lock. Source pins are dropped before the destination pin,
// just as in the separate FindProxy + UnwrapScope path.
struct PresentationSnapshot {
    Pin<SurfaceProxy> destination{nullptr};
    UnwrapScope inputs;
    IDirectDrawSurface7* source = nullptr;
    bool sourceIsPrimary = false;
    HWND window = nullptr;
    unsigned long long ownerVersion = 0;
    Configuration config{};
    PresentationSnapshot(IDirectDrawSurface7* self, IDirectDrawSurface7* input) noexcept {
        [[maybe_unused]] profile::Scope timing(profile::Metric::PresenterSnapshot);
        AcquireSRWLockShared(&registryLock);
        destination.value = FindProxyLocked<SurfaceProxy>(self);
        if (destination.value) {
            source = static_cast<IDirectDrawSurface7*>(inputs.LookupLocked(input, &sourceIsPrimary));
            window = destination->owner->window.load(std::memory_order_acquire);
            ownerVersion = destination->owner->geometryVersion;
            config = {viewportProvider, presenterLog, clipWindowProvider, letterboxPainter,
                      expectedWidth, expectedHeight, presentationCommit, presentationAdmission};
        }
        ReleaseSRWLockShared(&registryLock);
    }
    PresentationSnapshot(const PresentationSnapshot&) = delete;
    PresentationSnapshot& operator=(const PresentationSnapshot&) = delete;
};

// Signature deduction preserves every SDK stdcall argument and return type.
// Unlike copying native function pointers, this always substitutes REAL this.
template<class Proxy, class Signature, auto Member, unsigned Operation, void (*OnSuccess)(Proxy*) = nullptr> struct Forwarder;
template<class Proxy, auto Member, unsigned Operation, void (*OnSuccess)(Proxy*), class Return, class Self, class... Args>
struct Forwarder<Proxy, Return (STDMETHODCALLTYPE*)(Self*, Args...), Member, Operation, OnSuccess> {
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
        if constexpr (OnSuccess != nullptr) if (SUCCEEDED(result)) OnSuccess(proxy.value);
        // Logging-only paths sample the logger after native failure, so a
        // reentrant ConfigureScalePresenter in that call selects the logger.
        if (FAILED(result) || pendingFailure[Operation].load(std::memory_order_relaxed))
            LogResult(GetConfiguration().log, Operation, result);
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
    constexpr unsigned operation = std::is_same_v<Proxy, DrawProxy> ? 0 : surfaceOperationBase;
    LogResult(GetConfiguration().log, operation, result);
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
HRESULT Adopt(typename Proxy::Interface* real, Owner* owner, Table* table,
            typename Proxy::Interface** output) noexcept {
    if (AdoptExisting<Proxy>(real, output)) return S_OK;
    if (!AdmitAllocation()) return E_OUTOFMEMORY;
    auto* candidate = new (std::nothrow) Proxy;
    if (!candidate) return E_OUTOFMEMORY;
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
        const auto maximum = std::is_same_v<Proxy, DrawProxy> ? drawCapacity : primaryCapacity;
        if (count < maximum) {
            candidate->next = head;
            head = candidate;
            *output = &candidate->iface;
            inserted = success = true;
        }
    }
    ReleaseSRWLockExclusive(&registryLock);
    if (!inserted) DropProxy(candidate); // Never release the caller's native ref on failure.
    return success ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
}

HRESULT STDMETHODCALLTYPE DrawCreateSurface(IDirectDraw7* self, LPDDSURFACEDESC2 description,
                                             LPDIRECTDRAWSURFACE7* output, IUnknown* outer) {
    LastErrorState error;
    Pin<DrawProxy> proxy(FindProxy<DrawProxy>(self));
    if (!proxy.value) return E_UNEXPECTED;
    const bool primary = description && description->dwSize == sizeof(DDSURFACEDESC2) &&
                         (description->dwFlags & DDSD_CAPS) != 0 &&
                         (description->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) != 0;
    UnwrapScope inputs;
    auto* nativeOuter = inputs.Map(outer);
    error.Restore();
    HRESULT result = proxy->real->lpVtbl->CreateSurface(proxy->real, description, output, nativeOuter);
    error.Capture();
    if (SUCCEEDED(result) && primary && output && *output) {
        auto* native = *output;
        auto* replacement = native;
        const HRESULT wrapped = Adopt<SurfaceProxy>(native, proxy->owner, SurfaceTable(), &replacement);
        if (SUCCEEDED(wrapped)) *output = replacement;
        else {
            *output = nullptr; // No scaled-input/native-presenter split on resource exhaustion.
            error.Restore();
            native->lpVtbl->Release(native); // Exactly the returned reference, outside locks.
            result = wrapped;
        }
        LogResult(GetConfiguration().log, wrapPrimaryOperation, wrapped);
    }
    LogResult(GetConfiguration().log, 6, result);
    return result;
}

HRESULT STDMETHODCALLTYPE DrawSetCooperativeLevel(IDirectDraw7* self, HWND window, DWORD flags) {
    LastErrorState error;
    Pin<DrawProxy> proxy(FindProxy<DrawProxy>(self));
    if (!proxy.value) return E_UNEXPECTED;
    error.Restore();
    const HRESULT result = proxy->real->lpVtbl->SetCooperativeLevel(proxy->real, window, flags);
    error.Capture();
    if (SUCCEEDED(result)) {
        AcquireSRWLockExclusive(&registryLock);
        proxy->owner->window.store(window, std::memory_order_release);
        ++proxy->owner->geometryVersion;
        ReleaseSRWLockExclusive(&registryLock);
    }
    LogResult(GetConfiguration().log, 20, result);
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
    ++proxy->geometryVersion;
    ReleaseSRWLockExclusive(&registryLock);
}
struct GeometryStamp { unsigned long long surface, owner; };
bool PrepareGeometry(SurfaceProxy* proxy, HWND window, const RECT& full, const RECT& fitted,
                     unsigned long long ownerVersion, GeometryStamp& stamp, bool& needsFill) noexcept {
    AcquireSRWLockShared(&registryLock);
    stamp = {proxy->geometryVersion, proxy->owner->geometryVersion};
    const bool currentOwner = stamp.owner == ownerVersion;
    needsFill = !proxy->geometryValid || proxy->filledWindow != window || proxy->filledOwnerVersion != stamp.owner ||
                !SameRect(proxy->filledClient, full) || !SameRect(proxy->filledViewport, fitted);
    ReleaseSRWLockShared(&registryLock);
    return currentOwner;
}
bool RememberGeometry(SurfaceProxy* proxy, HWND window, const RECT& full, const RECT& fitted,
                       const GeometryStamp& stamp) noexcept {
    AcquireSRWLockExclusive(&registryLock);
    const bool unchanged = proxy->geometryVersion == stamp.surface && proxy->owner->geometryVersion == stamp.owner;
    if (unchanged) {
        proxy->filledWindow = window;
        proxy->filledClient = full;
        proxy->filledViewport = fitted;
        proxy->filledOwnerVersion = stamp.owner;
        proxy->geometryValid = true;
    }
    ReleaseSRWLockExclusive(&registryLock);
    return unchanged;
}

HRESULT STDMETHODCALLTYPE SurfaceGetClipper(IDirectDrawSurface7* self, LPDIRECTDRAWCLIPPER* output) {
    LastErrorState error;
    Pin<SurfaceProxy> proxy(FindProxy<SurfaceProxy>(self));
    if (!proxy.value) return E_UNEXPECTED;
    error.Restore();
    const HRESULT result = proxy->real->lpVtbl->GetClipper(proxy->real, output);
    error.Capture();
    LogResult(GetConfiguration().log, surfaceOperationBase + 15, result);
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
        LogResult(config.log, clipperGetWindowOperation, queried);
        if (SUCCEEDED(queried) && current == owner) {
            error.Restore();
            const HRESULT changed = clipper->lpVtbl->SetHWnd(clipper, 0, target);
            LogResult(config.log, clipperSetWindowOperation, changed);
            if (FAILED(changed)) {
                error.Capture();
                return changed;
            }
        }
    }
    // Retargeting/query/provider LastError must not leak to the actual SetClipper.
    error.Restore();
    const HRESULT result = proxy->real->lpVtbl->SetClipper(proxy->real, clipper);
    error.Capture();
    if (SUCCEEDED(result)) InvalidateGeometry(proxy.value);
    LogResult(config.log, surfaceOperationBase + 28, result);
    return result;
}

HRESULT CallBlt(SurfaceProxy* proxy, LPRECT destination, LPDIRECTDRAWSURFACE7 source,
                LPRECT sourceRect, DWORD flags, LPDDBLTFX effects, LastErrorState& error,
                ScalePresenterLog log, unsigned operation) noexcept {
    HRESULT result;
    {
        [[maybe_unused]] profile::Scope timing(profile::Metric::NativeBlt);
        error.Restore();
        result = proxy->real->lpVtbl->Blt(proxy->real, destination, source, sourceRect, flags, effects);
        error.Capture();
    }
    LogResult(log, operation, result);
    return result;
}

bool CallPainter(ScaleLetterboxPainter painter, HWND window, const RECT* strips, UINT count) noexcept {
    [[maybe_unused]] profile::Scope timing(profile::Metric::LetterboxPaint);
    return painter(window, strips, count);
}

struct PresentationScope {
    bool nested = insidePresentation;
    PresentationScope() noexcept { insidePresentation = true; }
    ~PresentationScope() { insidePresentation = nested; }
};

HRESULT STDMETHODCALLTYPE SurfaceBlt(IDirectDrawSurface7* self, LPRECT destination,
                                     LPDIRECTDRAWSURFACE7 source, LPRECT sourceRect,
                                     DWORD flags, LPDDBLTFX effects) {
    [[maybe_unused]] profile::Scope timing(profile::Metric::SurfaceBlt);
    LastErrorState error;
    PresentationSnapshot snapshot(self, source);
    auto* proxy = snapshot.destination.value;
    if (!proxy) return E_UNEXPECTED;
    auto* realSource = snapshot.source;
    const auto config = snapshot.config;
    PresentationScope scope;
    const auto forward = [&]() noexcept {
        InvalidateGeometry(proxy);
        return CallBlt(proxy, destination, realSource, sourceRect, flags, effects, error, config.log, surfaceOperationBase + 5);
    };
    constexpr DWORD admittedFlags = DDBLT_WAIT | DDBLT_DONOTWAIT | DDBLT_DDFX;
    if (scope.nested || !realSource || snapshot.sourceIsPrimary || sourceRect || (!config.provider && !config.admission) ||
        config.width <= 0 || config.height <= 0 ||
        !destination || static_cast<long long>(destination->right) - destination->left != config.width ||
        static_cast<long long>(destination->bottom) - destination->top != config.height ||
        (flags & ~admittedFlags) != 0) return forward();
    DDSURFACEDESC2 description{};
    description.dwSize = sizeof(description);
    HRESULT described;
    {
        [[maybe_unused]] profile::Scope descriptionTiming(profile::Metric::SurfaceDescription);
        error.Restore();
        described = realSource->lpVtbl->GetSurfaceDesc(realSource, &description);
    }
    LogResult(config.log, sourceDescriptionOperation, described);
    if (FAILED(described)) {
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
    constexpr DWORD requiredDescription = DDSD_WIDTH | DDSD_HEIGHT | DDSD_CAPS;
    constexpr DWORD admittedCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY |
        DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM;
    if (description.dwSize != sizeof(description) || (description.dwFlags & requiredDescription) != requiredDescription ||
        width != config.width || height != config.height || !(description.ddsCaps.dwCaps & DDSCAPS_OFFSCREENPLAIN) ||
        (description.ddsCaps.dwCaps & ~admittedCaps) || description.ddsCaps.dwCaps2 ||
        description.ddsCaps.dwCaps3 || description.ddsCaps.dwCaps4) return forward();
    const HWND window = snapshot.window;
    if (config.admission) {
        error.Restore();
        const HRESULT admitted = config.admission(window);
        if (FAILED(admitted)) InvalidateGeometry(proxy);
        LogResult(config.log, admissionOperation, admitted);
        if (FAILED(admitted)) return admitted; // Never turn a missing prerequisite into native full-frame presentation.
    }
    if (!config.provider) return forward();
    RECT full{};
    RECT fitted{};
    bool provided;
    {
        [[maybe_unused]] profile::Scope viewportTiming(profile::Metric::Viewport);
        error.Restore();
        provided = config.provider(window, width, height, &full, &fitted);
    }
    if (!provided) return forward();
    if (!window || !ValidViewport(full, fitted)) {
        LogOnce(config.log, badViewportLogged, L"Scaled presentation bypassed: invalid owner viewport");
        return forward();
    }
    const DWORD callerError = error.value;
    GeometryStamp geometry{};
    bool needsFill = false;
    if (!PrepareGeometry(proxy, window, full, fitted, snapshot.ownerVersion, geometry, needsFill)) return forward();
    if (needsFill) {
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
            painted = CallPainter(config.painter, window, strips, ARRAYSIZE(strips));
        DDBLTFX fill{};
        fill.dwSize = sizeof(fill);
        fill.dwFillColor = 0;
        for (RECT& strip : strips) {
            if (painted) break;
            if (!HasArea(strip)) continue;
            error.value = callerError;
            const HRESULT result = CallBlt(proxy, &strip, nullptr, nullptr,
                                           DDBLT_COLORFILL | DDBLT_WAIT, &fill, error, config.log, letterboxOperation);
            if (FAILED(result)) {
                if (config.painter && CallPainter(config.painter, window, strips, ARRAYSIZE(strips))) {
                    proxy->fallbackFill.store(true);
                    painted = true;
                    LogOnce(config.log, fallbackLogged,
                        L"Primary COLORFILL unavailable: owned-host GDI letterbox fallback active; presentation HRESULT remains real");
                    break;
                }
                InvalidateGeometry(proxy);
                return result;
            }
        }
    }
    error.value = callerError;
    const HRESULT result = CallBlt(proxy, &fitted, realSource, nullptr, flags, effects,
                                   error, config.log, presentationOperation);
    if (SUCCEEDED(result)) {
        if (RememberGeometry(proxy, window, full, fitted, geometry)) {
            if (config.commit) { error.Restore(); config.commit(window, width, height, &full, &fitted); }
            LogActive(config.log, width, height, fitted);
        }
    } else InvalidateGeometry(proxy);
    return result;
}

IDirectDraw7Vtbl* DrawTable() noexcept {
    static IDirectDraw7Vtbl table = []() {
        IDirectDraw7Vtbl value{};
#define DD_FORWARD(method) value.method = Forwarder<DrawProxy, decltype(value.method), &IDirectDraw7Vtbl::method, offsetof(IDirectDraw7Vtbl, method) / sizeof(void*)>::Call
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
        value.RestoreAllSurfaces = Forwarder<DrawProxy, decltype(value.RestoreAllSurfaces),
            &IDirectDraw7Vtbl::RestoreAllSurfaces, 25, InvalidateOwnerGeometry>::Call;
        return value;
    }();
    return &table;
}

IDirectDrawSurface7Vtbl* SurfaceTable() noexcept {
    static IDirectDrawSurface7Vtbl table = []() {
        IDirectDrawSurface7Vtbl value{};
#define SURFACE_FORWARD(method) value.method = Forwarder<SurfaceProxy, decltype(value.method), &IDirectDrawSurface7Vtbl::method, surfaceOperationBase + offsetof(IDirectDrawSurface7Vtbl, method) / sizeof(void*)>::Call
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
        value.Restore = Forwarder<SurfaceProxy, decltype(value.Restore), &IDirectDrawSurface7Vtbl::Restore,
            surfaceOperationBase + 27, InvalidateGeometry>::Call;
        value.ReleaseDC = Forwarder<SurfaceProxy, decltype(value.ReleaseDC), &IDirectDrawSurface7Vtbl::ReleaseDC,
            surfaceOperationBase + 26, InvalidateGeometry>::Call;
        return value;
    }();
    return &table;
}

} // namespace

void ConfigureScalePresenter(ScaleViewportProvider provider, ScalePresenterLog log,
                              ScaleClipWindowProvider clipWindow, ScaleLetterboxPainter painter,
                              LONG width, LONG height, ScalePresentationCommit commit,
                              ScalePresentationAdmission admission) noexcept {
    LastErrorState error;
    AcquireSRWLockExclusive(&registryLock);
    viewportProvider = provider;
    presenterLog = log;
    clipWindowProvider = clipWindow;
    letterboxPainter = painter;
    expectedWidth = width;
    expectedHeight = height;
    presentationCommit = commit;
    presentationAdmission = admission;
    ReleaseSRWLockExclusive(&registryLock);
}

namespace {
HRESULT TryWrapDirectDraw7(void** object) noexcept {
    if (!object) return E_POINTER;
    if (!*object) return E_UNEXPECTED;
    auto* real = static_cast<IDirectDraw7*>(*object);
    auto* replacement = real;
    if (AdoptExisting<DrawProxy>(real, &replacement)) {
        *object = replacement;
        return S_OK;
    }
    if (!AdmitAllocation()) return E_OUTOFMEMORY;
    auto* owner = new (std::nothrow) Owner;
    if (!owner) return E_OUTOFMEMORY;
    liveOwners.fetch_add(1, std::memory_order_relaxed);
    const HRESULT result = Adopt<DrawProxy>(real, owner, DrawTable(), &replacement);
    DropOwner(owner);
    if (SUCCEEDED(result)) *object = replacement;
    return result;
}
} // namespace

bool WrapDirectDraw7(void** object) noexcept {
    LastErrorState error;
    return SUCCEEDED(TryWrapDirectDraw7(object));
}

HRESULT CompleteScaledDirectDrawCreation(HRESULT nativeResult, void** object) noexcept {
    LastErrorState error;
    if (FAILED(nativeResult)) return nativeResult;
    const HRESULT wrapped = TryWrapDirectDraw7(object);
    if (FAILED(wrapped) && object && *object) {
        auto* native = static_cast<IDirectDraw7*>(*object);
        *object = nullptr;
        error.Restore(); native->lpVtbl->Release(native);
    }
    LogResult(GetConfiguration().log, wrapDrawOperation, wrapped);
    return FAILED(wrapped) ? wrapped : nativeResult;
}

void ConfigureScalePresenterResources(const ScalePresenterResources& resources) noexcept {
    LastErrorState error;
    AcquireSRWLockExclusive(&registryLock);
    drawCapacity = resources.drawCapacity < maxDrawObjects ? resources.drawCapacity : maxDrawObjects;
    primaryCapacity = resources.primaryCapacity < maxPrimarySurfaces ? resources.primaryCapacity : maxPrimarySurfaces;
    allocationBudget.store(resources.allocationBudget, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&registryLock);
}

ScalePresenterDiagnostics GetScalePresenterDiagnostics() noexcept {
    LastErrorState error;
    AcquireSRWLockShared(&diagnosticLock);
    const auto result = diagnostics;
    ReleaseSRWLockShared(&diagnosticLock);
    return result;
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
