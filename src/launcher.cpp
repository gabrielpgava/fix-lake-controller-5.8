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

static_assert(sizeof(void*) == 4, "Build the launcher and DLL for x86, not x64");

namespace {
constexpr DWORD kTimeout = 15000;
#ifndef LAKE_FIX_TESTING
constexpr wchar_t kExeHash[] = L"e9c9d291f9c20732904dd6b943737537c242508e546cda2afb1f72dfc2374713";
constexpr wchar_t kContourHash[] = L"bb7d4126a83b775968d49ce52f885b588561838d97ffe062ec46a8208b94c283";
#endif

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

std::wstring FullPath(const std::wstring& path) {
    std::vector<wchar_t> buffer(32768);
    DWORD size = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    return size && size < buffer.size() ? std::wstring(buffer.data()) : std::wstring();
}

std::wstring Parent(const std::wstring& path) {
    return path.substr(0, path.find_last_of(L"\\/"));
}

bool FindRunningImage(const wchar_t* name, DWORD& pid) {
    pid = 0;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) return false;
    do {
        if (_wcsicmp(entry.szExeFile, name) == 0) { pid = entry.th32ProcessID; return true; }
    } while (Process32NextW(snapshot.get(), &entry));
    return GetLastError() == ERROR_NO_MORE_FILES;
}

std::wstring TryWriteExitReport(const std::wstring& directory, DWORD pid, const char* line, DWORD length) {
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};
    const std::wstring path = directory + L"\\LakeWindowedFix-" + std::to_wstring(pid) + L"-exit.log";
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
    const std::wstring local = TryWriteExitReport(patchDirectory + L"\\logs", pid, line, static_cast<DWORD>(length));
    if (!local.empty()) return local;
    // An existing directory can still reject CreateFile/WriteFile. Retry the
    // complete, unchanged record in TEMP after any local failure.
    wchar_t temporary[1024]{};
    const DWORD temporaryLength = GetTempPathW(ARRAYSIZE(temporary), temporary);
    if (!temporaryLength) return {};
    if (temporaryLength >= ARRAYSIZE(temporary)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return {}; }
    return TryWriteExitReport(std::wstring(temporary) + L"LakeWindowedFix", pid, line, static_cast<DWORD>(length));
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

bool HashMatches(const std::wstring& path, const wchar_t* expected) {
    // Deny concurrent writers/deletion while calculating identity.
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    BYTE buffer[65536];
    DWORD bytes = 0;
    while (ok) {
        if (!ReadFile(file.get(), buffer, sizeof(buffer), &bytes, nullptr)) { ok = false; break; }
        if (!bytes) break;
        ok = BCryptHashData(hash, buffer, bytes, 0) >= 0;
    }
    BYTE digest[32]{};
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    wchar_t hex[65]{};
    for (size_t i = 0; i < sizeof(digest); ++i) swprintf_s(hex + i * 2, 3, L"%02x", digest[i]);
    return ok && _wcsicmp(hex, expected) == 0;
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

bool WriteBreakpointByte(HANDLE process, void* address, BYTE value) {
    DWORD protection{};
    if (!VirtualProtectEx(process, address, 1, PAGE_EXECUTE_READWRITE, &protection)) return false;
    SIZE_T bytes{};
    const bool written = WriteProcessMemory(process, address, &value, 1, &bytes) && bytes == 1;
    const bool flushed = FlushInstructionCache(process, address, 1) != FALSE;
    DWORD ignored{};
    const bool restored = VirtualProtectEx(process, address, 1, protection, &ignored) != FALSE;
    return written && flushed && restored;
}

bool StopBeforeEntry(PROCESS_INFORMATION& process) {
    // The first loader breakpoint may still own the loader lock. A temporary
    // INT3 at the PE entry point stops AFTER that lock is released. Its exact
    // original byte and EIP are restored before detaching/injecting. No file is
    // changed, and no compatibility patch remains in executable instructions.
    const ULONGLONG deadline = GetTickCount64() + kTimeout;
    void* entry = nullptr;
    BYTE original{};
    bool armed = false;
    while (true) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, static_cast<DWORD>(deadline - now))) break;
        bool failed = false;
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
                entry = base + nt.OptionalHeader.AddressOfEntryPoint;
                if (ReadProcessMemory(process.hProcess, entry, &original, 1, &bytes) && bytes == 1) {
                    armed = true; // Also restore on a partially failed write.
                    failed = !WriteBreakpointByte(process.hProcess, entry, 0xcc);
                } else failed = true;
            } else failed = true;
        }
        if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile)
            CloseHandle(event.u.LoadDll.hFile);
        const bool exception = event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT;
        const DWORD code = exception ? event.u.Exception.ExceptionRecord.ExceptionCode : 0;
        // WOW64 can expose STATUS_WX86_BREAKPOINT as well as BREAKPOINT.
        const bool breakpoint = exception && (code == EXCEPTION_BREAKPOINT || code == 0x4000001f);
        if (armed && breakpoint && event.dwThreadId == process.dwThreadId &&
            event.u.Exception.ExceptionRecord.ExceptionAddress == entry) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            const bool read = GetThreadContext(process.hThread, &context) != FALSE;
            const bool restored = WriteBreakpointByte(process.hProcess, entry, original);
            armed = !restored;
            context.Eip = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(entry));
            const bool rewound = read && SetThreadContext(process.hThread, &context);
            const bool stopped = SuspendThread(process.hThread) == 0;
            const bool continued = ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE) != FALSE;
            const bool detached = DebugActiveProcessStop(process.dwProcessId) != FALSE;
            return restored && rewound && stopped && continued && detached;
        }
        if (failed) {
            SuspendThread(process.hThread);
            if (armed) WriteBreakpointByte(process.hProcess, entry, original);
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
            DebugActiveProcessStop(process.dwProcessId);
            SetLastError(ERROR_INVALID_FUNCTION);
            return false;
        }
        const bool exited = event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT;
        const DWORD status = exception && !breakpoint ? DBG_EXCEPTION_NOT_HANDLED : DBG_CONTINUE;
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status) || exited) break;
    }
    SuspendThread(process.hThread);
    if (armed) WriteBreakpointByte(process.hProcess, entry, original);
    DebugActiveProcessStop(process.dwProcessId);
    SetLastError(ERROR_TIMEOUT);
    return false;
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
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (snapshot.get() == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (!Module32FirstW(snapshot.get(), &module)) return nullptr;
    do {
        if (_wcsicmp(module.szModule, baseName) == 0) {
            const ULONG_PTR offset = reinterpret_cast<ULONG_PTR>(address) - reinterpret_cast<ULONG_PTR>(owner);
            if (offset >= module.modBaseSize) return nullptr;
            return reinterpret_cast<LPTHREAD_START_ROUTINE>(module.modBaseAddr + offset);
        }
    } while (Module32NextW(snapshot.get(), &module));
    return nullptr;
}

