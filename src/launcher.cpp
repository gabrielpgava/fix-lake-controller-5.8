#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include "startup_policy.h"
#include "component_contract.h"
#include "lake_build_profile.h"

static_assert(sizeof(void*) == 4, "Build the launcher and DLL for x86, not x64");

namespace {
constexpr DWORD kTimeout = 15000;
[[maybe_unused]] constexpr wchar_t kExeHash[] = LAKE_FIX_WIDEN(LAKE_BUILD007_EXE_SHA256);
[[maybe_unused]] constexpr wchar_t kContourHash[] = LAKE_FIX_WIDEN(LAKE_BUILD007_CONTOUR_SHA256);
using lakefix::startup::Result;
using lakefix::startup::State;

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = nullptr) {
        const DWORD error = GetLastError();
        if (valid()) CloseHandle(value_);
        value_ = value;
        SetLastError(error);
    }
private:
    HANDLE value_;
};

std::wstring FullPath(const std::wstring& path) {
    std::vector<wchar_t> buffer(32768);
    DWORD size = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!size) return {};
    if (size >= buffer.size()) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return {}; }
    return std::wstring(buffer.data());
}

std::wstring Parent(const std::wstring& path) {
    return path.substr(0, path.find_last_of(L"\\/"));
}

HANDLE CreateSnapshot(DWORD flags, DWORD pid,
                      decltype(&CreateToolhelp32Snapshot) create = &CreateToolhelp32Snapshot,
                      DWORD retryDelay = lakefix::startup::kSnapshotRetryDelay) {
    for (DWORD attempt = 1;; ++attempt) {
        HANDLE snapshot = create(flags, pid);
        if (snapshot != INVALID_HANDLE_VALUE) return snapshot;
        const DWORD error = GetLastError();
        if (!lakefix::startup::RetrySnapshot(error, attempt)) {
            SetLastError(error);
            return INVALID_HANDLE_VALUE;
        }
        Sleep(retryDelay);
    }
}

bool FindRunningImage(const wchar_t* name, DWORD& pid) {
    pid = 0;
    Handle snapshot(CreateSnapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) return GetLastError() == ERROR_NO_MORE_FILES;
    do {
        if (_wcsicmp(entry.szExeFile, name) == 0) { pid = entry.th32ProcessID; return true; }
    } while (Process32NextW(snapshot.get(), &entry));
    return GetLastError() == ERROR_NO_MORE_FILES;
}

// advapi32 is loaded explicitly from System32, keeping the existing launcher
// link contract (kernel32 + bcrypt) unchanged. TokenUser is an OS-provided SID,
// not a mutable username/environment variable. Local\\ scopes the kernel name
// to this Windows session; all installations for this user share one gate.
std::wstring StartupMutexName() {
    HMODULE security = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!security) return {};
    auto openToken = reinterpret_cast<decltype(&OpenProcessToken)>(reinterpret_cast<void*>(GetProcAddress(security, "OpenProcessToken")));
    auto tokenInfo = reinterpret_cast<decltype(&GetTokenInformation)>(reinterpret_cast<void*>(GetProcAddress(security, "GetTokenInformation")));
    HANDLE raw{};
    DWORD error = ERROR_PROC_NOT_FOUND;
    std::wstring name;
    if (openToken && tokenInfo) {
        if (openToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
            Handle token(raw);
            DWORD bytes{};
            tokenInfo(token.get(), TokenUser, nullptr, 0, &bytes);
            error = GetLastError();
            if (error == ERROR_INSUFFICIENT_BUFFER && bytes >= sizeof(TOKEN_USER)) {
                std::vector<BYTE> data(bytes);
                if (tokenInfo(token.get(), TokenUser, data.data(), bytes, &bytes)) {
                    const auto* user = reinterpret_cast<const TOKEN_USER*>(data.data());
                    const auto* sid = static_cast<const SID*>(user->User.Sid);
                    const ULONG_PTR begin = reinterpret_cast<ULONG_PTR>(data.data());
                    const ULONG_PTR at = reinterpret_cast<ULONG_PTR>(sid);
                    const size_t header = offsetof(SID, SubAuthority);
                    if (at >= begin && at - begin <= data.size() - header &&
                        sid->SubAuthorityCount <= SID_MAX_SUB_AUTHORITIES) {
                        const size_t sidBytes = header + sid->SubAuthorityCount * sizeof(DWORD);
                        if (sidBytes <= data.size() - (at - begin)) {
                            name = L"Local\\LakeWindowedFix.Startup.v1.";
                            const auto* value = reinterpret_cast<const BYTE*>(sid);
                            constexpr wchar_t hex[] = L"0123456789abcdef";
                            for (size_t i = 0; i < sidBytes; ++i) {
                                name += hex[value[i] >> 4];
                                name += hex[value[i] & 15];
                            }
                            error = ERROR_SUCCESS;
                        } else error = ERROR_INVALID_SID;
                    } else error = ERROR_INVALID_SID;
                } else error = GetLastError();
            }
        } else error = GetLastError();
    }
    FreeLibrary(security);
    SetLastError(error);
    return name;
}

class StartupMutex {
public:
    ~StartupMutex() { Release(); }
    bool Acquire(const std::wstring& name, DWORD timeout = kTimeout) {
        if (name.empty()) { SetLastError(ERROR_INVALID_NAME); return false; }
        handle_.reset(CreateMutexExW(nullptr, name.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE));
        if (!handle_.valid()) return false;
        const DWORD wait = WaitForSingleObject(handle_.get(), timeout);
        owned_ = lakefix::startup::MutexAcquired(wait);
        if (!owned_ && wait != WAIT_FAILED) SetLastError(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_GEN_FAILURE);
        return owned_;
    }
    void Release() {
        const DWORD error = GetLastError();
        if (owned_) ReleaseMutex(handle_.get());
        owned_ = false;
        handle_.reset();
        SetLastError(error);
    }
private:
    Handle handle_;
    bool owned_ = false;
};

