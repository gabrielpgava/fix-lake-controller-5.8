#pragma once

#include <windows.h>

namespace lakefix::startup {
constexpr DWORD kSnapshotAttempts = 8;
constexpr DWORD kSnapshotRetryDelay = 10;

enum class State { ready, childExited, waitError, timeout, failed };

struct Result {
    State state = State::failed;
    DWORD error = ERROR_GEN_FAILURE;
    bool hasExitCode = false;
    DWORD exitCode = 0;
    DWORD cleanupError = ERROR_SUCCESS;
};

constexpr Result Ready() noexcept { return {State::ready, ERROR_SUCCESS, false, 0, ERROR_SUCCESS}; }
constexpr Result Exited(DWORD code) noexcept { return {State::childExited, ERROR_SUCCESS, true, code, ERROR_SUCCESS}; }
constexpr Result Failure(DWORD error) noexcept {
    return {State::failed, error ? error : ERROR_GEN_FAILURE, false, 0, ERROR_SUCCESS};
}
constexpr Result Timeout() noexcept { return {State::timeout, ERROR_TIMEOUT, false, 0, ERROR_SUCCESS}; }

// Do NOT infer a timeout just because cleanup/the clock takes a long time.
// A real WaitForDebugEvent error always retains its original Win32 code.
constexpr Result DebugWaitFailure(DWORD error) noexcept {
    return error == ERROR_SEM_TIMEOUT || error == ERROR_TIMEOUT ? Timeout()
        : Result{State::waitError, error ? error : ERROR_GEN_FAILURE, false, 0, ERROR_SUCCESS};
}

constexpr bool RetrySnapshot(DWORD error, DWORD attemptsMade) noexcept {
    return error == ERROR_BAD_LENGTH && attemptsMade < kSnapshotAttempts;
}

constexpr bool MutexAcquired(DWORD wait) noexcept {
    // Abandonment grants ownership, but NEVER skips process enumeration.
    return wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
}

constexpr bool ExistingImageBlocks(DWORD pid) noexcept { return pid != 0; }

constexpr const char* StateName(State state) noexcept {
    switch (state) {
    case State::ready: return "ready";
    case State::childExited: return "child-exited";
    case State::waitError: return "wait-error";
    case State::timeout: return "timeout";
    case State::failed: return "failure";
    }
    return "failure";
}
} // namespace lakefix::startup