bool RemoteCall(HANDLE process, LPTHREAD_START_ROUTINE function, void* argument, DWORD& result) {
    if (!function) return false;
    Handle thread(CreateRemoteThread(process, nullptr, 0, function, argument, 0, nullptr));
    if (!thread.get()) return false;
    const DWORD wait = WaitForSingleObject(thread.get(), kTimeout);
    if (wait != WAIT_OBJECT_0) { SetLastError(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_GEN_FAILURE); return false; }
    return GetExitCodeThread(thread.get(), &result) != FALSE;
}

bool Inject(PROCESS_INFORMATION& process, const std::wstring& dll) {
    // Map our own DLL only as an image to locate its export RVA; don't execute
    // its DllMain or load its dependencies in the launcher.
    HMODULE image = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!image) return false;
    FARPROC initialize = GetProcAddress(image, "LakeWindowedInitialize");
    const ULONG_PTR rva = initialize
        ? reinterpret_cast<ULONG_PTR>(initialize) - reinterpret_cast<ULONG_PTR>(image) : 0;
    FreeLibrary(image);
    if (!rva) { SetLastError(ERROR_PROC_NOT_FOUND); return false; }

    LPTHREAD_START_ROUTINE load = RemoteSystemFunction(process.dwProcessId, "LoadLibraryW");
    if (!load) { SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    SIZE_T written{};
    DWORD module{};
    if (!WriteProcessMemory(process.hProcess, remote, dll.c_str(), bytes, &written) || written != bytes) {
        VirtualFreeEx(process.hProcess, remote, 0, MEM_RELEASE);
        return false;
    }
    if (!RemoteCall(process.hProcess, load, remote, module)) {
        // The owned child is terminated by the caller. Never free a buffer
        // that a timed-out loader thread could still be reading.
        return false;
    }
    VirtualFreeEx(process.hProcess, remote, 0, MEM_RELEASE);
    if (!module) { SetLastError(ERROR_DLL_INIT_FAILED); return false; }
    DWORD result = ERROR_INVALID_FUNCTION;
    auto init = reinterpret_cast<LPTHREAD_START_ROUTINE>(static_cast<ULONG_PTR>(module) + rva);
    if (!RemoteCall(process.hProcess, init, nullptr, result)) return false;
    if (result != ERROR_SUCCESS) {
        fwprintf(stderr, L"LakeWindowedInitialize retornou %lu (0x%08lx).\n", result, result);
        SetLastError(result);
        return false;
    }
    return true;
}

