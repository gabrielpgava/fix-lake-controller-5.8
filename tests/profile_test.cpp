#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// Header first: the disabled API and pure helpers must not depend on Windows.
#include "../src/profile.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <limits>
#include <type_traits>
#include <utility>

// Standalone console tests. No windows, cursor changes, GPU, DirectDraw,
// injection, proprietary code, files, timers, sleeps, or exact-time assertions.
namespace {
namespace profile = lakefix::profile;
using profile::Metric;
using profile::MetricSnapshot;
using profile::Scope;
using profile::Snapshot;

int checks;
int failures;
unsigned countingCalls;
constexpr DWORD kLastError = 0x4c414b45u;
constexpr std::uint64_t kHigh64 = (std::numeric_limits<std::uint64_t>::max)();
constexpr std::uint32_t kHigh32 = (std::numeric_limits<std::uint32_t>::max)();

void Check(bool condition, const char* name) {
    ++checks;
    printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

bool SameMetric(const MetricSnapshot& a, const MetricSnapshot& b) noexcept {
    if (a.count != b.count || a.totalTicks != b.totalTicks || a.minTicks != b.minTicks ||
        a.maxTicks != b.maxTicks || a.invalid != b.invalid || a.saturated != b.saturated) return false;
    for (unsigned bin = 0; bin < profile::kHistogramBins; ++bin)
        if (a.histogram[bin] != b.histogram[bin]) return false;
    return true;
}

bool SameSnapshot(const Snapshot& a, const Snapshot& b) noexcept {
    if (a.frequency != b.frequency || a.dropped != b.dropped || a.ignored != b.ignored ||
        a.countersSaturated != b.countersSaturated) return false;
    for (unsigned index = 0; index < profile::kMetricCount; ++index)
        if (!SameMetric(a.metrics[index], b.metrics[index])) return false;
    return true;
}

std::uint64_t HistogramCount(const MetricSnapshot& metric) noexcept {
    std::uint64_t sum = 0;
    bool saturated = false;
    for (std::uint64_t count : metric.histogram)
        sum = profile::detail::SaturatingAdd(sum, count, saturated);
    return sum;
}

Snapshot SentinelSnapshot() noexcept {
    Snapshot output{};
    output.frequency = 1234567;
    output.dropped = 123;
    output.ignored = 456;
    output.countersSaturated = true;
    for (unsigned index = 0; index < profile::kMetricCount; ++index) {
        MetricSnapshot& metric = output.metrics[index];
        metric.count = 100 + index;
        metric.totalTicks = 200 + index;
        metric.minTicks = 300 + index;
        metric.maxTicks = 400 + index;
        metric.invalid = 500 + index;
        metric.saturated = true;
        for (unsigned bin = 0; bin < profile::kHistogramBins; ++bin)
            metric.histogram[bin] = 600 + bin + index;
    }
    return output;
}

void CountingLogger(const wchar_t*) noexcept {
    ++countingCalls;
    SetLastError(kLastError + 1);
}

void TestBuckets() {
    using profile::detail::HistogramBucket;
    Check(HistogramBucket(0) == 0 && HistogramBucket(1) == 0,
          "histogram bin zero includes both zero and one tick");
    for (unsigned bin = 0; bin < 31; ++bin) {
        const std::uint64_t ceiling = std::uint64_t{1} << bin;
        const std::uint64_t first = bin ? (ceiling >> 1) + 1 : 0;
        char name[128]{};
        std::snprintf(name, sizeof(name), "histogram bin %u has inclusive ceil-power-of-two boundaries", bin);
        Check(HistogramBucket(first) == bin && HistogramBucket(ceiling) == bin, name);
        std::snprintf(name, sizeof(name), "histogram bin %u joins the next bin without a gap", bin);
        Check(HistogramBucket(ceiling + 1) == bin + 1, name);
    }
    Check(HistogramBucket((std::uint64_t{1} << 30) + 1) == 31 &&
          HistogramBucket(std::uint64_t{1} << 31) == 31 && HistogramBucket(kHigh64) == 31,
          "last bin is open-ended, including every duration above 2^30");
    bool monotonic = true;
    unsigned previous = HistogramBucket(0);
    for (std::uint64_t ticks = 1; ticks <= 4096; ++ticks) {
        const unsigned current = HistogramBucket(ticks);
        monotonic = monotonic && current >= previous && current <= previous + 1;
        previous = current;
    }
    Check(monotonic, "small contiguous durations have no missing or reversed bins");
}

void TestSaturation() {
    using profile::detail::SaturatingAdd;
    bool saturated = false;
    Check(SaturatingAdd(kHigh64 - 1, std::uint64_t{1}, saturated) == kHigh64 && !saturated,
          "exact UINT64_MAX is representable, not an overflow");
    Check(SaturatingAdd(kHigh64, std::uint64_t{0}, saturated) == kHigh64 && !saturated,
          "adding zero at UINT64_MAX does not lose information");
    Check(SaturatingAdd(kHigh64, std::uint64_t{1}, saturated) == kHigh64 && saturated,
          "UINT64_MAX increment saturates and reports overflow");
    Check(SaturatingAdd(std::uint64_t{3}, std::uint64_t{4}, saturated) == 7 && saturated,
          "overflow flag stays sticky across later additions");
    saturated = false;
    Check(SaturatingAdd(kHigh64 - 2, std::uint64_t{10}, saturated) == kHigh64 && saturated,
          "large total addition saturates before unsigned wrap");
    saturated = false;
    Check(SaturatingAdd(kHigh32 - 1, std::uint32_t{1}, saturated) == kHigh32 && !saturated,
          "32-bit loss bookkeeping can reach its exact maximum");
    Check(SaturatingAdd(kHigh32, std::uint32_t{1}, saturated) == kHigh32 && saturated,
          "32-bit loss bookkeeping saturates rather than wraps");
}

void TestMetricArithmetic() {
    using profile::detail::AddInvalid;
    using profile::detail::AddSample;
    MetricSnapshot metric{};
    for (std::uint64_t ticks : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2},
                                std::uint64_t{3}, std::uint64_t{17}}) AddSample(metric, ticks);
    Check(metric.count == 5 && metric.totalTicks == 23 && metric.minTicks == 0 && metric.maxTicks == 17,
          "pure aggregation keeps count, total, minimum and maximum, including a valid zero");
    Check(metric.histogram[0] == 2 && metric.histogram[1] == 1 && metric.histogram[2] == 1 &&
          metric.histogram[5] == 1 && HistogramCount(metric) == metric.count && !metric.saturated,
          "pure aggregation adds each valid duration to exactly one bucket");
    MetricSnapshot expected = metric;
    expected.invalid = 1;
    AddInvalid(metric);
    Check(SameMetric(metric, expected), "an invalid clock changes no valid-duration statistics");

