#include "session_diagnostics.h"

#include <bcrypt.h>
#include <atomic>
#include <type_traits>

namespace lakefix::diagnostics {
namespace {

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "Producer bookkeeping must not hide a blocking lock");

enum class Kind : std::uint32_t { Text, Exception };
struct TextEvent {
    unsigned length;
    bool truncated;
    wchar_t text[kTextCapacity];
};
struct ExceptionEvent { DWORD code; std::uintptr_t address; };
struct Event {
    std::uint64_t sequence;
    std::uint64_t tick;
    DWORD threadId;
    Kind kind;
    union { TextEvent text; ExceptionEvent exception; };
};
static_assert(std::is_trivial<Event>::value && std::is_standard_layout<Event>::value,
              "The preallocated ring, including VEH events, contains POD only");

struct Counts {
    std::atomic<std::uint32_t> rejectedInactive{0}, rejectedInvalid{0};
    std::atomic<std::uint32_t> droppedBusy{0}, droppedFull{0}, truncated{0};
    std::atomic<std::uint32_t> writtenRecords{0}, droppedFileLimit{0}, droppedIo{0};
    std::atomic<std::uint32_t> metadataWritten{0}, metadataDropped{0}, writeCalls{0};
    std::atomic<std::uint32_t> workerWakeups{0}, statsCalls{0};
};

// Constant-initialized, process-lifetime storage. No TLS, heap, static destructor,
// renderer dependency, or initialization on a producer's first use.
SRWLOCK gQueueLock = SRWLOCK_INIT;
SRWLOCK gFlushLock = SRWLOCK_INIT; // Only Flush callers use this, never the worker.
Event gQueue[kQueueCapacity]{};
unsigned gHead = 0, gTail = 0, gDepth = 0, gHighWater = 0;
std::uint64_t gEnqueued = 0, gCompleted = 0;
bool gInFlight = false;
Counts gCounts{};
std::atomic<std::uint32_t> gState{static_cast<std::uint32_t>(State::NotStarted)};
std::atomic<std::uint32_t> gStartError{0}, gWorkerThread{0}, gBytes{0}, gIoError{0};
std::atomic<std::uint32_t> gWrapped{0}, gLimit{0}, gIoFailed{0}, gHeaderComplete{0};
#if defined(LAKE_DIAGNOSTICS_TESTING)
std::atomic<std::uint32_t> gReportRequested{0};
#endif
SessionInfo gSession{}; // Immutable once Running is published.
Options gOptions{}; // Copied by Start; buildId points at gSession, not caller storage.
HANDLE gWake = nullptr, gProgress = nullptr, gExited = nullptr;
HANDLE gFile = INVALID_HANDLE_VALUE; // Duplicate, closed only by worker or failed Start.
using Sink = bool (*)(void*, const char*, DWORD, DWORD&, DWORD&) noexcept;
Sink gSink = nullptr;
void* gSinkContext = nullptr;

struct PreserveLastError {
    const DWORD saved = GetLastError();
    ~PreserveLastError() noexcept { SetLastError(saved); }
};

State LoadState() noexcept {
    return static_cast<State>(gState.load(std::memory_order_acquire));
}

void Increment(std::atomic<std::uint32_t>& counter) noexcept {
    // Exactly one RMW, no saturating-CAS retry loop in producers.
    if (counter.fetch_add(1, std::memory_order_relaxed) == UINT32_MAX)
        gWrapped.store(1, std::memory_order_relaxed);
}

// Returns with the queue lock held on success. Recheck admission under the lock
// so a producer racing Shutdown cannot publish after the worker sees an empty ring.
Event* Reserve() noexcept {
    if (LoadState() != State::Running) {
        Increment(gCounts.rejectedInactive);
        return nullptr;
    }
    if (!TryAcquireSRWLockExclusive(&gQueueLock)) {
        Increment(gCounts.droppedBusy);
        return nullptr;
    }
    if (LoadState() != State::Running) {
        ReleaseSRWLockExclusive(&gQueueLock);
        Increment(gCounts.rejectedInactive);
        return nullptr;
    }
    if (gDepth == kQueueCapacity) {
        ReleaseSRWLockExclusive(&gQueueLock);
        Increment(gCounts.droppedFull);
        return nullptr;
    }
    Event& event = gQueue[gTail];
    event.tick = GetTickCount64();
    event.threadId = GetCurrentThreadId();
    return &event;
}

void Commit(Event& event) noexcept {
    const bool wake = gDepth == 0;
    event.sequence = ++gEnqueued;
    gTail = (gTail + 1) % kQueueCapacity;
    if (++gDepth > gHighWater) gHighWater = gDepth;
    ReleaseSRWLockExclusive(&gQueueLock);
    // Events are never closed after a successful Start: a delayed producer may
    // still signal after Shutdown. SetEvent is notification, not an I/O wait.
    if (wake) SetEvent(gWake);
}

bool Pop(Event& event) noexcept {
    AcquireSRWLockExclusive(&gQueueLock); // Only the background worker may wait.
    const bool found = gDepth != 0;
    if (found) {
        event = gQueue[gHead];
        gHead = (gHead + 1) % kQueueCapacity;
        --gDepth;
        gInFlight = true;
    }
    ReleaseSRWLockExclusive(&gQueueLock);
    return found;
}

void Complete(const Event& event) noexcept {
    AcquireSRWLockExclusive(&gQueueLock);
    gCompleted = event.sequence;
    gInFlight = false;
    ReleaseSRWLockExclusive(&gQueueLock);
    SetEvent(gProgress);
}

// Formatting is consumer-only. Fixed buffers also make callback/failure behavior
// independent of CRT locale, printf allocation, and debugger notifications.
class Text {
public:
    void Character(wchar_t ch) noexcept {
        if (length_ + 1 < kCapacity) text_[length_++] = ch;
    }
    void Append(const wchar_t* text) noexcept { while (*text) Character(*text++); }
    void Ascii(const char* text) noexcept {
        while (*text) Character(static_cast<unsigned char>(*text++));
    }
    void Number(std::uint64_t value, unsigned width = 0) noexcept {
        wchar_t digits[20];
        unsigned count = 0;
        do { digits[count++] = static_cast<wchar_t>(L'0' + value % 10); value /= 10; } while (value);
        for (unsigned i = count; i < width; ++i) Character(L'0');
        while (count) Character(digits[--count]);
    }
    void Hex(std::uint64_t value, unsigned digits) noexcept {
        for (unsigned i = digits; i; --i)
            Character(L"0123456789abcdef"[(value >> ((i - 1) * 4)) & 15]);
    }
    void Guid(const GUID& id) noexcept {
        Hex(id.Data1, 8); Character(L'-'); Hex(id.Data2, 4); Character(L'-');
        Hex(id.Data3, 4); Character(L'-'); Hex(id.Data4[0], 2); Hex(id.Data4[1], 2);
        Character(L'-');
        for (unsigned i = 2; i < 8; ++i) Hex(id.Data4[i], 2);
    }
    void Utc(std::uint64_t tick) noexcept {
        ULARGE_INTEGER utc{};
        utc.LowPart = gSession.utcStart.dwLowDateTime;
        utc.HighPart = gSession.utcStart.dwHighDateTime;
        // UTC is anchored once, then advanced by a monotonic millisecond clock;
        // subsequent wall-clock adjustments cannot reverse event timestamps.
        if (tick >= gSession.monotonicStartMs)
            utc.QuadPart += (tick - gSession.monotonicStartMs) * 10000;
        const FILETIME fileTime{utc.LowPart, utc.HighPart};
        SYSTEMTIME time{};
        if (!FileTimeToSystemTime(&fileTime, &time)) { Append(L"invalid-utc"); return; }
        Number(time.wYear, 4); Character(L'-'); Number(time.wMonth, 2); Character(L'-');
        Number(time.wDay, 2); Character(L'T'); Number(time.wHour, 2); Character(L':');
        Number(time.wMinute, 2); Character(L':'); Number(time.wSecond, 2);
        Character(L'.'); Number(time.wMilliseconds, 3); Character(L'Z');
    }
    void Prefix(std::uint64_t tick, DWORD threadId) noexcept {
        Append(L"[LakeWindowedFix "); Ascii(LAKE_FIX_VERSION); Append(L"] build=");
        Ascii(gSession.buildId); Append(L" tick=");
        Number(tick); Append(L" pid="); Number(gSession.processId);
        Append(L" tid="); Number(threadId); Append(L" session="); Guid(gSession.id);
        Append(L" utc="); Utc(tick); Character(L' ');
    }
    const wchar_t* Data() const noexcept { return text_; }
    int Size() const noexcept { return static_cast<int>(length_); }
private:
    static constexpr unsigned kCapacity = 1536;
    wchar_t text_[kCapacity];
    unsigned length_ = 0;
};

enum class Output { Written, Limit, Io };

bool FileSink(void*, const char* text, DWORD bytes, DWORD& written, DWORD& error) noexcept {
    const BOOL ok = WriteFile(gFile, text, bytes, &written, nullptr);
    error = ok ? ERROR_SUCCESS : GetLastError();
    return ok != FALSE;
}

Output Emit(Text& text, bool footer = false) noexcept {
    if (gIoFailed.load(std::memory_order_relaxed)) return Output::Io;
    if (!footer && gLimit.load(std::memory_order_relaxed)) return Output::Limit;
    text.Append(L"\r\n");
    char utf8[6144];
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.Data(), text.Size(), utf8,
                                       static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    if (size <= 0) {
        gIoError.store(GetLastError(), std::memory_order_relaxed);
        gIoFailed.store(1, std::memory_order_relaxed);
        return Output::Io;
    }
    const DWORD bytes = static_cast<DWORD>(size);
    const DWORD limit = gOptions.maximumFileBytes - (footer ? 0 : kFooterReserveBytes);
    const DWORD used = gBytes.load(std::memory_order_relaxed);
    if (used > limit || bytes > limit - used) {
        gLimit.store(1, std::memory_order_relaxed);
        return Output::Limit;
    }
    DWORD written = 0, error = ERROR_SUCCESS;
    Increment(gCounts.writeCalls);
    // NO diagnostics lock held across a user sink or a potentially stuck WriteFile.
    const bool ok = gSink(gSinkContext, utf8, bytes, written, error);
    const bool invalidCount = written > bytes;
    if (invalidCount) written = bytes;
    gBytes.store(used + written, std::memory_order_relaxed);
    if (!ok || written != bytes || invalidCount) {
        if (invalidCount) error = ERROR_INVALID_DATA;
        else if (!error) error = ERROR_WRITE_FAULT;
        gIoError.store(error, std::memory_order_relaxed);
        gIoFailed.store(1, std::memory_order_relaxed); // Terminal: no retries/storms.
        return Output::Io;
    }
    return Output::Written;
}

void Metadata(Text& text, bool footer = false) noexcept {
    Increment(Emit(text, footer) == Output::Written ? gCounts.metadataWritten : gCounts.metadataDropped);
}

void Header() noexcept {
    Text text;
    text.Prefix(gSession.monotonicStartMs, GetCurrentThreadId());
    text.Append(L"Diagnostics session begin schema=1 fileCap=");
    text.Number(gOptions.maximumFileBytes); text.Append(L" queueCapacity=");
    text.Number(kQueueCapacity); text.Append(L" textCapacity="); text.Number(kTextCapacity);
    text.Append(L" utc=monotonic-anchored loss-counters=modulo32");
    text.Append(L" pointer-bits="); text.Number(sizeof(void*) * 8);
    text.Append(L" process-creation-filetime=");
    if (gSession.processCreationTimeAvailable)
        text.Number((static_cast<std::uint64_t>(gSession.processCreationTime.dwHighDateTime) << 32) |
                    gSession.processCreationTime.dwLowDateTime);
    else text.Append(L"unavailable");
    Metadata(text);
    gHeaderComplete.store(1, std::memory_order_release);
    SetEvent(gProgress);
}

bool Contains(ImageRange range, std::uintptr_t address) noexcept {
    return range.base && address >= range.base && address - range.base < range.bytes;
}

void Consume(const Event& event) noexcept {
    Text text;
    text.Prefix(event.tick, event.threadId);
    if (event.kind == Kind::Text) {
        // Existing diagnostic message text is retained verbatim (UTF-8 output).
        for (unsigned i = 0; i < event.text.length; ++i) text.Character(event.text.text[i]);
        if (event.text.truncated) text.Append(L" [diagnostics truncated]");
    } else {
        const auto& exception = event.exception;
        text.Append(L"First-chance exception (may be handled by application) code=");
        text.Number(exception.code); text.Append(L" (0x"); text.Hex(exception.code, 8);
        text.Append(L") address=0x"); text.Hex(exception.address, sizeof(void*) * 2);
        const wchar_t* image = L"outside-cached-target-images";
        std::uintptr_t rva = 0;
        if (Contains(gOptions.mainImage, exception.address)) {
            image = L"main-executable"; rva = exception.address - gOptions.mainImage.base;
        } else if (Contains(gOptions.rendererImage, exception.address)) {
            image = L"Contour.dll"; rva = exception.address - gOptions.rendererImage.base;
        }
        text.Append(L" cached-image="); text.Append(image); text.Append(L" rva=0x");
        text.Hex(rva, sizeof(void*) * 2);
    }
    const Output result = Emit(text);
    if (result == Output::Written) Increment(gCounts.writtenRecords);
    else if (result == Output::Limit) Increment(gCounts.droppedFileLimit);
    else Increment(gCounts.droppedIo);
    Complete(event); // A terminally discarded record also satisfies a drain barrier.
}

bool ReadSnapshot(Snapshot& output, bool wait) noexcept {
    if (wait) AcquireSRWLockShared(&gQueueLock); // Final worker summary only.
    else if (!TryAcquireSRWLockShared(&gQueueLock)) return false;
    Snapshot s{};
    s.state = LoadState();
    s.enqueued = gEnqueued; s.completed = gCompleted;
    s.queueDepth = gDepth; s.highWater = gHighWater; s.inFlight = gInFlight;
    ReleaseSRWLockShared(&gQueueLock);
    if (s.state == State::Running || s.state == State::Stopping || s.state == State::Stopped)
        s.session = gSession;
    s.startError = gStartError.load(std::memory_order_relaxed);
#define LAKE_DIAG_COUNT(name) s.name = gCounts.name.load(std::memory_order_relaxed)
    LAKE_DIAG_COUNT(rejectedInactive); LAKE_DIAG_COUNT(rejectedInvalid);
    LAKE_DIAG_COUNT(droppedBusy); LAKE_DIAG_COUNT(droppedFull); LAKE_DIAG_COUNT(truncated);
    LAKE_DIAG_COUNT(writtenRecords); LAKE_DIAG_COUNT(droppedFileLimit); LAKE_DIAG_COUNT(droppedIo);
    LAKE_DIAG_COUNT(metadataWritten); LAKE_DIAG_COUNT(metadataDropped); LAKE_DIAG_COUNT(writeCalls);
    LAKE_DIAG_COUNT(workerWakeups); LAKE_DIAG_COUNT(statsCalls);
#undef LAKE_DIAG_COUNT
    s.bytesWritten = gBytes.load(std::memory_order_relaxed);
    s.lastIoError = gIoError.load(std::memory_order_relaxed);
    s.fileLimitReached = gLimit.load(std::memory_order_relaxed) != 0;
    s.ioFailed = gIoFailed.load(std::memory_order_relaxed) != 0;
    s.countersWrapped = gWrapped.load(std::memory_order_relaxed) != 0;
    output = s;
    return true;
}

void Summary(bool final) noexcept {
    Snapshot s{};
    if (!ReadSnapshot(s, final)) return;
    Text text;
    text.Prefix(GetTickCount64(), GetCurrentThreadId());
    text.Append(final ? L"Diagnostics session end" : L"Diagnostics counts");
    text.Append(L" enqueued="); text.Number(s.enqueued);
    text.Append(L" completed="); text.Number(s.completed);
    text.Append(L" written="); text.Number(s.writtenRecords);
    text.Append(L" inactive="); text.Number(s.rejectedInactive);
    text.Append(L" invalid="); text.Number(s.rejectedInvalid);
    text.Append(L" droppedBusy="); text.Number(s.droppedBusy);
    text.Append(L" droppedFull="); text.Number(s.droppedFull);
    text.Append(L" droppedFileLimit="); text.Number(s.droppedFileLimit);
    text.Append(L" droppedIo="); text.Number(s.droppedIo);
    text.Append(L" metadataDropped="); text.Number(s.metadataDropped);
    text.Append(L" truncated="); text.Number(s.truncated);
    text.Append(L" highWater="); text.Number(s.highWater);
    text.Append(L" bytesBeforeSummary="); text.Number(s.bytesWritten);
    text.Append(L" lastIoError="); text.Number(s.lastIoError);
    text.Append(L" fileLimitReached="); text.Number(s.fileLimitReached ? 1 : 0);
    text.Append(L" ioFailed="); text.Number(s.ioFailed ? 1 : 0);
    text.Append(L" countersWrapped="); text.Number(s.countersWrapped ? 1 : 0);
    Metadata(text, final); // A single reserved footer, never rotation/deletion.
}

void Report() noexcept {
    if (gOptions.statsCallback) {
        PresenterStats stats{};
        Increment(gCounts.statsCalls);
        // In particular do not carry the queue/flush lock into GetScalePresenterStats.
        if (gOptions.statsCallback(gOptions.statsContext, stats)) {
            Text text;
            text.Prefix(GetTickCount64(), GetCurrentThreadId());
            text.Append(L"PresenterStats directDrawObjects="); text.Number(stats.directDrawObjects);
            text.Append(L" primarySurfaces="); text.Number(stats.primarySurfaces);
            text.Append(L" liveProxyObjects="); text.Number(stats.liveProxyObjects);
            text.Append(L" ownerContexts="); text.Number(stats.ownerContexts);
            Metadata(text);
        }
    }
    Summary(false);
}

DWORD WINAPI Worker(void*) noexcept {
    gWorkerThread.store(GetCurrentThreadId(), std::memory_order_release);
    Header();
    const DWORD interval = gOptions.reportIntervalMs;
    std::uint64_t nextReport = GetTickCount64() + interval;
    for (;;) {
        Event event;
        bool empty = false;
        // Bound each batch so continuous traffic cannot starve periodic stats.
        for (unsigned i = 0; i < 32; ++i) {
            if (!Pop(event)) { empty = true; break; }
            Consume(event);
        }
        const State state = LoadState();
        if (empty && state == State::Stopping) {
            // Pop may have observed empty BEFORE the final producer published
            // and Shutdown closed admission. Recheck after observing Stopping.
            AcquireSRWLockShared(&gQueueLock);
            const bool drained = gDepth == 0;
            ReleaseSRWLockShared(&gQueueLock);
            if (drained) break;
            continue;
        }
        const std::uint64_t now = GetTickCount64();
        bool report = interval && now >= nextReport;
#if defined(LAKE_DIAGNOSTICS_TESTING)
        report = gReportRequested.exchange(0, std::memory_order_acq_rel) != 0 || report;
#endif
        if (state == State::Running && report) {
            Report();
            nextReport = GetTickCount64() + interval;
        }
        if (!empty) continue;
        DWORD wait = INFINITE;
        if (interval && state == State::Running) {
            const std::uint64_t current = GetTickCount64();
            wait = current >= nextReport ? 0 : static_cast<DWORD>(nextReport - current);
        }
        const DWORD result = WaitForSingleObject(gWake, wait);
        Increment(gCounts.workerWakeups);
        if (result == WAIT_FAILED) {
            gIoError.store(GetLastError(), std::memory_order_relaxed);
            gIoFailed.store(1, std::memory_order_relaxed);
            // Owned event handles are never closed while active; fail closed if
            // this invariant is broken instead of a WAIT_FAILED busy loop.
            gState.store(static_cast<std::uint32_t>(State::Stopping), std::memory_order_release);
        }
    }
    Summary(true);
    if (gFile != INVALID_HANDLE_VALUE) CloseHandle(gFile);
    gState.store(static_cast<std::uint32_t>(State::Stopped), std::memory_order_release);
    SetEvent(gProgress);
    SetEvent(gExited); // No module/context access after this notification.
    return 0;
}

void FailedStart(DWORD error) noexcept {
    if (gFile != INVALID_HANDLE_VALUE) CloseHandle(gFile);
    if (gWake) CloseHandle(gWake);
    if (gProgress) CloseHandle(gProgress);
    if (gExited) CloseHandle(gExited);
    gFile = INVALID_HANDLE_VALUE;
    gWake = gProgress = gExited = nullptr;
    gStartError.store(error ? error : ERROR_GEN_FAILURE, std::memory_order_relaxed);
    gState.store(static_cast<std::uint32_t>(State::Failed), std::memory_order_release);
}

bool StartImpl(HANDLE file, Sink sink, void* context, const Options& options) noexcept {
    std::uint32_t expected = static_cast<std::uint32_t>(State::NotStarted);
    if (!gState.compare_exchange_strong(expected, static_cast<std::uint32_t>(State::Starting),
                                        std::memory_order_acq_rel)) return false;
    if (options.maximumFileBytes < 8192 || options.maximumFileBytes > kMaximumFileBytes) {
        FailedStart(ERROR_INVALID_PARAMETER); return false;
    }
    gOptions = options;
    if (gOptions.reportIntervalMs) {
        if (gOptions.reportIntervalMs < 1000) gOptions.reportIntervalMs = 1000;
        if (gOptions.reportIntervalMs > 60000) gOptions.reportIntervalMs = 60000;
    }
    const char* build = options.buildId ? options.buildId : "unknown";
    unsigned i = 0;
    for (; i + 1 < kBuildIdCapacity && build[i]; ++i) {
        const char ch = build[i];
        gSession.buildId[i] = ch >= '!' && ch <= '~' && ch != '[' && ch != ']' ? ch : '_';
    }
    gSession.buildId[i] = '\0';
    gOptions.buildId = gSession.buildId;
    gSession.processId = GetCurrentProcessId();
    FILETIME processExit{}, kernelTime{}, userTime{};
    gSession.processCreationTimeAvailable = GetProcessTimes(GetCurrentProcess(), &gSession.processCreationTime,
                                                           &processExit, &kernelTime, &userTime) != FALSE;
    gSession.monotonicStartMs = GetTickCount64();
    GetSystemTimePreciseAsFileTime(&gSession.utcStart);
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&gSession.id), sizeof(GUID),
                       BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        FailedStart(ERROR_GEN_FAILURE); return false;
    }
    gSession.id.Data3 = static_cast<WORD>((gSession.id.Data3 & 0x0fff) | 0x4000);
    gSession.id.Data4[0] = static_cast<BYTE>((gSession.id.Data4[0] & 0x3f) | 0x80);
    if (!sink) {
        if (!file || file == INVALID_HANDLE_VALUE) { FailedStart(ERROR_INVALID_HANDLE); return false; }
        if (GetFileType(file) != FILE_TYPE_DISK) { FailedStart(ERROR_INVALID_HANDLE); return false; }
        if (!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &gFile, 0,
                             FALSE, DUPLICATE_SAME_ACCESS)) {
            FailedStart(GetLastError()); return false;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(gFile, &size)) { FailedStart(GetLastError()); return false; }
        if (size.QuadPart < 0 || static_cast<ULONGLONG>(size.QuadPart) >
                options.maximumFileBytes - kFooterReserveBytes) {
            FailedStart(ERROR_FILE_TOO_LARGE); return false;
        }
        gBytes.store(static_cast<DWORD>(size.QuadPart), std::memory_order_relaxed);
        sink = FileSink;
    }
    gSink = sink;
    gSinkContext = context;
    gWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!gWake) { FailedStart(GetLastError()); return false; }
    gProgress = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!gProgress) { FailedStart(GetLastError()); return false; }
    gExited = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!gExited) { FailedStart(GetLastError()); return false; }
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&Worker), &pinned)) {
        FailedStart(GetLastError()); return false;
    }
    const HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!thread) { FailedStart(GetLastError()); return false; }
    CloseHandle(thread); // Wait via gExited, not an additional process-lifetime handle.
    gState.store(static_cast<std::uint32_t>(State::Running), std::memory_order_release);
    SetEvent(gWake);
    return true;
}