std::wstring TryWriteReport(const std::wstring& directory, DWORD pid, const wchar_t* kind, const char* line, DWORD length) {
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};
    const std::wstring path = directory + L"\\LakeWindowedFix-" + std::to_wstring(pid) + L"-" + kind + L".log";
    DWORD error = ERROR_SUCCESS;
    {
        // Append only: a failed/partial local write must never truncate an old log.
        Handle file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE) return {};
        DWORD written{};
        if (!WriteFile(file.get(), line, length, &written, nullptr)) {
            error = GetLastError();
            if (!error) error = ERROR_WRITE_FAULT;
        } else if (written != length) error = ERROR_WRITE_FAULT;
    }
    if (error != ERROR_SUCCESS) { SetLastError(error); return {}; }
    return path;
}

std::wstring WriteReport(const std::wstring& patchDirectory, DWORD pid, const wchar_t* kind,
                         const char* line, DWORD length) {
    if (!patchDirectory.empty()) {
        const std::wstring local = TryWriteReport(patchDirectory + L"\\logs", pid, kind, line, length);
        if (!local.empty()) return local;
    }
    // Retry the entire record after ANY local open/write failure, append only.
    wchar_t temporary[1024]{};
    const DWORD temporaryLength = GetTempPathW(ARRAYSIZE(temporary), temporary);
    if (!temporaryLength) return {};
    if (temporaryLength >= ARRAYSIZE(temporary)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return {}; }
    return TryWriteReport(std::wstring(temporary) + L"LakeWindowedFix", pid, kind, line, length);
}

std::wstring WriteExitReport(const std::wstring& patchDirectory, DWORD pid, DWORD code) {
    char line[384]{};
    const int length = snprintf(line, sizeof(line),
        "pid=%lu tick=%llu code_decimal=%lu code_hex=0x%08lx\r\n"
        "Observed process exit status only; code zero does not prove graphics correctness.\r\n",
        pid, GetTickCount64(), code, code);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(line)) {
        SetLastError(ERROR_INVALID_DATA);
        return {};
    }
    return WriteReport(patchDirectory, pid, L"exit", line, static_cast<DWORD>(length));
}

std::wstring WriteStartupReport(const std::wstring& directory, DWORD pid, const char* stage,
                                const Result& result, bool terminatedChild = false) {
    char exit[96]{};
    if (result.hasExitCode)
        snprintf(exit, sizeof(exit), " code_decimal=%lu code_hex=0x%08lx", result.exitCode, result.exitCode);
    char line[1024]{};
    const int length = snprintf(line, sizeof(line),
        "launcher_pid=%lu pid=%lu tick=%llu stage=%s status=%s win32_decimal=%lu win32_hex=0x%08lx "
        "exit_code_available=%u%s cleanup_win32=%lu launcher_terminated_child=%u\r\n"
        "Startup failure only; no claim about the Lake window or graphics.\r\n",
        GetCurrentProcessId(), pid, GetTickCount64(), stage, lakefix::startup::StateName(result.state),
        result.error, result.error, result.hasExitCode ? 1u : 0u, exit, result.cleanupError, terminatedChild ? 1u : 0u);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(line)) { SetLastError(ERROR_INVALID_DATA); return {}; }
    return WriteReport(directory, pid, L"startup", line, static_cast<DWORD>(length));
}

int StartupError(const std::wstring& directory, DWORD pid, const char* stage, const wchar_t* description,
                 const Result& result, bool terminatedChild = false, int status = 1) {
    fwprintf(stderr, L"%ls (estado=%hs, Win32=%lu / 0x%08lx).\n", description,
             lakefix::startup::StateName(result.state), result.error, result.error);
    if (result.hasExitCode)
        fwprintf(stderr, L"Filho encerrou durante startup: %lu (0x%08lx).\n", result.exitCode, result.exitCode);
    const std::wstring report = WriteStartupReport(directory, pid, stage, result, terminatedChild);
    if (!report.empty()) fwprintf(stderr, L"Registro de startup: %ls\n", report.c_str());
    else fwprintf(stderr, L"Falha ao gravar registro de startup (Win32=%lu).\n", GetLastError());
    SetLastError(result.error);
    return status;
}

#ifdef LAKE_FIX_TESTING
bool CheckExitReport(const std::wstring& path, DWORD code, LONGLONG offset = 0) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) return false;
    char text[4096]{};
    DWORD bytes{};
    if (!ReadFile(file.get(), text, sizeof(text) - 1, &bytes, nullptr)) return false;
    char expected[64]{};
    snprintf(expected, sizeof(expected), "code_decimal=%lu code_hex=0x%08lx", code, code);
    return strstr(text, expected) != nullptr;
}