    MetricSnapshot invalidOnly{};
    AddInvalid(invalidOnly);
    Check(invalidOnly.invalid == 1 && invalidOnly.count == 0 && invalidOnly.totalTicks == 0 &&
          invalidOnly.minTicks == 0 && invalidOnly.maxTicks == 0 && HistogramCount(invalidOnly) == 0,
          "invalid-only metrics do not invent a zero-duration sample");

    MetricSnapshot total{};
    AddSample(total, kHigh64 - 3);
    AddSample(total, 10);
    Check(total.count == 2 && total.totalTicks == kHigh64 && total.minTicks == 10 &&
          total.maxTicks == kHigh64 - 3 && total.saturated && HistogramCount(total) == 2,
          "total ticks saturate independently while extrema and histogram stay meaningful");

    MetricSnapshot count{};
    count.count = kHigh64;
    AddSample(count, 1);
    Check(count.count == kHigh64 && count.totalTicks == 1 && count.histogram[0] == 1 && count.saturated,
          "sample count overflow saturates without skipping other arithmetic");

    MetricSnapshot histogram{};
    histogram.count = 1;
    histogram.histogram[0] = kHigh64;
    AddSample(histogram, 0);
    Check(histogram.count == 2 && histogram.histogram[0] == kHigh64 && histogram.saturated,
          "each histogram counter independently saturates");