DWORD BoundedWait(DWORD timeout) noexcept { return timeout > kMaximumWaitMs ? kMaximumWaitMs : timeout; }

bool Barrier(std::uint64_t target, bool& done) noexcept {
    if (!TryAcquireSRWLockShared(&gQueueLock)) return false;
    done = gCompleted >= target && gHeaderComplete.load(std::memory_order_acquire) != 0;
    ReleaseSRWLockShared(&gQueueLock);
    return true;
}

} // namespace

bool Start(HANDLE file, const Options& options) noexcept {
    PreserveLastError preserve;
    return StartImpl(file, nullptr, nullptr, options);
}

bool Write(const wchar_t* text) noexcept {
    PreserveLastError preserve;
    if (!text) { Increment(gCounts.rejectedInvalid); return false; }
    Event* event = Reserve();
    if (!event) return false;
    event->kind = Kind::Text;
    unsigned length = 0;
    while (length + 1 < kTextCapacity && text[length]) {
        event->text.text[length] = text[length];
        ++length;
    }
    event->text.text[length] = L'\0';
    event->text.length = length;
    event->text.truncated = text[length] != L'\0';
    if (event->text.truncated) Increment(gCounts.truncated);
    Commit(*event);
    return true;
}

bool RecordException(DWORD code, const void* address) noexcept {
    PreserveLastError preserve;
    Event* event = Reserve();
    if (!event) return false;
    event->kind = Kind::Exception;
    event->exception.code = code;
    event->exception.address = reinterpret_cast<std::uintptr_t>(address);
    Commit(*event);
    return true;
}