bool CheckExitReportFallback() {
    wchar_t temporary[1024]{};
    const DWORD length = GetTempPathW(ARRAYSIZE(temporary), temporary);
    if (!length || length >= ARRAYSIZE(temporary)) return false;
    const DWORD pid = GetCurrentProcessId();
    const std::wstring name = L"\\LakeWindowedFix-" + std::to_wstring(pid) + L"-exit.log";
    const std::wstring fallback = std::wstring(temporary) + L"LakeWindowedFix" + name;
    LARGE_INTEGER before{};
    {
        // The fallback may predate this test. Check only the newly appended
        // record, and never remove/truncate that shared-directory log.
        Handle existing(CreateFileW(fallback.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (existing.get() != INVALID_HANDLE_VALUE) {
            if (!GetFileSizeEx(existing.get(), &before)) return false;
        } else {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        }
    }
    const std::wstring prefix = std::wstring(temporary) + L"LakeWindowedFix-report-test-" +
        std::to_wstring(pid) + L"-" + std::to_wstring(GetTickCount64()) + L"-";
    std::wstring directory;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const std::wstring candidate = prefix + std::to_wstring(attempt);
        if (CreateDirectoryW(candidate.c_str(), nullptr)) { directory = candidate; break; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    if (directory.empty()) return false;
    const std::wstring logs = directory + L"\\logs";
    const std::wstring local = logs + name;
    const bool logsCreated = CreateDirectoryW(logs.c_str(), nullptr) != FALSE;
    bool passed = false;
    if (logsCreated) {
        // CREATE_NEW proves ownership; DELETE_ON_CLOSE removes only this file.
        // No FILE_SHARE_WRITE: the local directory exists, but append is denied.
        Handle blocked(CreateFileW(local.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
        if (blocked.get() != INVALID_HANDLE_VALUE) {
            constexpr char marker[] = "test-owned local log; preserve contents\r\n";
            constexpr DWORD markerBytes = static_cast<DWORD>(sizeof(marker) - 1);
            DWORD written{};
            if (WriteFile(blocked.get(), marker, markerBytes, &written, nullptr) && written == markerBytes) {
                // Formatting input only: no process is faulted or exited here.
                constexpr DWORD code = 0x87654321;
                const std::wstring report = WriteExitReport(directory, pid, code);
                LARGE_INTEGER size{}, beginning{};
                char unchanged[sizeof(marker)]{};
                DWORD read{};
                const bool preserved = GetFileSizeEx(blocked.get(), &size) && size.QuadPart == static_cast<LONGLONG>(markerBytes) &&
                    SetFilePointerEx(blocked.get(), beginning, nullptr, FILE_BEGIN) &&
                    ReadFile(blocked.get(), unchanged, markerBytes, &read, nullptr) &&
                    read == markerBytes && memcmp(unchanged, marker, markerBytes) == 0;
                passed = report == fallback && CheckExitReport(report, code, before.QuadPart) && preserved;
            }
        }
    }
    // Nonrecursive cleanup of directories created above; never touch fallback.
    const bool logsRemoved = !logsCreated || RemoveDirectoryW(logs.c_str()) != FALSE;
    const bool directoryRemoved = RemoveDirectoryW(directory.c_str()) != FALSE;
    passed = passed && logsRemoved && directoryRemoved;
    wprintf(L"%ls: fallback TEMP com log local sem compartilhamento de escrita; codigo decimal/hex preservado\n",
            passed ? L"PASS" : L"FAIL");
    if (!passed) SetLastError(ERROR_INVALID_DATA);
    return passed;
}
#endif

bool HashFile(HANDLE file, BYTE (&digest)[32]) {
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) return false;
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    DWORD error = ERROR_INVALID_DATA;
    BYTE buffer[65536];
    DWORD bytes = 0;
    while (ok) {
        if (!ReadFile(file, buffer, sizeof(buffer), &bytes, nullptr)) { error = GetLastError(); ok = false; break; }
        if (!bytes) break;
        ok = BCryptHashData(hash, buffer, bytes, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) SetLastError(error);
    return ok;
}

bool DigestMatches(const BYTE (&digest)[32], const wchar_t* expected) {
    wchar_t hex[65]{};
    for (size_t i = 0; i < sizeof(digest); ++i) swprintf_s(hex + i * 2, 3, L"%02x", digest[i]);
    if (_wcsicmp(hex, expected) == 0) return true;
    SetLastError(ERROR_INVALID_DATA);
    return false;
}

std::wstring FinalPath(HANDLE file) {
    std::vector<wchar_t> path(32768);
    const DWORD size = GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()),
                                               FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!size) return {};
    if (size >= path.size()) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return {}; }
    return std::wstring(path.data(), size);
}

lakefix::ComponentFileIdentity FileIdentity(const BY_HANDLE_FILE_INFORMATION& info) {
    return {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
            info.nFileSizeHigh, info.nFileSizeLow, info.ftLastWriteTime.dwHighDateTime,
            info.ftLastWriteTime.dwLowDateTime};
}

bool SameFile(const lakefix::ComponentFileIdentity& a, const lakefix::ComponentFileIdentity& b) {
    return memcmp(&a, &b, sizeof(a)) == 0;
}

class VerifiedFile {
public:
    ~VerifiedFile() { Release(); }
    void Release() {
        const DWORD error = GetLastError();
        for (HANDLE directory : directories_) CloseHandle(directory);
        directories_.clear();
        file_.reset();
        SetLastError(error);
    }
    bool Open(const std::wstring& path) {
        // Keep this exact handle alive, not just a momentary hash of a pathname.
        file_.reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file_.valid()) return false;
        path_ = FinalPath(file_.get());
        if (path_.empty()) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file_.get(), &info)) return false;
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { SetLastError(ERROR_DIRECTORY); return false; }
        identity_ = FileIdentity(info);
        // A retained file cannot be renamed, but an unpinned parent could be.
        // Pin the canonical directory chain against deletion/rename too.
        std::wstring parent = Parent(path_);
        size_t rootLength = 6; // \\?\C: (root itself cannot be renamed).
        if (path_.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
            const size_t server = path_.find(L'\\', 8);
            const size_t share = server == std::wstring::npos ? server : path_.find(L'\\', server + 1);
            if (share == std::wstring::npos) { SetLastError(ERROR_BAD_PATHNAME); return false; }
            rootLength = share;
        }
        while (parent.size() > rootLength) {
            HANDLE directory = CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (directory == INVALID_HANDLE_VALUE) return false;
            directories_.push_back(directory);
            const std::wstring next = Parent(parent);
            if (next == parent) break;
            parent = next;
        }
        // Detect a namespace change while acquiring the directory pins.
        Handle check(CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        BY_HANDLE_FILE_INFORMATION current{};
        if (!check.valid() || !GetFileInformationByHandle(check.get(), &current)) return false;
        if (!SameFile(identity_, FileIdentity(current))) { SetLastError(ERROR_FILE_INVALID); return false; }
        return true;
    }
    bool Hash() { return HashFile(file_.get(), digest_); }
    bool Matches(const wchar_t* expected) { return Hash() && DigestMatches(digest_, expected); }
    bool IsSameFile(const std::wstring& path) const {
        Handle actual(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!actual.valid() || !GetFileInformationByHandle(actual.get(), &info)) return false;
        const std::wstring actualPath = FinalPath(actual.get());
        if (!actualPath.empty() && _wcsicmp(path_.c_str(), actualPath.c_str()) == 0 && SameFile(identity_, FileIdentity(info)))
            return true;
        SetLastError(ERROR_FILE_INVALID);
        return false;
    }
    const std::wstring& path() const { return path_; }
    const lakefix::ComponentFileIdentity& identity() const { return identity_; }
    const BYTE (&digest() const)[32] { return digest_; }
private:
    Handle file_;
    std::vector<HANDLE> directories_;
    std::wstring path_;
    lakefix::ComponentFileIdentity identity_{};
    BYTE digest_[32]{};
};