    MetricSnapshot invalid{};
    invalid.invalid = kHigh64;
    AddInvalid(invalid);
    Check(invalid.invalid == kHigh64 && invalid.count == 0 && invalid.saturated,
          "pure invalid counter also saturates without creating samples");

    MetricSnapshot maximum{};
    AddSample(maximum, kHigh64);
    AddSample(maximum, 0);
    Check(maximum.count == 2 && maximum.totalTicks == kHigh64 && maximum.minTicks == 0 &&
          maximum.maxTicks == kHigh64 && maximum.histogram[31] == 1 && !maximum.saturated,
          "exact maximum duration plus a zero is representable without false overflow");
}

void TestClocksAndMetricValidation() {
    using profile::detail::ElapsedTicks;
    std::uint64_t ticks = 777;
    Check(ElapsedTicks(true, 10, true, 14, ticks) && ticks == 4,
          "pure clock subtraction produces elapsed ticks");
    Check(ElapsedTicks(true, 10, true, 10, ticks) && ticks == 0,
          "equal valid timestamps are a real zero-duration sample");
    const std::int64_t high = (std::numeric_limits<std::int64_t>::max)();
    Check(ElapsedTicks(true, 0, true, high, ticks) && ticks == static_cast<std::uint64_t>(high),
          "clock subtraction handles INT64_MAX without signed overflow");
    struct InvalidClock {
        bool startOk;
        std::int64_t start;
        bool endOk;
        std::int64_t end;
        const char* name;
    };
    const InvalidClock invalid[] = {
        {false, 10, true, 14, "failed start clock discards the sample"},
        {true, 10, false, 14, "failed end clock discards the sample"},
        {false, 0, false, 0, "two failed clocks do not invent zero ticks"},
        {true, 14, true, 10, "backwards clocks discard the sample"},
        {true, -1, true, 10, "negative start clock is rejected"},
        {true, 10, true, -1, "negative end clock is rejected"},
        {true, (std::numeric_limits<std::int64_t>::min)(), true, high,
         "full signed clock span is rejected before subtraction"}
    };
    for (const InvalidClock& clock : invalid) {
        ticks = 777;
        MetricSnapshot result{};
        const bool valid = ElapsedTicks(clock.startOk, clock.start, clock.endOk, clock.end, ticks);
        if (valid) profile::detail::AddSample(result, ticks);
        else profile::detail::AddInvalid(result);
        Check(!valid && ticks == 777 && result.invalid == 1 && result.count == 0 &&
              HistogramCount(result) == 0, clock.name);
    }
    bool allMetrics = true;
    for (unsigned index = 0; index < profile::kMetricCount; ++index)
        allMetrics = allMetrics && profile::detail::IsMetric(static_cast<Metric>(index));
    Check(allMetrics && !profile::detail::IsMetric(Metric::Count) &&
          !profile::detail::IsMetric(static_cast<Metric>((std::numeric_limits<unsigned>::max)())),
          "metric validation admits all known values and rejects sentinels and out-of-range values");
}

#if defined(LAKE_ENABLE_PROFILING) && LAKE_ENABLE_PROFILING == 1

Snapshot ReadRequired(const char* name) {
    Snapshot result{};
    Check(profile::Read(result), name);
    return result;
}

bool MetricConsistent(const MetricSnapshot& metric) noexcept {
    if (metric.saturated || HistogramCount(metric) != metric.count) return false;
    if (!metric.count) return metric.totalTicks == 0 && metric.minTicks == 0 && metric.maxTicks == 0;
    return metric.minTicks <= metric.maxTicks && metric.totalTicks >= metric.maxTicks;
}