bool Read(Snapshot& output) noexcept {
    PreserveLastError preserve;
    return ReadSnapshot(output, false);
}

bool Flush(DWORD timeoutMs) noexcept {
    PreserveLastError preserve;
    const State state = LoadState();
    if (state != State::Running && state != State::Stopping && state != State::Stopped) return false;
    if (state != State::Stopped &&
        GetCurrentThreadId() == gWorkerThread.load(std::memory_order_acquire)) return false;
    if (!TryAcquireSRWLockExclusive(&gFlushLock)) return false;
    struct Unlock { ~Unlock() noexcept { ReleaseSRWLockExclusive(&gFlushLock); } } unlock;
    const std::uint64_t deadline = GetTickCount64() + BoundedWait(timeoutMs);
    if (!TryAcquireSRWLockShared(&gQueueLock)) return false;
    const std::uint64_t target = gEnqueued;
    ReleaseSRWLockShared(&gQueueLock);
    for (;;) {
        bool done = false;
        if (!Barrier(target, done)) return false;
        if (done) return true;
        const std::uint64_t now = GetTickCount64();
        if (now >= deadline) return false;
        if (WaitForSingleObject(gProgress, static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0)
            return false;
    }
}

bool Shutdown(DWORD timeoutMs) noexcept {
    PreserveLastError preserve;
    const State state = LoadState();
    if (state == State::NotStarted || state == State::Starting || state == State::Failed) return false;
    if (state != State::Stopped &&
        GetCurrentThreadId() == gWorkerThread.load(std::memory_order_acquire)) return false;
    std::uint32_t expected = static_cast<std::uint32_t>(State::Running);
    gState.compare_exchange_strong(expected, static_cast<std::uint32_t>(State::Stopping),
                                   std::memory_order_acq_rel);
    SetEvent(gWake);
    // Also wait when State is already Stopped: gExited is the lifetime boundary,
    // not the earlier state store just before the final notifications.
    return WaitForSingleObject(gExited, BoundedWait(timeoutMs)) == WAIT_OBJECT_0;
}

#if defined(LAKE_DIAGNOSTICS_TESTING)
bool StartForTesting(TestSink sink, void* context, const Options& options) noexcept {
    PreserveLastError preserve;
    if (!sink) return false;
    return StartImpl(INVALID_HANDLE_VALUE, sink, context, options);
}

void RequestReportForTesting() noexcept {
    PreserveLastError preserve;
    const State state = LoadState();
    if (state != State::Running && state != State::Stopping) return;
    gReportRequested.store(1, std::memory_order_release);
    SetEvent(gWake);
}

bool WithQueueLockForTesting(void (*probe)(void*) noexcept, void* context) noexcept {
    PreserveLastError preserve;
    if (!probe || !TryAcquireSRWLockExclusive(&gQueueLock)) return false;
    probe(context);
    ReleaseSRWLockExclusive(&gQueueLock);
    return true;
}
#endif

} // namespace lakefix::diagnostics