[[maybe_unused]] bool HashMatches(const std::wstring& path, const wchar_t* expected) {
    VerifiedFile file;
    return file.Open(path) && file.Matches(expected);
}

#ifdef LAKE_FIX_TESTING
bool CheckHashImplementation(const std::wstring& directory) {
    wchar_t temporary[32768]{};
    if (!GetTempFileNameW(directory.c_str(), L"lfx", 0, temporary)) return false;
    bool written = false;
    {
        Handle file(CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD bytes{};
        written = file.get() != INVALID_HANDLE_VALUE && WriteFile(file.get(), "abc", 3, &bytes, nullptr) && bytes == 3;
    }
    const bool matched = written && HashMatches(temporary, L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const bool rejected = !HashMatches(temporary, L"0000000000000000000000000000000000000000000000000000000000000000");
    DeleteFileW(temporary);
    wprintf(L"%ls: SHA-256 conhecido e hash incorreto\n", matched && rejected ? L"PASS" : L"FAIL");
    return matched && rejected;
}
#endif

class EntryBreakpoint {
public:
    bool Arm(HANDLE process, void* address) {
        process_ = process;
        address_ = address;
        SIZE_T bytes{};
        if (!ReadProcessMemory(process_, address_, &original_, 1, &bytes)) return false;
        if (bytes != 1 || original_ == 0xcc) { SetLastError(ERROR_INVALID_DATA); return false; }
        if (!VirtualProtectEx(process_, address_, 1, PAGE_EXECUTE_READWRITE, &protection_)) return false;
        active_ = true; // Includes a partially written byte or failed protection restore.
        return Write(0xcc);
    }
    bool Restore() {
        if (!active_) return true;
        DWORD ignored{};
        if (!VirtualProtectEx(process_, address_, 1, PAGE_EXECUTE_READWRITE, &ignored)) return false;
        const bool restored = Write(original_);
        if (restored) active_ = false;
        return restored;
    }
    void* address() const { return address_; }
    bool active() const { return active_; }
private:
    bool Write(BYTE value) {
        DWORD error = ERROR_SUCCESS;
        SIZE_T bytes{};
        if (!WriteProcessMemory(process_, address_, &value, 1, &bytes)) error = GetLastError();
        else if (bytes != 1) error = ERROR_PARTIAL_COPY;
        if (!FlushInstructionCache(process_, address_, 1) && !error) error = GetLastError();
        DWORD ignored{};
        // Always use the FIRST protection, including retries after failed arm.
        if (!VirtualProtectEx(process_, address_, 1, protection_, &ignored) && !error) error = GetLastError();
        if (error) SetLastError(error);
        return error == ERROR_SUCCESS;
    }
    HANDLE process_ = nullptr;
    void* address_ = nullptr;
    BYTE original_ = 0;
    DWORD protection_ = 0;
    bool active_ = false;
};

bool RewindEntry(HANDLE thread, void* entry) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(thread, &context)) return false;
    const DWORD address = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(entry));
    if (context.Eip != address + 1 && context.Eip != address) { SetLastError(ERROR_INVALID_ADDRESS); return false; }
    // Preserve EFlags, ESP, EBP and every other member from GetThreadContext.
    context.Eip = address;
    return SetThreadContext(thread, &context) != FALSE;
}