void TestBeforeInitialize() {
    Snapshot output = SentinelSnapshot();
    unsigned char saved[sizeof(output)]{};
    std::memcpy(saved, &output, sizeof(output));
    SetLastError(kLastError);
    const bool read = profile::Read(output);
    const DWORD error = GetLastError();
    Check(!read && std::memcmp(saved, &output, sizeof(output)) == 0 && error == kLastError,
          "before explicit initialization Read fails without changing output or LastError");
    SetLastError(kLastError);
    {
        Scope sample(Metric::SurfaceBlt);
        sample.Stop();
        sample.Stop();
    }
    Check(GetLastError() == kLastError,
          "scopes before explicit initialization are inert and preserve LastError");
    SetLastError(kLastError);
    const bool reported = profile::Report(CountingLogger, L"before-init");
    Check(!reported && !countingCalls && GetLastError() == kLastError,
          "before initialization Report never calls its logger");
}

bool TestInitialize() {
    SetLastError(kLastError);
    const bool initialized = profile::Initialize();
    const DWORD error = GetLastError();
    Check(initialized && error == kLastError, "explicit QPC initialization succeeds and preserves LastError");
    if (!initialized) return false;
    Snapshot initial{};
    SetLastError(kLastError);
    const bool read = profile::Read(initial);
    const DWORD readError = GetLastError();
    Check(read && initial.frequency > 0 && readError == kLastError,
          "initialized Read exposes a positive QPC frequency and preserves LastError");
    Snapshot expected{};
    expected.frequency = initial.frequency;
    Check(SameSnapshot(initial, expected), "initialization starts empty and pre-init scopes did not record");
    return read;
}

void TestScopes() {
    const unsigned index = static_cast<unsigned>(Metric::SurfaceBlt);
    const Snapshot before = ReadRequired("scope baseline snapshot is readable");
    Snapshot once{};
    {
        SetLastError(kLastError);
        Scope sample(Metric::SurfaceBlt);
        Check(GetLastError() == kLastError, "scope constructor preserves caller LastError");
        SetLastError(kLastError + 2);
        sample.Stop();
        Check(GetLastError() == kLastError + 2, "scope Stop preserves the current LastError");
        once = ReadRequired("stopped scope snapshot is readable");
        SetLastError(kLastError + 3);
        sample.Stop();
        Check(GetLastError() == kLastError + 3, "second Stop is inert and preserves LastError");
        const Snapshot twice = ReadRequired("double-stopped scope snapshot is readable");
        Check(SameSnapshot(once, twice), "calling Stop twice does not record twice");
        SetLastError(kLastError + 4);
    }
    Check(GetLastError() == kLastError + 4, "destruction after Stop preserves LastError");
    const Snapshot destroyed = ReadRequired("destroyed scope snapshot is readable");
    Check(SameSnapshot(once, destroyed), "destructor does not record an explicitly stopped scope again");
    const std::uint64_t accounted = once.metrics[index].count - before.metrics[index].count +
        once.metrics[index].invalid - before.metrics[index].invalid + once.dropped - before.dropped;
    Check(accounted == 1 && MetricConsistent(once.metrics[index]),
          "one scope is accounted exactly once without assuming its clock duration");

    const unsigned viewport = static_cast<unsigned>(Metric::Viewport);
    SetLastError(kLastError);
    { Scope sample(Metric::Viewport); }
    Check(GetLastError() == kLastError, "active scope destructor preserves LastError");
    const Snapshot automatic = ReadRequired("automatic-stop snapshot is readable");
    Check(automatic.metrics[viewport].count + automatic.metrics[viewport].invalid == 1 &&
          MetricConsistent(automatic.metrics[viewport]), "ordinary destruction records one sample");

    SetLastError(kLastError);
    {
        Scope sentinel(Metric::Count);
        Scope unknown(static_cast<Metric>((std::numeric_limits<unsigned>::max)()));
        sentinel.Stop();
        unknown.Stop();
    }
    Check(GetLastError() == kLastError, "unknown metrics preserve LastError");
    const Snapshot ignored = ReadRequired("ignored-metric snapshot is readable");
    Snapshot expected = automatic;
    expected.ignored += 2;
    Check(SameSnapshot(ignored, expected), "unknown metrics are safely ignored and counted, never sampled");

    SetLastError(kLastError);
    const bool again = profile::Initialize();
    const DWORD error = GetLastError();
    const Snapshot repeated = ReadRequired("repeat-initialization snapshot is readable");
    Check(again && error == kLastError && SameSnapshot(ignored, repeated),
          "Initialize is idempotent and never resets existing samples or counters");
}

