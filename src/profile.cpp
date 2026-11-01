#include "profile.h"

#if defined(LAKE_ENABLE_PROFILING) && LAKE_ENABLE_PROFILING == 1

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>

namespace lakefix::profile {
namespace {

constexpr unsigned kUninitialized = 0;
constexpr unsigned kInitializing = 1;
constexpr unsigned kReady = 2;
constexpr unsigned kFailed = 3;

struct AtomicCounter {
    std::atomic<std::uint32_t> value{0};
    std::atomic<bool> saturated{false};
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "Profiling loss counters must not hide a blocking lock");
static_assert(std::atomic<unsigned>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
              "Profiling state must not hide a blocking lock");

// All storage is bounded and constant-initialized. The sample lock is entirely
// independent of the renderer, presenter registry, and diagnostic logger locks.
SRWLOCK gSamplesLock = SRWLOCK_INIT;
MetricSnapshot gMetrics[kMetricCount]{};
AtomicCounter gInvalid[kMetricCount]{};
AtomicCounter gDropped{};
AtomicCounter gIgnored{};
std::atomic<unsigned> gInitialization{kUninitialized};
std::uint64_t gFrequency = 0; // Published once by the release-store of kReady.
// Zig/Clang-MinGW diagnostic targets must use -fno-emulated-tls: native TLS
// keeps first-use emutls allocation out of this callback-recursion guard.
thread_local bool gReporting = false;

struct PreserveLastError {
    const DWORD saved = GetLastError();
    ~PreserveLastError() noexcept { SetLastError(saved); }
};

struct ReportingGuard {
    ReportingGuard() noexcept { gReporting = true; }
    ~ReportingGuard() noexcept { gReporting = false; }
};

void Increment(AtomicCounter& counter) noexcept {
    std::uint32_t current = counter.value.load(std::memory_order_relaxed);
    for (;;) {
        bool saturated = false;
        const std::uint32_t next = detail::SaturatingAdd(current, std::uint32_t{1}, saturated);
        if (saturated) {
            counter.saturated.store(true, std::memory_order_relaxed);
            return;
        }
        // Lock-free saturating bookkeeping, never wait for a profiler lock.
        if (counter.value.compare_exchange_strong(current, next, std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) return;
    }
}

void Record(Metric metric, bool valid, std::uint64_t ticks) noexcept {
    const unsigned index = static_cast<unsigned>(metric); // Scope already validated it.
    if (!valid) {
        // Bad clocks are classified even if another thread owns the sample lock.
        Increment(gInvalid[index]);
        return;
    }
    if (!TryAcquireSRWLockExclusive(&gSamplesLock)) {
        Increment(gDropped);
        return;
    }
    detail::AddSample(gMetrics[index], ticks);
    ReleaseSRWLockExclusive(&gSamplesLock);
}

constexpr const wchar_t* kMetricNames[] = {
    L"SurfaceBlt", L"PresenterSnapshot", L"SurfaceDescription", L"Viewport",
    L"NativeBlt", L"LetterboxPaint", L"QueueMapping", L"CursorPosition",
    L"CursorPaint", L"LogTotal", L"LogWrite"
};
static_assert(sizeof(kMetricNames) / sizeof(kMetricNames[0]) == kMetricCount,
              "Every profiling metric must have a report label");

// No CRT formatting, locale initialization, heap allocation, or string parsing.
// Even eight UINT64_MAX decimal counts plus labels fit comfortably in 576.
class RecordText {
public:
    void Reset() noexcept { size_ = 0; text_[0] = L'\0'; }

    void Append(const wchar_t* text) noexcept {
        while (*text && size_ < kCapacity - 1) AppendCharacter(*text++);
    }

    void AppendNumber(std::uint64_t value) noexcept {
        wchar_t digits[20]{};
        unsigned count = 0;
        do {
            digits[count++] = static_cast<wchar_t>(L'0' + value % 10);
            value /= 10;
        } while (value);
        while (count) AppendCharacter(digits[--count]);
    }

    void AppendReason(const wchar_t* reason) noexcept {
        if (!reason || !*reason) {
            Append(L"unknown");
            return;
        }
        constexpr unsigned limit = 80;
        unsigned index = 0;
        for (; index < limit && reason[index]; ++index) {
            const wchar_t ch = reason[index];
            AppendCharacter(ch >= L'!' && ch <= L'~' ? ch : L'_');
        }
        if (index == limit) Append(L"...");
    }

    const wchar_t* Text() const noexcept { return text_; }

private:
    static constexpr unsigned kCapacity = 576;
    static_assert(kCapacity < 600, "Profile records must fit the existing logger");
    wchar_t text_[kCapacity]{};
    unsigned size_ = 0;

    void AppendCharacter(wchar_t ch) noexcept {
        if (size_ < kCapacity - 1) {
            text_[size_++] = ch;
            text_[size_] = L'\0';
        }
    }
};

} // namespace

bool Initialize() noexcept {
    PreserveLastError preserve;
    unsigned state = gInitialization.load(std::memory_order_acquire);
    if (state != kUninitialized) return state == kReady;
    if (!gInitialization.compare_exchange_strong(state, kInitializing,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) return state == kReady;
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        gInitialization.store(kFailed, std::memory_order_release);
        return false;
    }
    gFrequency = static_cast<std::uint64_t>(frequency.QuadPart);
    gInitialization.store(kReady, std::memory_order_release);
    return true;
}

bool Read(Snapshot& output) noexcept {
    PreserveLastError preserve;
    if (gInitialization.load(std::memory_order_acquire) != kReady) return false;
    if (!TryAcquireSRWLockShared(&gSamplesLock)) return false;
    output.frequency = gFrequency;
    output.dropped = gDropped.value.load(std::memory_order_relaxed);
    output.ignored = gIgnored.value.load(std::memory_order_relaxed);
    output.countersSaturated = gDropped.saturated.load(std::memory_order_relaxed) ||
                               gIgnored.saturated.load(std::memory_order_relaxed);
    for (unsigned index = 0; index < kMetricCount; ++index) {
        output.metrics[index] = gMetrics[index];
        output.metrics[index].invalid = gInvalid[index].value.load(std::memory_order_relaxed);
        const bool invalidSaturated = gInvalid[index].saturated.load(std::memory_order_relaxed);
        output.metrics[index].saturated = output.metrics[index].saturated || invalidSaturated;
        output.countersSaturated = output.countersSaturated || invalidSaturated;
    }
    ReleaseSRWLockShared(&gSamplesLock);
    return true;
}

Scope::Scope(Metric metric) noexcept : metric_(metric) {
    PreserveLastError preserve;
    if (gReporting || gInitialization.load(std::memory_order_acquire) != kReady) return;
    if (!detail::IsMetric(metric)) {
        Increment(gIgnored);
        return;
    }
    active_ = true;
    LARGE_INTEGER start{};
    startValid_ = QueryPerformanceCounter(&start) != FALSE && start.QuadPart >= 0;
    startTicks_ = start.QuadPart;
}

Scope::~Scope() noexcept { Stop(); }

void Scope::Stop() noexcept {
    if (!active_) return;
    PreserveLastError preserve;
    active_ = false; // Including failed clocks: explicit Stop + destructor is once only.
    if (gReporting) return;
    LARGE_INTEGER end{};
    const bool endOk = startValid_ && QueryPerformanceCounter(&end) != FALSE;
    std::uint64_t ticks = 0;
    const bool valid = detail::ElapsedTicks(startValid_, startTicks_, endOk, end.QuadPart, ticks);
    Record(metric_, valid, ticks);
}

bool Report(void (*logger)(const wchar_t*) noexcept, const wchar_t* reason) noexcept {
    if (!logger) return false;
    PreserveLastError preserve;
    if (gReporting) return false;
    ReportingGuard reporting;
    Snapshot snapshot{};
    if (!Read(snapshot)) return false;
    // No profiler lock is held below this point, including during callbacks.
    RecordText text;
    text.Append(L"Profile diagnostic-only schema=1 reason=");
    text.AppendReason(reason);
    text.Append(L" frequency=");
    text.AppendNumber(snapshot.frequency);
    text.Append(L" dropped=");
    text.AppendNumber(snapshot.dropped);
    text.Append(L" ignored=");
    text.AppendNumber(snapshot.ignored);
    text.Append(L" countersSaturated=");
    text.AppendNumber(snapshot.countersSaturated ? 1 : 0);
    logger(text.Text());

    text.Reset();
    text.Append(L"Profile limitations=overlapping-durations,QPC-not-CPU,no-displayed-FPS "
                L"histogram=QPC-ticks bins=32 bin0=0..1 bins1..30=(2^(i-1),2^i] "
                L"bin31=(1073741824,UINT64_MAX] open-ended-overflow-last "
                L"loss-counters=individually-atomic");
    logger(text.Text());

    for (unsigned index = 0; index < kMetricCount; ++index) {
        const MetricSnapshot& metric = snapshot.metrics[index];
        text.Reset();
        text.Append(L"Profile metric=");
        text.Append(kMetricNames[index]);
        text.Append(L" count=");
        text.AppendNumber(metric.count);
        text.Append(L" totalTicks=");
        text.AppendNumber(metric.totalTicks);
        text.Append(L" minTicks=");
        text.AppendNumber(metric.minTicks);
        text.Append(L" maxTicks=");
        text.AppendNumber(metric.maxTicks);
        text.Append(L" saturated=");
        text.AppendNumber(metric.saturated ? 1 : 0);
        text.Append(L" invalid=");
        text.AppendNumber(metric.invalid);
        logger(text.Text());

        for (unsigned group = 0; group < kHistogramBins; group += 8) {
            text.Reset();
            text.Append(L"Profile histogram metric=");
            text.Append(kMetricNames[index]);
            text.Append(L" bins=");
            text.AppendNumber(group);
            text.Append(L"..");
            text.AppendNumber(group + 7);
            text.Append(L" counts=");
            for (unsigned bin = group; bin < group + 8; ++bin) {
                if (bin != group) text.Append(L",");
                text.AppendNumber(metric.histogram[bin]);
            }
            logger(text.Text());
        }
    }
    return true;
}

} // namespace lakefix::profile

#endif
