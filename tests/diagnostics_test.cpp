#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/session_diagnostics.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <new>
#include <string_view>
#include <type_traits>

#if !defined(LAKE_DIAGNOSTICS_TESTING)
#error Compile both sources with LAKE_DIAGNOSTICS_TESTING for this isolated test.
#endif

// Synthetic child processes only. No application, renderer, windows, GPU,
// hardware, installation, network, injection, or operational log files.
namespace {
namespace diag = lakefix::diagnostics;
constexpr DWORD kError = 0x4c414b45;
int checks = 0, failures = 0;
std::atomic<unsigned> newCalls{0};

void Check(bool condition, const char* name) {
    ++checks;
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

struct SinkState {
    HANDLE entered = nullptr, release = nullptr;
    DWORD mainThread = 0;
    unsigned blockCall = 1, failCall = 0, shortCall = 0, invalidCountCall = 0;
    bool reenter = false;
    bool reenteredRead = false, reenteredWrite = false, workerWaitsRejected = false;
    bool consumerOnly = true, overflow = false;
    DWORD consumerThread = 0;
    std::atomic<unsigned> calls{0};
    DWORD total = 0;
    char text[diag::kMaximumFileBytes + 1]{};
};
SinkState sink{}; // Context and storage survive every timeout until process exit.

bool Has(const char* text) noexcept { return std::strstr(sink.text, text) != nullptr; }

bool Consumer(void* context, const char* text, DWORD bytes, DWORD& written, DWORD& error) noexcept {
    auto& s = *static_cast<SinkState*>(context);
    const unsigned call = s.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const DWORD tid = GetCurrentThreadId();
    if (!s.consumerThread) s.consumerThread = tid;
    s.consumerOnly = s.consumerOnly && tid != s.mainThread && tid == s.consumerThread;
    if (call == s.blockCall) {
        SetEvent(s.entered);
        if (WaitForSingleObject(s.release, INFINITE) != WAIT_OBJECT_0) {
            written = 0; error = ERROR_OPERATION_ABORTED; return false;
        }
    }
    if (s.reenter && call == 1) {
        diag::Snapshot snapshot{};
        s.reenteredRead = diag::Read(snapshot);
        s.reenteredWrite = diag::Write(L"consumer sink reentry");
        s.workerWaitsRejected = !diag::Flush(5000) && !diag::Shutdown(5000);
    }
    written = call == s.failCall ? 0 : (call == s.shortCall ? bytes / 2 : bytes);
    error = call == s.failCall ? ERROR_DISK_FULL : ERROR_SUCCESS;
    if (written > diag::kMaximumFileBytes - s.total) {
        s.overflow = true; written = 0; error = ERROR_BUFFER_OVERFLOW; return false;
    }
    std::memcpy(s.text + s.total, text, written);
    s.total += written;
    s.text[s.total] = '\0';
    if (call == s.invalidCountCall) written = bytes + 1;
    SetLastError(kError + 1); // Public operations must preserve the caller, not this thread.
    return call != s.failCall;
}

bool Prepare() {
    sink.mainThread = GetCurrentThreadId();
    sink.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    sink.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(sink.entered && sink.release, "owned test gate events created");
    return sink.entered && sink.release;
}

diag::Options Options() noexcept {
    diag::Options options{};
    options.reportIntervalMs = 0;
    options.buildId = "diagnostics-test";
    return options;
}

bool Begin(const diag::Options& options) {
    SetLastError(kError);
    const bool started = diag::StartForTesting(Consumer, &sink, options);
    Check(started && GetLastError() == kError, "Start succeeds outside loader lock, preserving LastError");
    if (!started) return false;
    const DWORD gate = WaitForSingleObject(sink.entered, 5000);
    Check(gate == WAIT_OBJECT_0, "consumer is deterministically stalled on its first output");
    if (gate != WAIT_OBJECT_0) { SetEvent(sink.release); return false; }
    return true;
}

diag::Snapshot ReadRequired(const char* name) {
    diag::Snapshot snapshot{};
    SetLastError(kError);
    const bool read = diag::Read(snapshot);
    Check(read && GetLastError() == kError, name);
    return snapshot;
}

bool Finish() {
    SetEvent(sink.release);
    SetLastError(kError);
    const bool stopped = diag::Shutdown(5000);
    Check(stopped && GetLastError() == kError, "bounded Shutdown drains and preserves LastError");
    if (stopped) {
        Check(sink.consumerOnly && !sink.overflow, "only one background consumer invokes the bounded sink");
        Check(CloseHandle(sink.entered) && CloseHandle(sink.release), "test gate handles closed after worker exits");
    }
    return stopped;
}

struct ProbeResult { bool writes = false, read = false, flush = false; };
void ContentionProbe(void* context) noexcept {
    auto& result = *static_cast<ProbeResult*>(context);
    SetLastError(kError);
    const bool write = diag::Write(L"must be dropped, not wait");
    const bool exception = diag::RecordException(EXCEPTION_ACCESS_VIOLATION,
                                                reinterpret_cast<void*>(1));
    result.writes = !write && !exception && GetLastError() == kError;
    diag::Snapshot output{};
    output.enqueued = 12345;
    unsigned char saved[sizeof(output)];
    std::memcpy(saved, &output, sizeof(output));
    result.read = !diag::Read(output) && std::memcmp(saved, &output, sizeof(output)) == 0 &&
                  GetLastError() == kError;
    result.flush = !diag::Flush(5000) && GetLastError() == kError;
}

void Basic() {
    SetLastError(kError);
    Check(!diag::Write(L"before Start") && !diag::RecordException(1, nullptr) &&
          !diag::Write(nullptr) && GetLastError() == kError,
          "inactive/null producers return immediately with LastError intact");
    if (!Prepare()) return;
    auto options = Options();
    char build[] = "unit build[0.7]";
    options.buildId = build;
    options.mainImage = {0x1000, 0x100};
    options.rendererImage = {0x2000, 0x200};
    if (!Begin(options)) return;
    build[0] = 'X'; // Start must have copied caller-owned option text.
    ProbeResult probe{};
    Check(diag::WithQueueLockForTesting(ContentionProbe, &probe) && probe.writes && probe.read && probe.flush,
          "one-try producers/Read/Flush reject deterministic lock contention without waiting");
    SetLastError(kError);
    Check(diag::Write(L"fixture existing log text") && diag::Write(L"UTF8 \u00e7 \u754c"),
          "bounded UTF-16 text records admitted");
    wchar_t longText[1000];
    for (auto& ch : longText) ch = L'R';
    longText[999] = L'\0';
    SetLastError(kError);
    Check(diag::Write(longText) && GetLastError() == kError,
          "long text truncates without allocation or LastError changes");
    SetLastError(kError);
    Check(diag::RecordException(EXCEPTION_ACCESS_VIOLATION, reinterpret_cast<void*>(0x1020)) &&
          diag::RecordException(EXCEPTION_INT_DIVIDE_BY_ZERO, reinterpret_cast<void*>(0x2010)) &&
          diag::RecordException(EXCEPTION_ILLEGAL_INSTRUCTION, reinterpret_cast<void*>(0xffff0001)) &&
          GetLastError() == kError,
          "raw exception events accept opaque addresses without examining target memory");
    SetLastError(kError);
    Check(!diag::Flush(0) && GetLastError() == kError,
          "zero-timeout Flush cannot claim a blocked header/queue is drained");
    Check(!diag::StartForTesting(Consumer, &sink, options), "second Start cannot reset a live session");
    if (!Finish()) return;
    const auto s = ReadRequired("stopped snapshot preserves LastError");
    Check(s.state == diag::State::Stopped && s.enqueued == 6 && s.completed == 6 &&
          s.writtenRecords == 6 && s.queueDepth == 0 && !s.inFlight,
          "shutdown accounts for every admitted text/exception exactly once");
    Check(s.rejectedInactive == 2 && s.rejectedInvalid == 1 && s.droppedBusy == 2 &&
          s.droppedFull == 0 && s.truncated == 1 && !s.countersWrapped,
          "inactive, invalid, contention, and truncation counts remain distinct");
    Check((s.session.id.Data3 & 0xf000) == 0x4000 && (s.session.id.Data4[0] & 0xc0) == 0x80 &&
          s.session.processId == GetCurrentProcessId() && s.session.monotonicStartMs &&
          (s.session.utcStart.dwHighDateTime || s.session.utcStart.dwLowDateTime),
           "session has an independent UUID v4, PID, UTC anchor, and monotonic origin");
    FILETIME creation{}, exitTime{}, kernel{}, user{};
    Check(GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user) &&
          s.session.processCreationTimeAvailable &&
          s.session.processCreationTime.dwLowDateTime == creation.dwLowDateTime &&
          s.session.processCreationTime.dwHighDateTime == creation.dwHighDateTime && Has("process-creation-filetime="),
          "session header correlates proxy statistics to the actual process creation, not a recycled PID");
    Check(std::strcmp(s.session.buildId, "unit_build_0.7_") == 0 && Has("build=unit_build_0.7_"),
          "build ID copied/sanitized without changing caller-owned storage");
    Check(Has("session=") && Has(" utc=") && Has("Z fixture existing log text\r\n") &&
          Has("UTF8 \xc3\xa7 \xe7\x95\x8c") && Has("[diagnostics truncated]"),
          "consumer retains fixture text, emits UTF-8, UTC, GUID, and truncation marker");
    Check(Has("code=3221225477 (0xc0000005) address=0x00001020 cached-image=main-executable rva=0x00000020") &&
          Has("cached-image=Contour.dll rva=0x00000010") && Has("outside-cached-target-images"),
          "consumer alone formats raw exceptions and cached image classifications");
    Check(Has("Diagnostics session end") && Has("droppedBusy=2") && s.bytesWritten == sink.total &&
          s.metadataWritten == 2 && !s.ioFailed && !s.fileLimitReached,
          "reserved final loss summary and byte accounting are complete");
    SetLastError(kError);
    Check(diag::Flush(0) && diag::Shutdown(0) && !diag::Write(L"after shutdown") &&
          GetLastError() == kError, "stopped drain is idempotent; late producers are rejected");
}

struct FloodContext {
    unsigned iterations = 20000;
    unsigned accepted = 0;
    bool errorsPreserved = true;
    HANDLE firstAccepted = nullptr;
};
FloodContext flood[8]{};

DWORD WINAPI Producer(void* context) noexcept {
    auto& c = *static_cast<FloodContext*>(context);
    for (unsigned i = 0; i < c.iterations; ++i) {
        SetLastError(kError);
        const bool accepted = i % 2 ? diag::Write(L"synthetic producer flood") :
            diag::RecordException(EXCEPTION_ACCESS_VIOLATION, reinterpret_cast<void*>(1));
        c.errorsPreserved = c.errorsPreserved && GetLastError() == kError;
        if (accepted) {
            ++c.accepted;
            if (c.firstAccepted) SetEvent(c.firstAccepted);
        }
    }
    return 0;
}

unsigned StartProducers(HANDLE* threads, unsigned count) {
    unsigned created = 0;
    for (; created < count; ++created) {
        threads[created] = CreateThread(nullptr, 0, Producer, &flood[created], 0, nullptr);
        if (!threads[created]) break;
    }
    Check(created == count, "all owned synthetic producer threads started");
    return created;
}

void Join(HANDLE* threads, unsigned count) {
    bool joined = true;
    for (unsigned i = 0; i < count; ++i) {
        const bool done = WaitForSingleObject(threads[i], 5000) == WAIT_OBJECT_0;
        joined = done && joined;
        if (done) CloseHandle(threads[i]);
    }
    Check(joined, "all producers complete despite a blocked/saturated consumer");
}

void Flood() {
    if (!Prepare() || !Begin(Options())) return;
    const unsigned allocations = newCalls.load();
    bool filled = true;
    for (unsigned i = 0; i < diag::kQueueCapacity; ++i) {
        SetLastError(kError);
        filled = diag::Write(L"preallocated ring fill") && GetLastError() == kError && filled;
    }
    Check(filled, "all fixed ring slots can fill while the consumer is stalled");
    SetLastError(kError);
    Check(!diag::Write(L"overflow") && !diag::RecordException(1, nullptr) && GetLastError() == kError,
          "full ring discards text and raw exception immediately");
    HANDLE threads[4]{};
    const ULONGLONG began = GetTickCount64();
    const unsigned created = StartProducers(threads, 4);
    Join(threads, created);
    const ULONGLONG elapsed = GetTickCount64() - began;
    const auto s = ReadRequired("flood snapshot readable while consumer remains blocked");
    unsigned attempts = 2;
    bool rejected = true;
    for (unsigned i = 0; i < created; ++i) {
        attempts += flood[i].iterations;
        rejected = rejected && !flood[i].accepted && flood[i].errorsPreserved;
    }
    Check(rejected && s.enqueued == diag::kQueueCapacity && s.completed == 0 &&
          s.queueDepth == diag::kQueueCapacity && s.highWater == diag::kQueueCapacity && !s.inFlight,
          "flood cannot grow the bounded queue or block producers on sink I/O");
    Check(s.droppedBusy + s.droppedFull == attempts && s.droppedFull >= 2,
          "every rejected concurrent attempt is accounted as busy or full");
    Check(newCalls.load() == allocations, "enqueue/flood paths perform zero C++ heap allocations");
    SetLastError(kError);
    const ULONGLONG flushBegan = GetTickCount64();
    Check(!diag::Flush(25) && GetLastError() == kError,
          "bounded Flush times out without waiting for stalled sink completion");
    std::printf("METRIC flood_attempts=%u producer_elapsed_ms=%llu flush_wait_ms=%llu\n",
                attempts, elapsed, GetTickCount64() - flushBegan);
    Check(!diag::Shutdown(0) && !diag::Write(L"shutdown admission closed"),
          "zero-timeout shutdown returns while sink is blocked and closes admission");
    if (!Finish()) return;
    const auto done = ReadRequired("quiescent flood counts readable");
    Check(done.enqueued == done.completed && done.writtenRecords == diag::kQueueCapacity &&
          done.queueDepth == 0 && !done.ioFailed && Has("Diagnostics session end"),
          "releasing slow sink drains exactly the bounded backlog and final summary");
}

void IoFailure(bool shortWrite, bool invalidCount) {
    if (!Prepare()) return;
    if (invalidCount) sink.invalidCountCall = 2;
    else if (shortWrite) sink.shortCall = 2;
    else sink.failCall = 2;
    if (!Begin(Options())) return;
    bool queued = true;
    for (unsigned i = 0; i < 8; ++i) queued = diag::Write(L"injected I/O failure") && queued;
    Check(queued, "failure fixture records all queued before consumer is released");
    if (!Finish()) return;
    const auto s = ReadRequired("failure snapshot readable after bounded shutdown");
    const DWORD expected = invalidCount ? ERROR_INVALID_DATA : (shortWrite ? ERROR_WRITE_FAULT : ERROR_DISK_FULL);
    Check(s.ioFailed && s.lastIoError == expected && s.completed == 8 && s.droppedIo == 8 &&
          s.writtenRecords == 0 && s.writeCalls == 2 && sink.calls.load() == 2,
          "failed/partial/invalid sink is terminal, counted once, never retried per queued record");
    Check(s.bytesWritten == sink.total && !s.fileLimitReached && !sink.overflow,
          "successful and partial bytes remain within the hard budget");
    Check(diag::Flush(0), "Flush confirms processing, not successful persistence, after I/O failure");
}

void FileCap() {
    if (!Prepare()) return;
    auto options = Options();
    options.maximumFileBytes = 8192;
    if (!Begin(options)) return;
    wchar_t text[diag::kTextCapacity];
    for (auto& ch : text) ch = L'\u754c';
    text[diag::kTextCapacity - 1] = L'\0';
    bool queued = true;
    for (unsigned i = 0; i < 32; ++i) queued = diag::Write(text) && queued;
    Check(queued, "file-cap fixture queues bounded worst-case UTF-8 payloads");
    if (!Finish()) return;
    const auto s = ReadRequired("file-cap snapshot readable");
    Check(s.fileLimitReached && !s.ioFailed && s.completed == 32 && s.droppedFileLimit > 0 &&
          s.writtenRecords + s.droppedFileLimit == 32,
          "file budget discards whole records with complete loss accounting");
    Check(s.bytesWritten == sink.total && sink.total <= 8192 && s.metadataWritten == 2 &&
          Has("Diagnostics session end") && !Has("droppedFileLimit=0 "),
          "one file stays within 8 KiB including its reserved final loss summary");
    std::printf("METRIC cap_bytes=%lu actual_bytes=%lu written=%u droppedFileLimit=%u\n",
                options.maximumFileBytes, sink.total, s.writtenRecords, s.droppedFileLimit);
}

struct CallbackState {
    HANDLE entered = nullptr, release = nullptr;
    bool read = false, write = false, exception = false, flush = false, shutdown = false, errors = false;
    unsigned calls = 0;
};
CallbackState callback{};

bool Stats(void* context, diag::PresenterStats& stats) noexcept {
    auto& c = *static_cast<CallbackState*>(context);
    ++c.calls;
    SetLastError(kError);
    diag::Snapshot s{};
    c.read = diag::Read(s);
    c.write = diag::Write(L"stats callback reentry");
    c.exception = diag::RecordException(0xe0000001, reinterpret_cast<void*>(1));
    c.flush = !diag::Flush(5000);
    c.shutdown = !diag::Shutdown(5000);
    c.errors = GetLastError() == kError;
    stats = {11, 22, 33, 44};
    SetEvent(c.entered);
    WaitForSingleObject(c.release, INFINITE); // Deliberate slow callback, not production guidance.
    return true;
}

void Reentrancy() {
    if (!Prepare()) return;
    callback.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    callback.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(callback.entered && callback.release, "owned stats callback gates created");
    if (!callback.entered || !callback.release) return;
    auto options = Options();
    options.statsCallback = Stats;
    options.statsContext = &callback;
    sink.reenter = true;
    if (!Begin(options)) return;
    SetLastError(kError);
    diag::RequestReportForTesting(); // Deterministic: no sleep or timer-phase assumptions.
    Check(GetLastError() == kError, "test report request preserves LastError");
    SetEvent(sink.release);
    const bool entered = WaitForSingleObject(callback.entered, 5000) == WAIT_OBJECT_0;
    Check(entered, "worker invokes optional stats callback without frame polling");
    if (!entered) { SetEvent(callback.release); Finish(); return; }
    Check(callback.read && callback.write && callback.exception && callback.flush && callback.shutdown && callback.errors,
          "stats callback reenters Read/Write/RecordException; worker Flush/Shutdown reject self-waits");
    Check(sink.reenteredRead && sink.reenteredWrite && sink.workerWaitsRejected,
          "injected consumer sink also runs without any diagnostics lock");
    SetLastError(kError);
    Check(diag::Write(L"renderer while stats callback is blocked") && GetLastError() == kError,
          "a stalled stats callback cannot lock out renderer enqueue");
    Check(!diag::Flush(25), "Flush remains bounded when callback is stalled");
    SetEvent(callback.release);
    if (!Finish()) return;
    const auto s = ReadRequired("reentrant callback final counts readable");
    Check(callback.calls == 1 && s.statsCalls == 1 && s.enqueued == 4 && s.completed == 4 &&
          s.writtenRecords == 4 && s.droppedBusy == 0,
          "callback and sink reentry produce no recursive reports or lock-contention drops");
    Check(Has("PresenterStats directDrawObjects=11 primarySurfaces=22 liveProxyObjects=33 ownerContexts=44") &&
          Has("stats callback reentry") && Has("consumer sink reentry") &&
          Has("renderer while stats callback is blocked"), "consumer formats optional presenter sample and reentrant messages");
    Check(CloseHandle(callback.entered) && CloseHandle(callback.release), "stats gates released only after worker exit");
}

void InvalidStart(bool badCap) {
    auto options = Options();
    if (badCap) options.maximumFileBytes = diag::kMaximumFileBytes + 1;
    SetLastError(kError);
    Check(!diag::Start(INVALID_HANDLE_VALUE, options) && GetLastError() == kError,
          "invalid Start fails without touching caller LastError");
    const auto s = ReadRequired("failed initialization snapshot readable");
    Check(s.state == diag::State::Failed && s.startError ==
          (badCap ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE) && s.writeCalls == 0,
          "invalid handle/cap fails closed before worker I/O, with explicit startError");
    Check(!diag::StartForTesting(Consumer, &sink, Options()) && !diag::Flush(0) && !diag::Shutdown(0),
          "failed one-shot initialization cannot restart or wait on missing resources");
}

void ShutdownRace() {
    if (!Prepare()) return;
    sink.blockCall = 0;
    SetEvent(sink.release);
    const HANDLE accepted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(accepted != nullptr, "owned first-admission event created");
    if (!accepted) return;
    Check(diag::StartForTesting(Consumer, &sink, Options()), "shutdown-race consumer started");
    HANDLE threads[8]{};
    for (auto& context : flood) context.firstAccepted = accepted;
    const unsigned created = StartProducers(threads, 8);
    Check(WaitForSingleObject(accepted, 5000) == WAIT_OBJECT_0, "shutdown races actual admitted records");
    const bool stopped = diag::Shutdown(5000);
    Check(stopped, "concurrent shutdown completes without abandoning the final admission");
    Join(threads, created);
    const auto s = ReadRequired("shutdown-race quiescent snapshot readable");
    std::uint64_t produced = 0, attempted = 0;
    bool errors = true;
    for (unsigned i = 0; i < created; ++i) {
        produced += flood[i].accepted; attempted += flood[i].iterations;
        errors = errors && flood[i].errorsPreserved;
    }
    Check(errors && s.enqueued == produced && s.completed == produced && s.queueDepth == 0 && !s.inFlight,
          "after concurrent shutdown all admitted tickets are completed, never stranded in the ring");
    Check(attempted == produced + s.droppedBusy + s.droppedFull + s.rejectedInactive,
          "shutdown-race accepted/busy/full/inactive categories exactly account for every attempt");
    if (stopped) { CloseHandle(sink.entered); CloseHandle(sink.release); }
    CloseHandle(accepted);
}

void OwnedHandle(bool oversized) {
    // CREATE_NEW under THIS executable's isolated build directory. Never reuse,
    // truncate, or inspect an existing file. Delete-on-close covers every exit.
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length == 32768) { Check(false, "test executable path readable"); return; }
    wchar_t* slash = std::wcsrchr(path, L'\\');
    if (!slash) { Check(false, "isolated test directory found"); return; }
    const std::size_t remaining = 32768 - static_cast<std::size_t>(slash + 1 - path);
    if (std::swprintf(slash + 1, remaining, L"diagnostics-owned-%lu.tmp", GetCurrentProcessId()) < 0) {
        Check(false, "owned temporary file name fits"); return;
    }
    const HANDLE view = CreateFileW(path, FILE_APPEND_DATA | FILE_READ_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    Check(view != INVALID_HANDLE_VALUE, "CREATE_NEW creates only a test-owned delete-on-close file");
    if (view == INVALID_HANDLE_VALUE) return;
    HANDLE file = nullptr;
    const bool duplicate = DuplicateHandle(GetCurrentProcess(), view, GetCurrentProcess(), &file,
                                           FILE_APPEND_DATA | SYNCHRONIZE, FALSE, 0) != FALSE;
    Check(duplicate, "caller receives append-only rights, without read-data/read-attributes access");
    if (!duplicate) { CloseHandle(view); return; }
    char seed[8192]{};
    std::memcpy(seed, "owned fixture prefix\r\n", 22);
    DWORD written = 0;
    const DWORD seedBytes = oversized ? static_cast<DWORD>(sizeof(seed)) : 22;
    Check(WriteFile(file, seed, seedBytes, &written, nullptr) && written == seedBytes,
          "only the owned fixture receives pre-existing bytes");
    LARGE_INTEGER zero{};
    Check(SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != FALSE,
          "fixture moves shared pointer to zero to verify append-only writes");
    auto options = Options();
    options.maximumFileBytes = 8192;
    SetLastError(kError);
    const bool started = diag::Start(file, options);
    Check(started == !oversized && GetLastError() == kError,
          "production Start validates an append HANDLE and preserves LastError");
    LARGE_INTEGER size{};
    Check(GetFileSizeEx(file, &size) != FALSE, "Start never closes the caller's original HANDLE");
    CloseHandle(file); // The worker must own a duplicate, not borrow this handle.
    bool stopped = !started;
    if (started) {
        // Header I/O does not hold the producer lock, but Pop briefly can. This
        // fixture deliberately does NOT require admission: Shutdown must always
        // preserve the seed and append its own header/footer using its duplicate.
        diag::Write(L"owned HANDLE diagnostic message");
        stopped = diag::Shutdown(5000);
        Check(stopped, "worker completes after caller closes its original handle");
    }
    const auto s = ReadRequired("owned-file snapshot readable");
    if (oversized) {
        Check(s.state == diag::State::Failed && s.startError == ERROR_FILE_TOO_LARGE && s.writeCalls == 0,
              "pre-existing bytes exceeding the data budget fail without truncation or I/O");
    } else {
        Check(s.state == diag::State::Stopped && !s.ioFailed && !s.fileLimitReached &&
              s.metadataWritten == 2 && s.bytesWritten > seedBytes && s.bytesWritten <= 8192,
              "real WriteFile appends a header/footer within the total file budget");
    }
    if (stopped) {
        char captured[8193]{};
        DWORD bytes = 0;
        Check(SetFilePointerEx(view, zero, nullptr, FILE_BEGIN) &&
              ReadFile(view, captured, 8192, &bytes, nullptr), "only the owned temporary file is read back");
        Check(bytes >= seedBytes && std::memcmp(captured, seed, seedBytes) == 0,
              "existing bytes remain unchanged despite the initial zero file pointer");
        Check(oversized ? bytes == seedBytes :
              bytes == s.bytesWritten && std::strstr(captured, "Diagnostics session end"),
              "real file size includes existing bytes and any final summary");
    }
    CloseHandle(view); // Last handle deletes THIS fixture; no DeleteFile/retention scan.
}

void Periodic() {
    if (!Prepare()) return;
    callback.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    callback.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(callback.entered && callback.release, "periodic stats gates created");
    if (!callback.entered || !callback.release) return;
    auto options = Options();
    options.reportIntervalMs = 1; // Production clamps to 1000; no test report request.
    options.statsCallback = Stats;
    options.statsContext = &callback;
    if (!Begin(options)) return;
    const auto before = ReadRequired("periodic callback is not invoked by Start or enqueue");
    Check(before.statsCalls == 0, "no synchronous presenter callback on producer/Initialize paths");
    const ULONGLONG began = GetTickCount64();
    SetEvent(sink.release);
    const bool entered = WaitForSingleObject(callback.entered, 5000) == WAIT_OBJECT_0;
    Check(entered, "idle event-driven worker reaches its low-frequency stats deadline");
    std::printf("METRIC periodic_first_callback_ms=%llu requested_interval_ms=1 effective_minimum_ms=1000\n",
                GetTickCount64() - began);
    SetEvent(callback.release);
    if (!Finish()) return;
    const auto s = ReadRequired("periodic final snapshot readable");
    Check(callback.calls == 1 && s.statsCalls == 1 && s.completed == s.enqueued && Has("PresenterStats"),
          "real worker deadline invokes exactly one sample before controlled shutdown");
    CloseHandle(callback.entered); CloseHandle(callback.release);
}

int RunCase(const wchar_t* name) {
    if (!std::wcscmp(name, L"basic")) Basic();
    else if (!std::wcscmp(name, L"flood")) Flood();
    else if (!std::wcscmp(name, L"fail")) IoFailure(false, false);
    else if (!std::wcscmp(name, L"short")) IoFailure(true, false);
    else if (!std::wcscmp(name, L"invalid-count")) IoFailure(false, true);
    else if (!std::wcscmp(name, L"cap")) FileCap();
    else if (!std::wcscmp(name, L"callback")) Reentrancy();
    else if (!std::wcscmp(name, L"bad-handle")) InvalidStart(false);
    else if (!std::wcscmp(name, L"bad-cap")) InvalidStart(true);
    else if (!std::wcscmp(name, L"shutdown-race")) ShutdownRace();
    else if (!std::wcscmp(name, L"handle")) OwnedHandle(false);
    else if (!std::wcscmp(name, L"existing-cap")) OwnedHandle(true);
    else if (!std::wcscmp(name, L"periodic")) Periodic();
    else { std::printf("Unknown synthetic case\n"); return 2; }
    std::printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}

int AllCases() {
    wchar_t executable[32768];
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length == 32768) return 2;
    const wchar_t* cases[] = {L"basic", L"flood", L"fail", L"short", L"invalid-count", L"cap", L"callback",
                              L"bad-handle", L"bad-cap", L"shutdown-race", L"handle", L"existing-cap", L"periodic"};
    for (const auto* name : cases) {
        std::printf("CASE %ls\n", name);
        wchar_t command[33000];
        const int count = std::swprintf(command, 33000, L"\"%ls\" --case %ls", executable, name);
        if (count < 0) return 2;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const bool created = CreateProcessW(executable, command, nullptr, nullptr, FALSE, 0,
                                             nullptr, nullptr, &startup, &process) != FALSE;
        if (!created) { Check(false, "synthetic child created"); continue; }
        const DWORD result = WaitForSingleObject(process.hProcess, 15000);
        DWORD exitCode = 1;
        if (result == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exitCode);
        else {
            // Only this test's OWN child, never an application/installed process.
            TerminateProcess(process.hProcess, 99);
            WaitForSingleObject(process.hProcess, 5000);
        }
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        Check(result == WAIT_OBJECT_0 && exitCode == 0, "synthetic case completes under watchdog");
    }
    std::printf("%d isolated case(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
} // namespace

// C++ allocation tripwire supplements the producer-path source audit. It does
// not claim to intercept OS allocations, CRT malloc, or BCrypt initialization.
void* operator new(std::size_t size) {
    newCalls.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

static_assert(std::is_trivially_copyable<diag::Snapshot>::value &&
              std::is_standard_layout<diag::Snapshot>::value, "Snapshots are bounded value data");
static_assert(sizeof(void*) == 4, "Run the requested x86 test binary, not an x64 surrogate");
static_assert(noexcept(diag::Write(nullptr)) && noexcept(diag::RecordException(0, nullptr)) &&
              noexcept(diag::Flush(0)) && noexcept(diag::Shutdown(0)), "Hot/control APIs are noexcept");

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && !std::wcscmp(argv[1], L"--case")) return RunCase(argv[2]);
    return AllCases();
}