struct WorkerContext {
    unsigned completed = 0;
    bool lastErrorPreserved = true;
};

constexpr unsigned kWorkerCount = 4;
constexpr unsigned kWorkerIterations = 4096;
WorkerContext workerContexts[kWorkerCount]{};

DWORD WINAPI RecordWorker(void* parameter) noexcept {
    auto& context = *static_cast<WorkerContext*>(parameter);
    for (unsigned iteration = 0; iteration < kWorkerIterations; ++iteration) {
        SetLastError(kLastError);
        {
            Scope sample(Metric::SurfaceBlt);
            context.lastErrorPreserved = context.lastErrorPreserved && GetLastError() == kLastError;
            sample.Stop();
            context.lastErrorPreserved = context.lastErrorPreserved && GetLastError() == kLastError;
            sample.Stop();
        }
        context.lastErrorPreserved = context.lastErrorPreserved && GetLastError() == kLastError;
        ++context.completed;
    }
    return 0;
}

bool TestConcurrentRecords() {
    const Snapshot before = ReadRequired("concurrent-record baseline snapshot is readable");
    HANDLE threads[kWorkerCount]{};
    unsigned created = 0;
    for (; created < kWorkerCount; ++created) {
        threads[created] = CreateThread(nullptr, 0, RecordWorker, &workerContexts[created], 0, nullptr);
        if (!threads[created]) break;
    }
    Check(created == kWorkerCount, "four test threads can record concurrently without renderer resources");

    Snapshot probe = SentinelSnapshot();
    unsigned char saved[sizeof(probe)]{};
    std::memcpy(saved, &probe, sizeof(probe));
    SetLastError(kLastError);
    const bool read = profile::Read(probe); // One attempt, not polling or waiting for success.
    const DWORD readError = GetLastError();
    Check(readError == kLastError && (read ? probe.frequency == before.frequency :
          std::memcmp(saved, &probe, sizeof(probe)) == 0),
          "a concurrent try-read either succeeds or leaves the entire output unchanged");

    bool joined = true;
    bool closed = true;
    for (unsigned worker = 0; worker < created; ++worker) {
        joined = (WaitForSingleObject(threads[worker], INFINITE) == WAIT_OBJECT_0) && joined;
        closed = (CloseHandle(threads[worker]) != FALSE) && closed;
    }
    Check(joined && closed, "all owned test threads finish and their handles are closed");
    if (!joined) return false; // Contexts have static lifetime even on an unexpected wait failure.

    std::uint64_t attempted = 0;
    bool errorsPreserved = true;
    for (unsigned worker = 0; worker < created; ++worker) {
        attempted += workerContexts[worker].completed;
        errorsPreserved = errorsPreserved && workerContexts[worker].lastErrorPreserved;
    }
    Check(attempted == static_cast<std::uint64_t>(created) * kWorkerIterations && errorsPreserved,
          "every worker completes all scopes with its own LastError intact");
    const Snapshot after = ReadRequired("quiescent concurrent-record snapshot is readable");
    const unsigned index = static_cast<unsigned>(Metric::SurfaceBlt);
    const std::uint64_t accepted = after.metrics[index].count - before.metrics[index].count;
    const std::uint64_t invalid = after.metrics[index].invalid - before.metrics[index].invalid;
    const std::uint64_t dropped = after.dropped - before.dropped;
    Check(accepted + invalid + dropped == attempted && !after.countersSaturated,
          "after joining, accepted + invalid + contention drops exactly accounts for every attempt");
    Check(MetricConsistent(after.metrics[index]) && after.ignored == before.ignored &&
          HistogramCount(after.metrics[index]) - HistogramCount(before.metrics[index]) == accepted,
          "concurrent accepted samples have coherent totals, extrema and histogram counts");
    return true;
}