Result StopBeforeEntry(PROCESS_INFORMATION& process, DWORD timeout = kTimeout,
                       decltype(&WaitForDebugEvent) wait = &WaitForDebugEvent) {
    // The first loader breakpoint may still own the loader lock. A temporary
    // INT3 at the PE entry point stops AFTER that lock is released. Its exact
    // original byte and EIP are restored before detaching/injecting. No file is
    // changed, and no compatibility patch remains in executable instructions.
    const ULONGLONG deadline = GetTickCount64() + timeout;
    EntryBreakpoint patch;
    DEBUG_EVENT event{};
    bool pending = false;
    bool atEntry = false;
    bool suspended = false;
    DWORD continuation = DBG_CONTINUE;
    auto fail = [&](Result result) {
        auto cleanup = [&](bool ok) {
            if (!ok && !result.cleanupError) result.cleanupError = GetLastError();
        };
        // Only called with handles obtained by THIS invocation's CreateProcess.
        // Never resume a failed child past a damaged breakpoint/context.
        if (!suspended) {
            const DWORD previous = SuspendThread(process.hThread);
            if (previous == static_cast<DWORD>(-1)) cleanup(false);
            else suspended = true;
        }
        cleanup(patch.Restore());
        if (atEntry) cleanup(RewindEntry(process.hThread, patch.address()));
        if (pending) cleanup(ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation) != FALSE);
        cleanup(DebugActiveProcessStop(process.dwProcessId) != FALSE);
        SetLastError(result.error);
        return result;
    };
    while (true) {
        const ULONGLONG now = GetTickCount64();
        const bool expired = now >= deadline;
        // Even at the deadline, inspect an already queued EXIT event before
        // declaring timeout. Never erase authoritative exit evidence by clock.
        if (!wait(&event, expired ? 0 : static_cast<DWORD>(deadline - now))) {
            const DWORD error = GetLastError(); // Capture BEFORE any cleanup API.
            return fail(lakefix::startup::DebugWaitFailure(error));
        }
        pending = true;
        atEntry = false;
        continuation = DBG_CONTINUE;
        if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            // The event is authoritative even for code zero or STILL_ACTIVE.
            // Do not write/suspend a process whose DLLs are already unloaded.
            Result result = lakefix::startup::Exited(event.u.ExitProcess.dwExitCode);
            if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE))
                result.cleanupError = GetLastError();
            return result;
        }
        if (expired) {
            if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT && event.u.CreateProcessInfo.hFile)
                CloseHandle(event.u.CreateProcessInfo.hFile);
            if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile)
                CloseHandle(event.u.LoadDll.hFile);
            if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT)
                continuation = DBG_EXCEPTION_NOT_HANDLED;
            return fail(lakefix::startup::Timeout());
        }
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            BYTE* base = static_cast<BYTE*>(event.u.CreateProcessInfo.lpBaseOfImage);
            IMAGE_DOS_HEADER dos{};
            IMAGE_NT_HEADERS32 nt{};
            SIZE_T bytes{};
            if (ReadProcessMemory(process.hProcess, base, &dos, sizeof(dos), &bytes) && bytes == sizeof(dos) &&
                dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && dos.e_lfanew < 1048576 &&
                ReadProcessMemory(process.hProcess, base + dos.e_lfanew, &nt, sizeof(nt), &bytes) &&
                bytes == sizeof(nt) && nt.Signature == IMAGE_NT_SIGNATURE &&
                nt.FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
                nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
                nt.OptionalHeader.AddressOfEntryPoint &&
                nt.OptionalHeader.AddressOfEntryPoint < nt.OptionalHeader.SizeOfImage) {
                if (!patch.Arm(process.hProcess, base + nt.OptionalHeader.AddressOfEntryPoint))
                    return fail(lakefix::startup::Failure(GetLastError()));
            } else return fail(lakefix::startup::Failure(ERROR_BAD_EXE_FORMAT));
        }
        if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile)
            CloseHandle(event.u.LoadDll.hFile);
        const bool exception = event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT;
        const DWORD code = exception ? event.u.Exception.ExceptionRecord.ExceptionCode : 0;
        // WOW64 can expose STATUS_WX86_BREAKPOINT as well as BREAKPOINT.
        const bool breakpoint = exception && (code == EXCEPTION_BREAKPOINT || code == 0x4000001f);
        if (patch.active() && breakpoint && event.dwThreadId == process.dwThreadId &&
            event.u.Exception.ExceptionRecord.ExceptionAddress == patch.address()) {
            atEntry = true;
            if (!patch.Restore() || !RewindEntry(process.hThread, patch.address()))
                return fail(lakefix::startup::Failure(GetLastError()));
            const DWORD previous = SuspendThread(process.hThread);
            suspended = previous != static_cast<DWORD>(-1);
            if (previous != 0)
                return fail(lakefix::startup::Failure(previous == static_cast<DWORD>(-1) ? GetLastError() : ERROR_INVALID_STATE));
            if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE))
                return fail(lakefix::startup::Failure(GetLastError()));
            pending = false;
            atEntry = false;
            if (!DebugActiveProcessStop(process.dwProcessId)) return fail(lakefix::startup::Failure(GetLastError()));
            return lakefix::startup::Ready();
        }
        continuation = exception && !breakpoint ? DBG_EXCEPTION_NOT_HANDLED : DBG_CONTINUE;
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation))
            return fail(lakefix::startup::Failure(GetLastError()));
        pending = false;
    }
}

