#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

namespace lakefix::profile {

enum class Metric : unsigned {
    SurfaceBlt,
    PresenterSnapshot,
    SurfaceDescription,
    Viewport,
    NativeBlt,
    LetterboxPaint,
    QueueMapping,
    CursorPosition,
    CursorPaint,
    LogTotal,
    LogWrite,
    Count
};

#if defined(LAKE_ENABLE_PROFILING) && LAKE_ENABLE_PROFILING == 1
inline constexpr bool kEnabled = true;
#else
inline constexpr bool kEnabled = false;
#endif

inline constexpr unsigned kMetricCount = static_cast<unsigned>(Metric::Count);
inline constexpr unsigned kHistogramBins = 32;

struct MetricSnapshot {
    std::uint64_t count = 0; // Accepted, valid clock samples only.
    std::uint64_t totalTicks = 0;
    std::uint64_t minTicks = 0; // Zero when count is zero; zero is also a valid duration.
    std::uint64_t maxTicks = 0;
    // Bin 0 contains [0,1]. Bins 1..30 contain (2^(i-1),2^i].
    // Bin 31 is open-ended: (2^30,UINT64_MAX], NOT a finite 2^31 ceiling.
    std::uint64_t histogram[kHistogramBins]{};
    std::uint64_t invalid = 0; // Failed/negative/backwards clocks, never zero samples.
    bool saturated = false; // Sticky: an addition lost information, not just reached MAX.
};

struct Snapshot {
    std::uint64_t frequency = 0; // QPC ticks per second, fixed by Initialize().
    MetricSnapshot metrics[kMetricCount]{};
    std::uint32_t dropped = 0; // Valid samples lost to the nonblocking sample lock.
    std::uint32_t ignored = 0; // Unknown Metric values; no QPC call is made for them.
    bool countersSaturated = false;
};

// Pure arithmetic is available in either build for deterministic boundary tests.
// None of these helpers reads a clock, touches globals, or calls Windows.
namespace detail {

inline constexpr bool IsMetric(Metric metric) noexcept {
    return static_cast<unsigned>(metric) < kMetricCount;
}

template<class UInt>
inline constexpr UInt SaturatingAdd(UInt value, UInt amount, bool& saturated) noexcept {
    static_assert(std::is_integral<UInt>::value && std::is_unsigned<UInt>::value,
                  "Profiling counters must be unsigned integers");
    constexpr UInt high = (std::numeric_limits<UInt>::max)();
    if (amount > high - value) {
        saturated = true;
        return high;
    }
    return static_cast<UInt>(value + amount);
}

inline constexpr unsigned HistogramBucket(std::uint64_t ticks) noexcept {
    unsigned bucket = 0;
    std::uint64_t ceiling = 1;
    while (bucket < kHistogramBins - 1 && ticks > ceiling) {
        ++bucket;
        ceiling <<= 1;
    }
    return bucket;
}

// Reject bad clocks without modifying the caller's duration. Widening before
// subtraction avoids signed overflow, including at INT64_MAX.
inline constexpr bool ElapsedTicks(bool startOk, std::int64_t start,
                                   bool endOk, std::int64_t end,
                                   std::uint64_t& ticks) noexcept {
    if (!startOk || !endOk || start < 0 || end < start) return false;
    ticks = static_cast<std::uint64_t>(end) - static_cast<std::uint64_t>(start);
    return true;
}

inline constexpr void AddSample(MetricSnapshot& metric, std::uint64_t ticks) noexcept {
    const bool first = metric.count == 0;
    metric.count = SaturatingAdd(metric.count, std::uint64_t{1}, metric.saturated);
    metric.totalTicks = SaturatingAdd(metric.totalTicks, ticks, metric.saturated);
    if (first || ticks < metric.minTicks) metric.minTicks = ticks;
    if (first || ticks > metric.maxTicks) metric.maxTicks = ticks;
    const unsigned bucket = HistogramBucket(ticks);
    metric.histogram[bucket] = SaturatingAdd(metric.histogram[bucket], std::uint64_t{1},
                                             metric.saturated);
}

inline constexpr void AddInvalid(MetricSnapshot& metric) noexcept {
    metric.invalid = SaturatingAdd(metric.invalid, std::uint64_t{1}, metric.saturated);
}

} // namespace detail

#if defined(LAKE_ENABLE_PROFILING) && LAKE_ENABLE_PROFILING == 1

// Explicit opt-in outside DllMain. No reset, allocation, worker, timer or I/O.
// The first attempt publishes a fixed frequency or a permanent failed state.
// A concurrent call while that attempt is in progress returns false, not waits.
bool Initialize() noexcept;

// Best effort: false leaves output unchanged. Accepted sample statistics are
// copied together under a separate try-shared SRW lock. Loss/invalid counters
// are individually atomic observations, not a cross-counter instant in time.
// Their backend storage saturates at UINT32_MAX; metric.invalid is widened here.
// After producers stop, accepted + invalid + dropped accounts for active,
// completed valid-Metric scopes not suppressed by reporting, unless a counter
// saturated. Pre-init/report-created scopes are inert; stopping an outer scope
// inside a report callback also suppresses it. All calls preserve LastError.
bool Read(Snapshot& output) noexcept;

// Synchronous diagnostic-only records, each <600 wchar_t including NUL. The
// logger runs after unlocking, may reenter Read(), and owns any eventual I/O.
// Recursive reports return false; scopes created during reporting are inert.
// Reason is opaque text: at most 80 characters, printable ASCII only (other
// characters become '_'), followed by '...' at the limit. Null/empty is unknown.
bool Report(void (*logger)(const wchar_t*) noexcept, const wchar_t* reason) noexcept;

class Scope {
public:
    explicit Scope(Metric metric) noexcept;
    ~Scope() noexcept;
    void Stop() noexcept;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

private:
    Metric metric_;
    std::int64_t startTicks_ = 0;
    bool startValid_ = false;
    bool active_ = false;
};

#else

// In ordinary builds even an unoptimized caller has no clock, Win32, lock,
// counter or callback dependency. In particular Read never touches its output.
inline bool Initialize() noexcept { return false; }
inline bool Read(Snapshot&) noexcept { return false; }
inline bool Report(void (*)(const wchar_t*) noexcept, const wchar_t*) noexcept { return false; }

class Scope {
public:
    explicit Scope(Metric) noexcept {}
    ~Scope() noexcept = default;
    void Stop() noexcept {}
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;
};

#endif

} // namespace lakefix::profile