constexpr const wchar_t* kExpectedMetricNames[profile::kMetricCount] = {
    L"SurfaceBlt", L"PresenterSnapshot", L"SurfaceDescription", L"Viewport",
    L"NativeBlt", L"LetterboxPaint", L"QueueMapping", L"CursorPosition",
    L"CursorPaint", L"LogTotal", L"LogWrite"
};
constexpr const wchar_t* kExpectedGroups[] = {
    L" bins=0..7 counts=", L" bins=8..15 counts=", L" bins=16..23 counts=", L" bins=24..31 counts="
};
constexpr unsigned kRecordsPerMetric = 1 + profile::kHistogramBins / 8;
constexpr unsigned kExpectedRecords = 2 + profile::kMetricCount * kRecordsPerMetric;

struct ReportCapture {
    Snapshot baseline{};
    wchar_t header[600]{};
    unsigned records = 0;
    unsigned maxLength = 0;
    bool bounded = true;
    bool labels = true;
    bool summaryFields = true;
    bool histogramGroups = true;
    bool limitations = false;
    bool reentrantRead = true;
    bool recursiveRejected = true;
};
ReportCapture capture{};

bool StartsWith(const wchar_t* text, const wchar_t* prefix) noexcept {
    return std::wcsncmp(text, prefix, std::wcslen(prefix)) == 0;
}

bool EightDecimalCounts(const wchar_t* text) noexcept {
    const wchar_t* counts = std::wcsstr(text, L" counts=");
    if (!counts) return false;
    counts += 8;
    unsigned commas = 0;
    unsigned digits = 0;
    for (; *counts; ++counts) {
        if (*counts == L',') {
            if (!digits) return false;
            ++commas;
            digits = 0;
        } else if (*counts >= L'0' && *counts <= L'9') {
            ++digits;
        } else {
            return false;
        }
    }
    return commas == 7 && digits > 0;
}