LPTHREAD_START_ROUTINE RemoteSystemFunction(DWORD pid, const char* name) {
    FARPROC address = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
    if (!address) return nullptr;
    HMODULE owner{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &owner)) return nullptr;
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(owner, path, ARRAYSIZE(path))) return nullptr;
    const wchar_t* baseName = wcsrchr(path, L'\\');
    baseName = baseName ? baseName + 1 : path;
    Handle snapshot(CreateSnapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (snapshot.get() == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (!Module32FirstW(snapshot.get(), &module)) return nullptr;
    do {
        if (_wcsicmp(module.szModule, baseName) == 0) {
            const ULONG_PTR offset = reinterpret_cast<ULONG_PTR>(address) - reinterpret_cast<ULONG_PTR>(owner);
            if (offset >= module.modBaseSize) { SetLastError(ERROR_INVALID_ADDRESS); return nullptr; }
            return reinterpret_cast<LPTHREAD_START_ROUTINE>(module.modBaseAddr + offset);
        }
    } while (Module32NextW(snapshot.get(), &module));
    if (GetLastError() == ERROR_NO_MORE_FILES) SetLastError(ERROR_MOD_NOT_FOUND);
    return nullptr;
}

bool RemoteCall(HANDLE process, LPTHREAD_START_ROUTINE function, void* argument, DWORD& result) {
    if (!function) { SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    Handle thread(CreateRemoteThread(process, nullptr, 0, function, argument, 0, nullptr));
    if (!thread.get()) return false;
    const DWORD wait = WaitForSingleObject(thread.get(), kTimeout);
    if (wait != WAIT_OBJECT_0) {
        if (wait != WAIT_FAILED) SetLastError(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_GEN_FAILURE);
        return false;
    }
    return GetExitCodeThread(thread.get(), &result) != FALSE;
}

struct ComponentExports {
    DWORD initialize = 0;
    DWORD query = 0;
    DWORD imageBytes = 0;
};

bool InspectComponent(const VerifiedFile& dll, ComponentExports& exports) {
    if (!lakefix::ComponentIdentityConfigured()) { SetLastError(ERROR_REVISION_MISMATCH); return false; }
    // Map our own DLL only as an image to locate its export RVA; don't execute
    // its DllMain or load its dependencies in the launcher.
    HMODULE image = LoadLibraryExW(dll.path().c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!image) return false;
    wchar_t path[32768]{};
    bool ok = false;
    DWORD error = ERROR_BAD_EXE_FORMAT;
    if (GetModuleFileNameW(image, path, ARRAYSIZE(path)) && dll.IsSameFile(path)) {
        const auto* base = reinterpret_cast<const BYTE*>(image);
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS32 nt{};
        SIZE_T bytes{};
        if (ReadProcessMemory(GetCurrentProcess(), base, &dos, sizeof(dos), &bytes) && bytes == sizeof(dos) &&
            dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && dos.e_lfanew < 1048576 &&
            ReadProcessMemory(GetCurrentProcess(), base + dos.e_lfanew, &nt, sizeof(nt), &bytes) && bytes == sizeof(nt) &&
            nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
            nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            FARPROC initialize = GetProcAddress(image, lakefix::kInitializeExport);
            FARPROC query = GetProcAddress(image, lakefix::kQueryContractExport);
            const ULONG_PTR initRva = reinterpret_cast<ULONG_PTR>(initialize) - reinterpret_cast<ULONG_PTR>(image);
            const ULONG_PTR queryRva = reinterpret_cast<ULONG_PTR>(query) - reinterpret_cast<ULONG_PTR>(image);
            exports.imageBytes = nt.OptionalHeader.SizeOfImage;
            // Reject missing AND forwarded exports (outside the retained image).
            if (initialize && query && initRva && initRva < exports.imageBytes && queryRva && queryRva < exports.imageBytes) {
                exports.initialize = static_cast<DWORD>(initRva);
                exports.query = static_cast<DWORD>(queryRva);
                ok = true;
            } else error = ERROR_PROC_NOT_FOUND;
        }
    } else error = GetLastError();
    FreeLibrary(image);
    if (!ok) SetLastError(error);
    return ok;
}

bool VerifyRemoteComponent(DWORD pid, DWORD base, const VerifiedFile& dll, DWORD expectedBytes) {
    Handle snapshot(CreateSnapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot.valid()) return false;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (!Module32FirstW(snapshot.get(), &module)) return false;
    do {
        if (reinterpret_cast<ULONG_PTR>(module.modBaseAddr) == base) {
            if (module.modBaseSize != expectedBytes) { SetLastError(ERROR_BAD_EXE_FORMAT); return false; }
            return dll.IsSameFile(module.szExePath);
        }
    } while (Module32NextW(snapshot.get(), &module));
    if (GetLastError() == ERROR_NO_MORE_FILES) SetLastError(ERROR_MOD_NOT_FOUND);
    return false;
}

// A timed-out remote call may still be using its argument. Free only after
// the worker is known to have finished; otherwise owned-child teardown reclaims
// it. No handle/buffer in another (pre-existing) process is ever used here.
bool RemoteExchange(HANDLE process, LPTHREAD_START_ROUTINE function, void* data, SIZE_T bytes, DWORD& result) {
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    SIZE_T transferred{};
    const bool wrote = WriteProcessMemory(process, remote, data, bytes, &transferred) != FALSE;
    if (!wrote || transferred != bytes) {
        const DWORD error = !wrote ? GetLastError() : ERROR_PARTIAL_COPY;
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        SetLastError(error);
        return false;
    }
    if (!RemoteCall(process, function, remote, result)) return false;
    const bool read = ReadProcessMemory(process, remote, data, bytes, &transferred) != FALSE;
    const DWORD error = !read ? GetLastError() : transferred == bytes ? ERROR_SUCCESS : ERROR_PARTIAL_COPY;
    const bool freed = VirtualFreeEx(process, remote, 0, MEM_RELEASE) != FALSE;
    if (error) SetLastError(error);
    return !error && freed;
}

bool Inject(PROCESS_INFORMATION& process, const VerifiedFile& dll, const VerifiedFile& contour,
             const ComponentExports& exports) {

    LPTHREAD_START_ROUTINE load = RemoteSystemFunction(process.dwProcessId, "LoadLibraryW");
    if (!load) return false;
    const SIZE_T bytes = (dll.path().size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    SIZE_T written{};
    DWORD module{};
    const bool wrote = WriteProcessMemory(process.hProcess, remote, dll.path().c_str(), bytes, &written) != FALSE;
    if (!wrote || written != bytes) {
        const DWORD error = !wrote ? GetLastError() : ERROR_PARTIAL_COPY;
        VirtualFreeEx(process.hProcess, remote, 0, MEM_RELEASE);
        SetLastError(error);
        return false;
    }
    if (!RemoteCall(process.hProcess, load, remote, module)) {
        // The owned child is terminated by the caller. Never free a buffer
        // that a timed-out loader thread could still be reading.
        return false;
    }
    if (!VirtualFreeEx(process.hProcess, remote, 0, MEM_RELEASE)) return false;
    if (!module) { SetLastError(ERROR_DLL_INIT_FAILED); return false; }
    if (!VerifyRemoteComponent(process.dwProcessId, module, dll, exports.imageBytes)) return false;

    lakefix::ComponentContract actual{};
    actual.size = sizeof(actual);
    DWORD queried = ERROR_INVALID_FUNCTION;
    auto query = reinterpret_cast<LPTHREAD_START_ROUTINE>(static_cast<ULONG_PTR>(module) + exports.query);
    if (!RemoteExchange(process.hProcess, query, &actual, sizeof(actual), queried)) return false;
    if (queried != ERROR_SUCCESS) { SetLastError(queried); return false; }
    if (!lakefix::ComponentContractMatches(actual)) { SetLastError(ERROR_REVISION_MISMATCH); return false; }

    lakefix::InitializeRequest request{};
    request.size = sizeof(request);
    request.abi = lakefix::kComponentAbi;
    request.component = lakefix::ExpectedComponentContract();
    request.contourIdentity = contour.identity();
    memcpy(request.contourSha256, contour.digest(), sizeof(request.contourSha256));
    if (contour.path().size() >= ARRAYSIZE(request.contourPath)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return false; }
    for (size_t i = 0; i < contour.path().size(); ++i) request.contourPath[i] = contour.path()[i];
    DWORD result = ERROR_INVALID_FUNCTION;
    auto init = reinterpret_cast<LPTHREAD_START_ROUTINE>(static_cast<ULONG_PTR>(module) + exports.initialize);
    if (!RemoteExchange(process.hProcess, init, &request, sizeof(request), result)) return false;
    if (result != ERROR_SUCCESS) {
        fwprintf(stderr, L"LakeWindowedInitialize retornou %lu (0x%08lx).\n", result, result);
        SetLastError(result);
        return false;
    }
    return true;
}

bool CleanupOwnedChild(PROCESS_INFORMATION& process, Result& result) {
    // Process handles, never PIDs obtained from enumeration, establish ownership.
    // Preserve natural exit evidence BEFORE requesting termination.
    DWORD wait = WaitForSingleObject(process.hProcess, 0);
    if (wait == WAIT_OBJECT_0 || result.state == State::childExited) {
        if (!result.hasExitCode && wait == WAIT_OBJECT_0) {
            DWORD code{};
            if (GetExitCodeProcess(process.hProcess, &code)) { result.hasExitCode = true; result.exitCode = code; }
            else if (!result.cleanupError) result.cleanupError = GetLastError();
        }
        if (result.state == State::childExited && WaitForSingleObject(process.hProcess, kTimeout) != WAIT_OBJECT_0 && !result.cleanupError)
            result.cleanupError = ERROR_TIMEOUT;
        return false;
    }
    if (wait == WAIT_FAILED && !result.cleanupError) result.cleanupError = GetLastError();
    const bool terminated = TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED) != FALSE;
    if (!terminated && !result.cleanupError) result.cleanupError = GetLastError();
    wait = WaitForSingleObject(process.hProcess, 100);
    if (wait == WAIT_TIMEOUT) {
        // A failed debug detach can leave an exit event awaiting continuation.
        // Detach ONLY this owned child, never an enumerated existing process.
        DebugActiveProcessStop(process.dwProcessId);
        wait = WaitForSingleObject(process.hProcess, kTimeout);
    }
    if (wait != WAIT_OBJECT_0 && !result.cleanupError)
        result.cleanupError = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
    if (!terminated && wait == WAIT_OBJECT_0 && !result.hasExitCode) {
        DWORD code{};
        if (GetExitCodeProcess(process.hProcess, &code)) { result.hasExitCode = true; result.exitCode = code; }
    }
    return terminated;
}

int Error(const wchar_t* stage) {
    fwprintf(stderr, L"%ls (Win32=%lu). Consulte docs/TROUBLESHOOTING.md.\n", stage, GetLastError());
    return 1;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t module[32768]{};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    if (!moduleLength || moduleLength >= ARRAYSIZE(module))
        return StartupError(L"", 0, "launcher-path", L"Caminho do launcher indisponivel",
                            lakefix::startup::Failure(moduleLength ? ERROR_INSUFFICIENT_BUFFER : GetLastError()));
    const std::wstring patchDirectory = Parent(module);
    std::wstring directory = patchDirectory;
    bool nativeOnly = false;
    bool waitForExit = false;
#ifdef LAKE_FIX_TESTING
    int fixtureExit = -1;
#endif
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--app-dir") == 0 && i + 1 < argc) directory = argv[++i];
        else if (wcscmp(argv[i], L"--native-only") == 0) nativeOnly = true;
        else if (wcscmp(argv[i], L"--wait") == 0) waitForExit = true;
#ifdef LAKE_FIX_TESTING
        else if (wcscmp(argv[i], L"--fixture-exit") == 0 && i + 1 < argc) {
            wchar_t* end{};
            const unsigned long value = wcstoul(argv[++i], &end, 10);
            if (!*argv[i] || *end || value > 255) return 2;
            fixtureExit = static_cast<int>(value);
        }
#endif
        else {
            fwprintf(stderr, L"Uso: LakeWindowedLauncher.exe [--app-dir PASTA] [--native-only] [--wait]\n");
            return wcscmp(argv[i], L"--help") == 0 ? 0 : 2;
        }
    }
    directory = FullPath(directory);
    if (directory.empty()) return StartupError(patchDirectory, 0, "app-directory", L"Pasta invalida", lakefix::startup::Failure(GetLastError()));
#ifdef LAKE_FIX_TESTING
    if (!CheckHashImplementation(patchDirectory)) return Error(L"Autoteste SHA-256 falhou");
    if (!CheckExitReportFallback()) return Error(L"Autoteste de fallback do registro de saida falhou");
    DWORD selfPid{};
    const wchar_t* selfName = wcsrchr(module, L'\\');
    if (!FindRunningImage(selfName ? selfName + 1 : module, selfPid) || !selfPid)
        return Error(L"Autoteste de descoberta de processo falhou");
    wprintf(L"PASS: descoberta de instancia existente sem abrir memoria do processo\n");
    const std::wstring executable = directory + L"\\LakeFixFixture.exe";
    constexpr wchar_t imageName[] = L"LakeFixFixture.exe";
#else
    const std::wstring executable = directory + L"\\Lake Controller.exe";
    constexpr wchar_t imageName[] = L"Lake Controller.exe";
#endif
    const std::wstring mutexName = StartupMutexName();
    if (mutexName.empty()) return StartupError(patchDirectory, 0, "startup-mutex-identity", L"Identidade do usuario indisponivel", lakefix::startup::Failure(GetLastError()));
    StartupMutex gate;
    if (!gate.Acquire(mutexName))
        return StartupError(patchDirectory, 0, "startup-mutex", L"Nao foi possivel serializar o startup nesta sessao", lakefix::startup::Failure(GetLastError()));
    // Gate spans enumeration, verification, CreateProcess, handshake and init.
    // Abandoned mutexes still take this EXACT enumeration path.
    DWORD running{};
    if (!FindRunningImage(imageName, running))
        return StartupError(patchDirectory, 0, "existing-process-snapshot", L"Nao foi possivel verificar instancias existentes", lakefix::startup::Failure(GetLastError()));
    if (lakefix::startup::ExistingImageBlocks(running)) {
        fwprintf(stderr, L"Ja existe uma instancia aberta (PID %lu), possivelmente oculta.\n"
                        L"Feche essa instancia antes de tentar novamente. Nenhum novo processo iniciado.\n", running);
        return StartupError(patchDirectory, 0, "existing-process", L"Nova instancia recusada", lakefix::startup::Failure(ERROR_ALREADY_EXISTS), false, 4);
    }
    VerifiedFile executableFile, contourFile, componentFile;
    if (!executableFile.Open(executable))
        return StartupError(patchDirectory, 0, "verified-input-open", L"Arquivos de entrada indisponiveis; nenhum processo iniciado", lakefix::startup::Failure(GetLastError()), false, 3);
    // Resolve CWD and Contour from the retained executable, not a mutable
    // --app-dir junction alias that could change after verification.
    directory = Parent(executableFile.path());
    if (!contourFile.Open(directory + L"\\Contour.dll"))
        return StartupError(patchDirectory, 0, "verified-input-open", L"Contour indisponivel; nenhum processo iniciado", lakefix::startup::Failure(GetLastError()), false, 3);
#ifdef LAKE_FIX_TESTING
    // Compile-time fixture executable only. No runtime switch weakens the
    // production allowlist; every retention/hash/handshake policy is shared.
    if (!contourFile.Hash())
#else
    if (!executableFile.Matches(kExeHash) || !contourFile.Matches(kContourHash))
#endif
        return StartupError(patchDirectory, 0, "verified-input-hash", L"Build nao reconhecido; nenhum processo iniciado", lakefix::startup::Failure(GetLastError()), false, 3);
    ComponentExports exports{};
    if (!nativeOnly && (!componentFile.Open(patchDirectory + L"\\LakeWindowedFix.dll") || !InspectComponent(componentFile, exports)))
        return StartupError(patchDirectory, 0, "component-preflight", L"DLL indisponivel ou sem contrato/build compativel", lakefix::startup::Failure(GetLastError()));
    std::wstring command = L"\"" + executableFile.path() + L"\" windowed nonfullscreen";
#ifdef LAKE_FIX_TESTING
    if (fixtureExit >= 0) command += L" --exit-code " + std::to_wstring(fixtureExit);
#endif
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    DWORD flags = nativeOnly ? CREATE_SUSPENDED : DEBUG_ONLY_THIS_PROCESS;
    if (!CreateProcessW(executableFile.path().c_str(), command.data(), nullptr, nullptr, FALSE, flags,
                         nullptr, directory.c_str(), &startup, &process))
        return StartupError(patchDirectory, 0, "create-process", L"CreateProcess falhou", lakefix::startup::Failure(GetLastError()));
    Handle processHandle(process.hProcess);
    Handle threadHandle(process.hThread);
    Result result = nativeOnly ? lakefix::startup::Ready() : StopBeforeEntry(process);
    const char* stage = "before-entry";
    const wchar_t* description = L"Pausa antes de WinMain falhou";
    if (result.state == State::ready && !nativeOnly && !Inject(process, componentFile, contourFile, exports)) {
        result = lakefix::startup::Failure(GetLastError());
        stage = "component-load-handshake-init";
        description = L"Carregamento/contrato/inicializacao da DLL falhou";
    }
    if (result.state == State::ready) {
        const DWORD previous = ResumeThread(process.hThread);
        if (previous != 1) {
            result = lakefix::startup::Failure(previous == static_cast<DWORD>(-1) ? GetLastError() : ERROR_INVALID_STATE);
            stage = "resume-main-thread";
            description = L"Contagem de suspensao inesperada em ResumeThread";
        }
    }
    if (result.state != State::ready) {
        const bool terminated = CleanupOwnedChild(process, result);
        gate.Release();
        return StartupError(patchDirectory, process.dwProcessId, stage, description, result, terminated);
    }
    // The DLL now owns its own retained Contour identity for subsequent loads.
    componentFile.Release();
    contourFile.Release();
    executableFile.Release();
    gate.Release(); // Never hold a startup gate during optional lifetime wait.
#ifdef LAKE_FIX_TESTING
    (void)waitForExit;
    if (WaitForSingleObject(process.hProcess, kTimeout) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, kTimeout);
        return Error(L"Fixture excedeu o limite");
    }
    DWORD exitCode;
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) return Error(L"GetExitCodeProcess falhou");
    const std::wstring report = WriteExitReport(patchDirectory, process.dwProcessId, exitCode);
    const bool recorded = !report.empty() && CheckExitReport(report, exitCode);
    wprintf(L"%ls: arquivo registra codigo de saida decimal/hexadecimal\n", recorded ? L"PASS" : L"FAIL");
    if (!recorded) return 1;
    if (fixtureExit >= 0) {
        const bool expected = exitCode == static_cast<DWORD>(fixtureExit);
        wprintf(L"%ls: saida nao-zero da fixture observada (%lu)\n", expected ? L"PASS" : L"FAIL", exitCode);
        return expected ? 0 : 1;
    }
    return static_cast<int>(exitCode);