int Error(const wchar_t* stage) {
    fwprintf(stderr, L"%ls (Win32=%lu). Consulte docs/TROUBLESHOOTING.md.\n", stage, GetLastError());
    return 1;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t module[32768]{};
    if (!GetModuleFileNameW(nullptr, module, ARRAYSIZE(module))) return Error(L"Caminho do launcher indisponivel");
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
    if (directory.empty()) return Error(L"Pasta invalida");
#ifdef LAKE_FIX_TESTING
    if (!CheckHashImplementation(patchDirectory)) return Error(L"Autoteste SHA-256 falhou");
    if (!CheckExitReportFallback()) return Error(L"Autoteste de fallback do registro de saida falhou");
    DWORD selfPid{};
    const wchar_t* selfName = wcsrchr(module, L'\\');
    if (!FindRunningImage(selfName ? selfName + 1 : module, selfPid) || !selfPid)
        return Error(L"Autoteste de descoberta de processo falhou");
    wprintf(L"PASS: descoberta de instancia existente sem abrir memoria do processo\n");
    const std::wstring executable = directory + L"\\LakeFixFixture.exe";
#else
    const std::wstring executable = directory + L"\\Lake Controller.exe";
    if (!HashMatches(executable, kExeHash) || !HashMatches(directory + L"\\Contour.dll", kContourHash)) {
        fwprintf(stderr, L"Build nao reconhecido ou arquivo indisponivel. Nenhum processo iniciado.\n");
        return 3;
    }
    DWORD running{};
    if (!FindRunningImage(L"Lake Controller.exe", running)) return Error(L"Nao foi possivel verificar instancias existentes");
    if (running) {
        fwprintf(stderr, L"Ja existe um Lake Controller aberto (PID %lu), possivelmente oculto.\n"
                        L"Feche essa instancia antes de tentar novamente. Nenhum novo processo iniciado.\n", running);
        return 4;
    }
#endif
    const std::wstring dll = patchDirectory + L"\\LakeWindowedFix.dll";
    if (!nativeOnly && GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
        return Error(L"LakeWindowedFix.dll nao encontrada ao lado do launcher");
    std::wstring command = L"\"" + executable + L"\" windowed nonfullscreen";
#ifdef LAKE_FIX_TESTING
    if (fixtureExit >= 0) command += L" --exit-code " + std::to_wstring(fixtureExit);
#endif
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    DWORD flags = nativeOnly ? 0 : DEBUG_ONLY_THIS_PROCESS;
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, flags,
                        nullptr, directory.c_str(), &startup, &process))
        return Error(L"CreateProcess falhou");
    Handle processHandle(process.hProcess);
    Handle threadHandle(process.hThread);
    if (!nativeOnly) {
        const wchar_t* failedStage = nullptr;
        if (!StopBeforeEntry(process)) failedStage = L"Pausa antes de WinMain falhou";
        else if (!Inject(process, dll)) failedStage = L"Carregamento/inicializacao da DLL falhou";
        else {
            const DWORD previous = ResumeThread(process.hThread);
            if (previous != 1) {
                if (previous != static_cast<DWORD>(-1)) SetLastError(ERROR_INVALID_STATE);
                failedStage = L"Contagem de suspensao inesperada em ResumeThread";
            }
        }
        if (failedStage) {
            const DWORD error = GetLastError();
            // Only the new child owned by this invocation. Never attach to,
            // kill or modify an already-running Lake instance.
            TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED);
            WaitForSingleObject(process.hProcess, kTimeout);
            SetLastError(error);
            return Error(failedStage);
        }
    }
#ifdef LAKE_FIX_TESTING
    (void)waitForExit;
    if (WaitForSingleObject(process.hProcess, kTimeout) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, kTimeout);
        return Error(L"Fixture excedeu o limite");
    }
    DWORD result;
    if (!GetExitCodeProcess(process.hProcess, &result)) return Error(L"GetExitCodeProcess falhou");
    const std::wstring report = WriteExitReport(patchDirectory, process.dwProcessId, result);
    const bool recorded = !report.empty() && CheckExitReport(report, result);
    wprintf(L"%ls: arquivo registra codigo de saida decimal/hexadecimal\n", recorded ? L"PASS" : L"FAIL");
    if (!recorded) return 1;
    if (fixtureExit >= 0) {
        const bool expected = result == static_cast<DWORD>(fixtureExit);
        wprintf(L"%ls: saida nao-zero da fixture observada (%lu)\n", expected ? L"PASS" : L"FAIL", result);
        return expected ? 0 : 1;
    }
    return static_cast<int>(result);
#else
    if (nativeOnly) wprintf(L"Processo criado (PID %lu), flags nativas sem hooks.\n", process.dwProcessId);
    else {
        wprintf(L"Processo criado (PID %lu); DLL v0.4 inicializada. Isso ainda nao confirma que a janela principal abriu.\n",
                process.dwProcessId);
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