void CaptureLogger(const wchar_t* text) noexcept {
    unsigned length = 0;
    if (!text) {
        capture.bounded = false;
        return;
    }
    for (; length < 600 && text[length]; ++length)
        if (text[length] == L'\n' || text[length] == L'\r') capture.bounded = false;
    if (length + 1 >= 600) {
        capture.bounded = false;
        return;
    }
    if (length > capture.maxLength) capture.maxLength = length;
    if (capture.records == 0) {
        std::memcpy(capture.header, text, (length + 1) * sizeof(wchar_t));
        capture.labels = capture.labels && StartsWith(text, L"Profile diagnostic-only schema=1 reason=") &&
            std::wcsstr(text, L" frequency=") && std::wcsstr(text, L" dropped=") &&
            std::wcsstr(text, L" ignored=") && std::wcsstr(text, L" countersSaturated=");
    } else if (capture.records == 1) {
        capture.limitations = std::wcsstr(text, L"overlapping-durations") &&
            std::wcsstr(text, L"QPC-not-CPU") && std::wcsstr(text, L"no-displayed-FPS") &&
            std::wcsstr(text, L"histogram=QPC-ticks") &&
            std::wcsstr(text, L"bin31=(1073741824,UINT64_MAX]") &&
            std::wcsstr(text, L"open-ended-overflow-last") &&
            std::wcsstr(text, L"loss-counters=individually-atomic");
    } else {
        const unsigned record = capture.records - 2;
        const unsigned metric = record / kRecordsPerMetric;
        const unsigned part = record % kRecordsPerMetric;
        const wchar_t* label = std::wcsstr(text, L"metric=");
        if (metric >= profile::kMetricCount || !label) {
            capture.labels = false;
        } else {
            label += 7;
            const std::size_t expectedLength = std::wcslen(kExpectedMetricNames[metric]);
            capture.labels = capture.labels &&
                std::wcsncmp(label, kExpectedMetricNames[metric], expectedLength) == 0 &&
                label[expectedLength] == L' ';
        }
        if (part == 0) {
            capture.summaryFields = capture.summaryFields && StartsWith(text, L"Profile metric=") &&
                std::wcsstr(text, L" count=") && std::wcsstr(text, L" totalTicks=") &&
                std::wcsstr(text, L" minTicks=") && std::wcsstr(text, L" maxTicks=") &&
                std::wcsstr(text, L" saturated=") && std::wcsstr(text, L" invalid=");
        } else {
            capture.histogramGroups = capture.histogramGroups &&
                StartsWith(text, L"Profile histogram metric=") &&
                std::wcsstr(text, kExpectedGroups[part - 1]) && EightDecimalCounts(text);
        }
    }
    ++capture.records;

    const DWORD error = GetLastError();
    Snapshot current{};
    const bool read = profile::Read(current);
    capture.reentrantRead = capture.reentrantRead && read && GetLastError() == error &&
        SameSnapshot(current, capture.baseline);
    {
        Scope loggerScope(Metric::LogTotal);
        Scope writeScope(Metric::LogWrite);
        writeScope.Stop();
        loggerScope.Stop();
    }
    capture.recursiveRejected = capture.recursiveRejected &&
        !profile::Report(CountingLogger, L"recursive-logger");
    SetLastError(kLastError + 10); // Outer Report must also preserve the caller's error.
}

void RunCapturedReport(const wchar_t* reason) {
    capture = ReportCapture{};
    capture.baseline = ReadRequired("report baseline snapshot is readable");
    const unsigned beforeCounting = countingCalls;
    SetLastError(kLastError);
    const bool reported = profile::Report(CaptureLogger, reason);
    const DWORD error = GetLastError();
    Check(reported && error == kLastError, "Report succeeds and restores LastError even after logger changes it");
    Check(capture.records == kExpectedRecords && capture.bounded && capture.maxLength + 1 < 600,
          "every report is a bounded header, limitations, and five fixed records per metric");
    Check(capture.labels && capture.summaryFields && capture.histogramGroups && capture.limitations,
          "report labels, all metric fields, eight-bin groups and diagnostic limitations are explicit");
    Check(capture.reentrantRead && capture.recursiveRejected && countingCalls == beforeCounting,
          "logger can reenter Read without a held lock and recursive Report is rejected");
    const Snapshot after = ReadRequired("post-report snapshot is readable");
    Check(SameSnapshot(capture.baseline, after), "logger scopes and the report itself do not profile their own overhead");
}

void TestReports() {
    SetLastError(kLastError);
    Check(!profile::Report(nullptr, L"null-logger") && GetLastError() == kLastError,
          "null logger is safely rejected without changing LastError");
    RunCapturedReport(nullptr);
    Check(std::wcsstr(capture.header, L"reason=unknown ") != nullptr, "null report reason has the bounded unknown label");
    RunCapturedReport(L"");
    Check(std::wcsstr(capture.header, L"reason=unknown ") != nullptr, "empty report reason has the unknown label");
    RunCapturedReport(L"unrecognized_%s");
    Check(std::wcsstr(capture.header, L"reason=unrecognized_%s ") != nullptr,
          "unknown reason text is opaque, never interpreted as a formatting instruction");
    wchar_t reason[200]{};
    for (unsigned index = 0; index < 199; ++index) reason[index] = L'R';
    reason[0] = L'L';
    reason[4] = L'\n';
    reason[5] = L'\t';
    reason[6] = L'%';
    reason[7] = L'\u00e9';
    reason[180] = L'Z';
    RunCapturedReport(reason);
    Check(std::wcsstr(capture.header, L"reason=LRRR__%_") &&
          std::wcsstr(capture.header, L"... frequency=") && !std::wcsstr(capture.header, L"Z"),
          "long reasons are sanitized to one line and truncated without reading an unbounded string");
}