#else
    if (nativeOnly) wprintf(L"Processo criado (PID %lu), flags nativas sem hooks.\n", process.dwProcessId);
    else {
        wprintf(L"Processo criado (PID %lu); DLL %hs inicializada (build %hs). Isso ainda nao confirma que a janela principal abriu.\n",
                 process.dwProcessId, LAKE_FIX_VERSION, LAKE_FIX_COMPONENT_BUILD_ID);
        wprintf(L"Log esperado: %ls\\logs\\LakeWindowedFix-%lu.log\n", patchDirectory.c_str(), process.dwProcessId);
    }
    wprintf(L"Arquivos originais nao alterados.\n");
    if (waitForExit) {
        wprintf(L"Aguardando o encerramento do Lake para registrar a saida. Feche o aplicativo normalmente ao terminar.\n");
        fflush(stdout);
        if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0)
            return Error(L"Espera pelo encerramento falhou");
        DWORD result{};
        if (!GetExitCodeProcess(process.hProcess, &result)) return Error(L"GetExitCodeProcess falhou");
        wprintf(L"Lake encerrou: codigo %lu (0x%08lx).\n", result, result);
        const std::wstring report = WriteExitReport(patchDirectory, process.dwProcessId, result);
        if (!report.empty()) wprintf(L"Registro de saida: %ls\n", report.c_str());
        else fwprintf(stderr, L"Nao foi possivel gravar o registro de saida (Win32=%lu).\n", GetLastError());
        return static_cast<int>(result);
    }
    return 0;
#endif
}
