#pragma once

#include "product_version.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

#ifndef LAKE_DIAGNOSTICS_BUILD_ID
#define LAKE_DIAGNOSTICS_BUILD_ID LAKE_FIX_BUILD_ID
#endif

namespace lakefix::diagnostics {

inline constexpr unsigned kQueueCapacity = 256;
inline constexpr unsigned kTextCapacity = 640; // Includes the terminating NUL.
inline constexpr unsigned kBuildIdCapacity = 64;
inline constexpr DWORD kMaximumFileBytes = 4u * 1024u * 1024u;
inline constexpr DWORD kFooterReserveBytes = 2048;
inline constexpr DWORD kMaximumWaitMs = 30000;

struct PresenterStats {
    std::uint32_t directDrawObjects = 0;
    std::uint32_t primarySurfaces = 0;
    std::uint32_t liveProxyObjects = 0;
    std::uint32_t ownerContexts = 0;
};

// Worker thread only, without any diagnostics lock. False omits this sample.
// It may reenter Write/RecordException/Read; Flush/Shutdown on the worker fail
// immediately. Code and context must survive until successful Shutdown or exit.
using StatsCallback = bool (*)(void* context, PresenterStats& output) noexcept;

struct ImageRange {
    std::uintptr_t base = 0;
    std::uintptr_t bytes = 0;
};

struct Options {
    const char* buildId = LAKE_DIAGNOSTICS_BUILD_ID; // Copied, bounded, ASCII sanitized.
    DWORD maximumFileBytes = kMaximumFileBytes; // [8192, kMaximumFileBytes].
    DWORD reportIntervalMs = 5000; // 0 disables; otherwise clamped to [1000,60000].
    StatsCallback statsCallback = nullptr;
    void* statsContext = nullptr;
    ImageRange mainImage{}; // Optional cached ranges; never dereferenced.
    ImageRange rendererImage{};
};

enum class State : std::uint32_t { NotStarted, Starting, Running, Stopping, Stopped, Failed };

struct SessionInfo {
    GUID id{}; // Random UUID v4, NOT derived from PID, clock, or file name.
    DWORD processId = 0;
    FILETIME utcStart{};
    FILETIME processCreationTime{};
    bool processCreationTimeAvailable = false;
    std::uint64_t monotonicStartMs = 0;
    char buildId[kBuildIdCapacity]{};
};

struct Snapshot {
    State state = State::NotStarted;
    DWORD startError = 0;
    SessionInfo session{}; // Available after Start succeeds, including after Shutdown.
    std::uint64_t enqueued = 0;
    std::uint64_t completed = 0; // Written OR discarded by consumer.
    unsigned queueDepth = 0;
    unsigned highWater = 0;
    bool inFlight = false;
    std::uint32_t rejectedInactive = 0;
    std::uint32_t rejectedInvalid = 0;
    std::uint32_t droppedBusy = 0;
    std::uint32_t droppedFull = 0;
    std::uint32_t truncated = 0;
    std::uint32_t writtenRecords = 0;
    std::uint32_t droppedFileLimit = 0;
    std::uint32_t droppedIo = 0;
    std::uint32_t metadataWritten = 0;
    std::uint32_t metadataDropped = 0;
    std::uint32_t writeCalls = 0;
    std::uint32_t workerWakeups = 0;
    std::uint32_t statsCalls = 0;
    DWORD bytesWritten = 0; // Includes pre-existing bytes, if any.
    DWORD lastIoError = 0;
    bool fileLimitReached = false;
    bool ioFailed = false;
    bool countersWrapped = false; // 32-bit counters are modulo 2^32, sticky flag.
};

// One attempt per process, ONLY from Initialize/control code outside loader lock.
// Duplicates a dedicated synchronous, append-only disk HANDLE; never opens a
// path, truncates, rotates, or closes the caller's handle. Existing size counts
// toward the cap. The image containing this module is permanently PINned.
// Link bcrypt. All public operations preserve the calling thread's LastError;
// a failed Start reports its error in Read().startError. No restart/unload API.
bool Start(HANDLE file, const Options& options = Options{}) noexcept;

// One TryAcquire, never a wait/retry, allocation, formatting, file I/O, or ODS.
// True means admitted, NOT persisted. Text must be readable through its NUL or
// kTextCapacity characters; copied at most kTextCapacity-1, with truncation count.
bool Write(const wchar_t* text) noexcept;
// Raw POD code/address/tick/tid only. Never examines memory at address. Caller
// selects exceptions and retains EXCEPTION_CONTINUE_SEARCH; not stack-overflow safe.
bool RecordException(DWORD code, const void* address) noexcept;

// Best-effort try-snapshot; false leaves output unchanged. Queue fields form a
// coherent view; counters/flags are individually atomic, not one global instant.
bool Read(Snapshot& output) noexcept;

// Control/shutdown paths ONLY, not DllMain/VEH/render. Timeout is clamped to
// kMaximumWaitMs (including INFINITE). False also covers lock contention and
// worker-thread reentry. True = header and pre-call admitted records processed,
// INCLUDING losses: inspect Read(). No FlushFileBuffers/durability guarantee.
// Timeout cannot cancel synchronous WriteFile or a stalled callback.
bool Flush(DWORD timeoutMs) noexcept;
// Stops admission, drains, writes a reserved final loss summary, waits boundedly.
// On false, the worker/owned file/callback context may STILL be in use. Even on
// true the module remains pinned; its three event handles live until process exit.
bool Shutdown(DWORD timeoutMs) noexcept;

#if defined(LAKE_DIAGNOSTICS_TESTING)
// Test-only replacement for WriteFile. Must report actual bytes (<= requested).
// Called on the real consumer, without diagnostics locks. No operational files.
using TestSink = bool (*)(void*, const char*, DWORD, DWORD& written, DWORD& error) noexcept;
bool StartForTesting(TestSink sink, void* context, const Options& options = Options{}) noexcept;
void RequestReportForTesting() noexcept;
// Deliberately holds the producer lock for deterministic contention probes ONLY.
bool WithQueueLockForTesting(void (*probe)(void*) noexcept, void* context) noexcept;
#endif

} // namespace lakefix::diagnostics