#else

void TestDisabled() {
    const Snapshot original = SentinelSnapshot();
    Snapshot output = original;
    unsigned char saved[sizeof(output)]{};
    std::memcpy(saved, &output, sizeof(output));
    SetLastError(kLastError);
    const bool initialized = profile::Initialize();
    Check(!initialized && GetLastError() == kLastError, "disabled Initialize is false and preserves LastError");
    SetLastError(kLastError);
    const bool read = profile::Read(output);
    Check(!read && GetLastError() == kLastError && SameSnapshot(output, original) &&
          std::memcmp(saved, &output, sizeof(output)) == 0,
          "disabled Read is false and leaves every output byte unchanged");
    SetLastError(kLastError);
    {
        Scope sample(Metric::SurfaceBlt);
        Scope unknown(Metric::Count);
        sample.Stop();
        sample.Stop();
        unknown.Stop();
    }
    Check(GetLastError() == kLastError, "disabled constructors, repeated Stop and destructors are inert");
    SetLastError(kLastError);
    const bool reported = profile::Report(CountingLogger, L"disabled_%s\n");
    const bool nullReason = profile::Report(CountingLogger, nullptr);
    const bool nullLogger = profile::Report(nullptr, L"disabled");
    Check(!reported && !nullReason && !nullLogger && !countingCalls && GetLastError() == kLastError,
          "disabled Report never calls a callback, even with null logger or reason");
    Check(!profile::Read(output) && std::memcmp(saved, &output, sizeof(output)) == 0,
          "all disabled operations leave subsequent Read output unchanged");
}

#endif

} // namespace

static_assert(std::is_standard_layout<Snapshot>::value && std::is_trivially_copyable<Snapshot>::value,
              "Snapshots must be plain, bounded, pointer-free value data");
static_assert(!std::is_copy_constructible<Scope>::value && !std::is_copy_assignable<Scope>::value &&
              !std::is_move_constructible<Scope>::value && !std::is_move_assignable<Scope>::value,
              "Scope ownership cannot be duplicated");
static_assert(std::is_nothrow_constructible<Scope, Metric>::value &&
              std::is_nothrow_destructible<Scope>::value, "Scope lifetime must be noexcept");
static_assert(noexcept(profile::Initialize()) && noexcept(profile::Read(std::declval<Snapshot&>())) &&
              noexcept(profile::Report(nullptr, nullptr)) &&
              noexcept(std::declval<Scope&>().Stop()), "Public profiling API must be noexcept");
static_assert(profile::detail::HistogramBucket(3) == 2 &&
              profile::detail::HistogramBucket(kHigh64) == 31, "Histogram helpers must remain constexpr");
#if defined(LAKE_ENABLE_PROFILING) && LAKE_ENABLE_PROFILING == 1
static_assert(profile::kEnabled, "Diagnostic test target must enable profiling");
#else
static_assert(!profile::kEnabled && std::is_empty<Scope>::value,
              "Ordinary builds must keep an empty, disabled scope");
#endif

int wmain() {
    TestBuckets();
    TestSaturation();
    TestMetricArithmetic();
    TestClocksAndMetricValidation();
#if defined(LAKE_ENABLE_PROFILING) && LAKE_ENABLE_PROFILING == 1
    TestBeforeInitialize();
    if (TestInitialize()) {
        TestScopes();
        if (TestConcurrentRecords()) TestReports();
    }
#else
    TestDisabled();
#endif
    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
