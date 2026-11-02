#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#include "setup_engine.h"
#include "lake_build_profile.h"
#include <bcrypt.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objidl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <utility>
#include <vector>

static_assert(sizeof(void*) == 4, "The setup must be built for Windows x86");
static_assert(sizeof(wchar_t) == 2, "The manifest uses UTF-16LE");

namespace lakefix::setup {
namespace {
constexpr wchar_t kProject[] = L"LakeWindowedFix";
constexpr wchar_t kDisplayName[] = L"Lake Controller - Janela";
constexpr wchar_t kApplicationName[] = L"Lake Controller.exe";
constexpr wchar_t kRendererName[] = L"Contour.dll";
constexpr std::size_t kPayloadCount = 6;
constexpr std::array<const wchar_t*, kPayloadCount> kNames = {{
    L"LakeWindowedLauncher.exe", L"LakeWindowedFix.dll", L"LakeWindowedFix.ini",
    L"LICENSE", L"Measure-LakeResources.ps1", L"LakeResourceMetrics.psm1"
}};
constexpr std::size_t kConfigurationIndex = 2;

class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); value_ = other.value_; other.value_ = INVALID_HANDLE_VALUE; }
        return *this;
    }
    HANDLE get() const { return value_; }
    bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void reset() {
        if (valid()) CloseHandle(value_);
        value_ = INVALID_HANDLE_VALUE;
    }
private:
    HANDLE value_;
};

template<class T> class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { if (value_) value_->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* get() const { return value_; }
    T** out() { return &value_; }
    T* operator->() const { return value_; }
private:
    T* value_ = nullptr;
};

class Apartment {
public:
    Apartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~Apartment() { if (SUCCEEDED(result_)) CoUninitialize(); }
    // An already initialized MTA belongs to the caller, not to this scope.
    bool ok() const { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }
    DWORD error() const { return static_cast<DWORD>(result_); }
private:
    HRESULT result_;
};

DWORD LastErrorOr(DWORD fallback = ERROR_INVALID_DATA) {
    const DWORD error = GetLastError();
    return error ? error : fallback;
}

bool Error(DWORD error) { SetLastError(error); return false; }

Result Failure(const wchar_t* message, DWORD error = ERROR_INVALID_DATA) {
    Result result;
    result.error = error ? error : ERROR_INVALID_DATA;
    result.message = message;
    SetLastError(result.error);
    return result;
}

Result Success(const wchar_t* message) {
    Result result;
    result.ok = true;
    result.message = message;
    return result;
}

bool EqualPath(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() &&
        CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                             b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool Within(const std::wstring& child, const std::wstring& parent) {
    return child.size() >= parent.size() &&
        CompareStringOrdinal(child.c_str(), static_cast<int>(parent.size()),
                             parent.c_str(), static_cast<int>(parent.size()), TRUE) == CSTR_EQUAL &&
        (child.size() == parent.size() || child[parent.size()] == L'\\');
}

bool Overlap(const std::wstring& a, const std::wstring& b) { return Within(a, b) || Within(b, a); }
std::wstring Join(const std::wstring& directory, const wchar_t* name) { return directory + L"\\" + name; }
std::wstring Parent(const std::wstring& path) {
    const std::size_t at = path.find_last_of(L'\\');
    return at == 2 ? path.substr(0, 3) : path.substr(0, at);
}

bool Missing(DWORD error) { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }

bool ReservedComponent(const std::wstring& part) {
    const std::wstring stem = part.substr(0, part.find(L'.'));
    for (const wchar_t* name : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"})
        if (EqualPath(stem, name)) return true;
    return stem.size() == 4 && (EqualPath(stem.substr(0, 3), L"COM") ||
        EqualPath(stem.substr(0, 3), L"LPT")) && stem[3] >= L'1' && stem[3] <= L'9';
}

// Deliberately accept local, drive-rooted filesystem paths only. Device paths,
// UNC paths, ADS, DOS aliases and lexical traversal are not setup destinations.
bool Normalize(const std::wstring& input, std::wstring& output, bool expandExisting = true) {
    if (input.size() < 4 || input.size() > 32000 || input.find(L'\0') != std::wstring::npos)
        return Error(ERROR_BAD_PATHNAME);
    std::wstring path = input;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (!((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        path[1] != L':' || path[2] != L'\\') return Error(ERROR_BAD_PATHNAME);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    if (path.size() <= 3) return Error(ERROR_BAD_PATHNAME);
    for (std::size_t start = 3; start < path.size();) {
        const std::size_t end = path.find(L'\\', start);
        const std::wstring part = path.substr(start, end == std::wstring::npos ? end : end - start);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' ||
            part.back() == L' ' || ReservedComponent(part)) return Error(ERROR_BAD_PATHNAME);
        for (wchar_t c : part)
            if (c < 32 || std::wcschr(L"<>:\"|?*", c)) return Error(ERROR_BAD_PATHNAME);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (!expandExisting) { output = path; return true; }
    if (GetDriveTypeW(path.substr(0, 3).c_str()) == DRIVE_REMOTE) return Error(ERROR_BAD_NETPATH);

    // Expand existing 8.3 components before making any containment decision.
    // Only the absent suffix may be appended without an on-disk identity.
    std::wstring prefix = path;
    while (GetFileAttributesW(prefix.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (!Missing(error)) return Error(error);
        if (prefix.size() <= 3) return Error(error);
        prefix = Parent(prefix);
    }
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetLongPathNameW(prefix.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) return Error(LastErrorOr(ERROR_FILENAME_EXCED_RANGE));
    output.assign(buffer.data(), length);
    if (prefix.size() < path.size()) {
        if (output.back() == L'\\') output.pop_back();
        output += path.substr(prefix.size() == 3 ? 2 : prefix.size());
    }
    return true;
}

// Inspect every existing ancestor, not merely the last component. Never follow
// a junction/symlink when reading an owned tree or selecting a write location.
bool PathSafe(const std::wstring& path, bool directory, bool allowMissing) {
    std::size_t end = 2;
    while (true) {
        const bool final = end == std::wstring::npos;
        const std::wstring prefix = final ? path : path.substr(0, end == 2 ? 3 : end);
        const DWORD attributes = GetFileAttributesW(prefix.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            return Missing(error) && allowMissing ? true : Error(error);
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return Error(ERROR_REPARSE_TAG_INVALID);
        if ((!final || directory) && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return Error(ERROR_DIRECTORY);
        if (final && !directory && (attributes & FILE_ATTRIBUTE_DIRECTORY)) return Error(ERROR_DIRECTORY);
        if (final) return true;
        end = path.find(L'\\', end == 2 ? 3 : end + 1);
    }
}

bool SameIdentity(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

bool RegularInfo(HANDLE handle, BY_HANDLE_FILE_INFORMATION& info) {
    if (!GetFileInformationByHandle(handle, &info)) return false;
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        return Error(ERROR_REPARSE_TAG_INVALID);
    // A hard-linked owned file is not a private setup-owned filesystem object.
    if (info.nNumberOfLinks != 1) return Error(ERROR_INVALID_DATA);
    return true;
}

bool OpenRegular(const std::wstring& path, DWORD access, Handle& handle,
                 BY_HANDLE_FILE_INFORMATION& info) {
    if (!PathSafe(path, false, false)) return false;
    handle = Handle(CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    return handle.valid() && RegularInfo(handle.get(), info);
}

class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return;
        DWORD bytes = 0, returned = 0;
        if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&bytes),
                              sizeof(bytes), &returned, 0) < 0) return;
        object_.resize(bytes);
        if (BCryptCreateHash(algorithm_, &hash_, object_.data(), bytes, nullptr, 0, 0) < 0) hash_ = nullptr;
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    bool add(const unsigned char* data, std::size_t size) {
        if (!hash_) return Error(ERROR_GEN_FAILURE);
        while (size) {
            const ULONG chunk = static_cast<ULONG>((std::min)(size, std::size_t(65536)));
            if (BCryptHashData(hash_, const_cast<PUCHAR>(data), chunk, 0) < 0) return Error(ERROR_GEN_FAILURE);
            data += chunk;
            size -= chunk;
        }
        return true;
    }
    std::wstring finish() {
        unsigned char digest[32]{};
        if (!hash_ || BCryptFinishHash(hash_, digest, sizeof(digest), 0) < 0) {
            SetLastError(ERROR_GEN_FAILURE);
            return {};
        }
        constexpr wchar_t hex[] = L"0123456789abcdef";
        std::wstring result(64, L'0');
        for (std::size_t i = 0; i < sizeof(digest); ++i) {
            result[2 * i] = hex[digest[i] >> 4];
            result[2 * i + 1] = hex[digest[i] & 15];
        }
        return result;
    }
private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<unsigned char> object_;
};

std::wstring HashMemory(const unsigned char* data, std::size_t size) {
    Sha256 hash;
    return hash.add(data, size) ? hash.finish() : std::wstring();
}

std::wstring HashHandle(HANDLE file) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return {};
    Sha256 hash;
    unsigned char buffer[65536];
    for (;;) {
        DWORD bytes = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &bytes, nullptr)) return {};
        if (!bytes) return hash.finish();
        if (!hash.add(buffer, bytes)) return {};
    }
}

bool Digest(const std::wstring& text, std::wstring& normalized) {
    if (text.size() != 64) return Error(ERROR_INVALID_DATA);
    normalized = text;
    for (wchar_t& c : normalized) {
        if (c >= L'A' && c <= L'F') c += L'a' - L'A';
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return Error(ERROR_INVALID_DATA);
    }
    return true;
}

struct Payloads {
    std::array<const PayloadFile*, kPayloadCount> files{};
    std::array<std::wstring, kPayloadCount> hashes{};
};

bool CheckPayloads(const PayloadFile* files, std::size_t count, Payloads& checked) {
    if (!files || count != kPayloadCount) return Error(ERROR_INVALID_DATA);
    for (std::size_t i = 0; i < count; ++i) {
        if (!files[i].name || !files[i].sha256 || !files[i].data || !files[i].size)
            return Error(ERROR_INVALID_DATA);
        std::size_t index = 0;
        while (index < kPayloadCount && std::wcscmp(files[i].name, kNames[index]) != 0) ++index;
        if (index == kPayloadCount || checked.files[index]) return Error(ERROR_INVALID_DATA);
        if (!Digest(files[i].sha256, checked.hashes[index])) return false;
        const std::wstring actual = HashMemory(files[i].data, files[i].size);
        if (actual.empty()) return false;
        if (actual != checked.hashes[index]) return Error(ERROR_CRC);
        checked.files[index] = &files[i];
    }
    return true;
}

bool Running(bool& running) {
    running = false;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) return GetLastError() == ERROR_NO_MORE_FILES;
    do {
        if (_wcsicmp(entry.szExeFile, kApplicationName) == 0) { running = true; return true; }
    } while (Process32NextW(snapshot.get(), &entry));
    return GetLastError() == ERROR_NO_MORE_FILES;
}

Result CheckStopped() {
    bool running = false;
    if (!Running(running)) return Failure(L"Não foi possível verificar se o Lake Controller está fechado.", LastErrorOr());
    if (running) return Failure(L"Feche o Lake Controller antes de instalar ou remover o modo janela.", ERROR_BUSY);
    return Success(L"");
}

Result ReadApplication(const std::wstring& directory, const SupportedBuild& build,
                       Handle& executableFile, Handle& rendererFile) {
    std::wstring app, executableHash, rendererHash;
    if (!Normalize(directory, app) || !PathSafe(app, true, false))
        return Failure(L"Selecione a pasta original do Lake Controller em um caminho local absoluto, sem links.", LastErrorOr());
    if (!Digest(build.executableSha256, executableHash) || !Digest(build.rendererSha256, rendererHash))
        return Failure(L"A identificação da versão suportada é inválida.");
    BY_HANDLE_FILE_INFORMATION executableInfo{}, rendererInfo{};
    if (!OpenRegular(Join(app, kApplicationName), GENERIC_READ, executableFile, executableInfo))
        return Failure(L"Não foi possível ler Lake Controller.exe na pasta selecionada.", LastErrorOr());
    if (!OpenRegular(Join(app, kRendererName), GENERIC_READ, rendererFile, rendererInfo))
        return Failure(L"Não foi possível ler Contour.dll na pasta selecionada.", LastErrorOr());
    const std::wstring executable = HashHandle(executableFile.get());
    if (executable.empty()) return Failure(L"Não foi possível verificar Lake Controller.exe.", LastErrorOr());
    const std::wstring renderer = HashHandle(rendererFile.get());
    if (renderer.empty()) return Failure(L"Não foi possível verificar Contour.dll.", LastErrorOr());
    if (executable != executableHash || renderer != rendererHash)
        return Failure(L"Esta versão do Lake Controller não é suportada. Nenhum arquivo original foi alterado.", ERROR_REVISION_MISMATCH);
    return Success(L"Versão original verificada (somente leitura).");
}

bool TreeSafe(const std::wstring& directory, unsigned depth = 0, std::size_t* visited = nullptr) {
    std::size_t local = 0;
    if (!visited) visited = &local;
    if (depth > 128 || ++*visited > 100000) return Error(ERROR_DIRECTORY);
    if (!PathSafe(directory, true, false)) return false;
    WIN32_FIND_DATAW entry{};
    const HANDLE search = FindFirstFileW(Join(directory, L"*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool ok = true;
    DWORD error = ERROR_SUCCESS;
    do {
        if (!std::wcscmp(entry.cFileName, L".") || !std::wcscmp(entry.cFileName, L"..")) continue;
        if (++*visited > 100000 || (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            ok = false; error = ERROR_REPARSE_TAG_INVALID; break;
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !TreeSafe(Join(directory, entry.cFileName), depth + 1, visited)) {
            ok = false; error = LastErrorOr(); break;
        }
    } while (FindNextFileW(search, &entry));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES) { ok = false; error = LastErrorOr(); }
    FindClose(search);
    return ok ? true : Error(error);
}

std::wstring Environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size || size > 32768) return {};
    std::vector<wchar_t> value(size);
    const DWORD length = GetEnvironmentVariableW(name, value.data(), size);
    return length && length < size ? std::wstring(value.data(), length) : std::wstring();
}

bool ProtectedDestination(const std::wstring& path) {
    std::vector<std::wstring> roots;
    wchar_t windows[32768]{};
    const UINT length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
    if (!length || length >= ARRAYSIZE(windows)) return true;
    roots.emplace_back(windows, length);
    const std::wstring drive(windows, 3);
    roots.push_back(drive + L"Program Files");
    roots.push_back(drive + L"Program Files (x86)");
    for (const wchar_t* name : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"}) {
        const std::wstring value = Environment(name);
        if (!value.empty()) roots.push_back(value);
    }
    for (const std::wstring& root : roots) {
        std::wstring normalized;
        if (!Normalize(root, normalized)) return true;
        if (Overlap(path, normalized)) return true;
    }
    return false;
}

bool TestRegistryKey(const std::wstring& key) {
    const std::wstring prefix = L"Software\\LakeWindowedFix.SetupTests\\";
    if (key.size() != prefix.size() + 38 || key.compare(0, prefix.size(), prefix) != 0) return false;
    GUID guid{};
    wchar_t canonical[40]{};
    const std::wstring suffix = key.substr(prefix.size());
    return SUCCEEDED(CLSIDFromString(suffix.c_str(), &guid)) && StringFromGUID2(guid, canonical, ARRAYSIZE(canonical)) &&
        EqualPath(suffix, canonical);
}

std::wstring RegistryKey(const Locations& locations) {
    return locations.testRegistryKey.empty() ? kRegistryKey : locations.testRegistryKey;
}

LSTATUS FlushRegistration(const Locations& locations, HKEY key) {
    // RegFlushKey flushes the entire user hive, not just this key. Sandbox
    // tests must not flush the operational profile as a side effect. They test
    // process-interruption recovery, not registry persistence after power loss.
    return locations.testRegistryKey.empty() ? RegFlushKey(key) : ERROR_SUCCESS;
}

bool CheckLocations(const Locations& input, const std::wstring& app, Locations& locations) {
    locations.registerUninstall = input.registerUninstall;
    if ((!input.testRegistryKey.empty() && !TestRegistryKey(input.testRegistryKey)) ||
        (input.faultInjection && input.registerUninstall && input.testRegistryKey.empty())) return Error(ERROR_INVALID_PARAMETER);
    locations.testRegistryKey = input.testRegistryKey;
    locations.faultInjection = input.faultInjection;
    if (!Normalize(input.installDirectory, locations.installDirectory) ||
        !Normalize(input.desktopDirectory, locations.desktopDirectory) ||
        !Normalize(input.programsDirectory, locations.programsDirectory)) return false;
    const std::array<std::wstring, 3> paths = {{locations.installDirectory, locations.desktopDirectory,
                                               locations.programsDirectory}};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (ProtectedDestination(paths[i])) return Error(ERROR_ACCESS_DENIED);
        if (!app.empty() && Overlap(paths[i], app)) return Error(ERROR_BAD_PATHNAME);
        if (!PathSafe(paths[i], true, true)) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (Overlap(paths[i], paths[j])) return Error(ERROR_BAD_PATHNAME);
    }
    return true;
}

std::wstring Arguments(const std::wstring& app) { return L"--app-dir \"" + app + L"\""; }

struct Manifest {
    std::wstring app;
    std::wstring managerHash;
    std::wstring version = kVersion;
    unsigned schema = 2;
    std::array<std::wstring, kPayloadCount> hashes{};
    // Exact serialized links make repair deterministic even if the launcher was
    // recreated with different timestamps. Never trust only an owned-looking name.
    std::array<std::vector<unsigned char>, 2> shortcuts{};
};

std::wstring HexBytes(const std::vector<unsigned char>& bytes) {
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring text;
    for (unsigned char byte : bytes) { text += hex[byte >> 4]; text += hex[byte & 15]; }
    return text;
}

bool UnhexBytes(const std::wstring& text, std::vector<unsigned char>& bytes) {
    if (text.empty() || text.size() > 16384 || text.size() % 2) return Error(ERROR_INVALID_DATA);
    const auto digit = [](wchar_t c) { return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1; };
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int a = digit(text[i]), b = digit(text[i + 1]);
        if (a < 0 || b < 0) return Error(ERROR_INVALID_DATA);
        bytes.push_back(static_cast<unsigned char>((a << 4) | b));
    }
    return true;
}

std::wstring ManifestText(const Locations& locations, const Manifest& manifest) {
    std::wstring text = L"\xfeff[setup]\r\nschema=" + std::to_wstring(manifest.schema) + L"\r\nproject=";
    text += kProject;
    text += L"\r\nversion=" + manifest.version;
    text += L"\r\nappDirectory=" + manifest.app;
    text += L"\r\ninstallDirectory=" + locations.installDirectory;
    text += L"\r\ndesktopDirectory=" + locations.desktopDirectory;
    text += L"\r\nprogramsDirectory=" + locations.programsDirectory;
    text += locations.registerUninstall ? L"\r\nregisterUninstall=1" : L"\r\nregisterUninstall=0";
    text += L"\r\nfileCount=7\r\n[files]\r\n";
    for (std::size_t i = 0; i < kPayloadCount; ++i) text += std::wstring(kNames[i]) + L"=" + manifest.hashes[i] + L"\r\n";
    text += std::wstring(kManagerName) + L"=" + manifest.managerHash + L"\r\n";
    if (manifest.schema == 2)
        text += L"[shortcuts]\r\ndesktop=" + HexBytes(manifest.shortcuts[0]) +
            L"\r\nprograms=" + HexBytes(manifest.shortcuts[1]) + L"\r\n";
    return text;
}

bool ReadSmallText(HANDLE file, std::wstring& text) {
    LARGE_INTEGER size{}, zero{};
    if (!GetFileSizeEx(file, &size)) return false;
    if (size.QuadPart < 2 || size.QuadPart > 131072 || size.QuadPart % sizeof(wchar_t)) return Error(ERROR_INVALID_DATA);
    text.resize(static_cast<std::size_t>(size.QuadPart) / sizeof(wchar_t));
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return false;
    DWORD bytes = 0;
    if (!ReadFile(file, text.data(), static_cast<DWORD>(size.QuadPart), &bytes, nullptr)) return false;
    return bytes == size.QuadPart && text.front() == L'\xfeff' && text.find(L'\0') == std::wstring::npos
        ? true : Error(ERROR_INVALID_DATA);
}

bool Field(const std::wstring& text, const std::wstring& key, std::wstring& value) {
    const std::wstring prefix = L"\r\n" + key + L"=";
    const std::size_t at = text.find(prefix);
    if (at == std::wstring::npos) return Error(ERROR_INVALID_DATA);
    const std::size_t start = at + prefix.size(), end = text.find(L"\r\n", start);
    if (end == std::wstring::npos) return Error(ERROR_INVALID_DATA);
    value = text.substr(start, end - start);
    return true;
}

bool ParseManifest(const std::wstring& text, const Locations& locations, Manifest& manifest) {
    // Canonical round-trip rejects duplicate/unknown fields, alternate sections,
    // path-bearing file names, a changed count and trailing hidden INI content.
    std::wstring value, recordedApp;
    if (!Field(text, L"schema", value) || (value != L"1" && value != L"2")) return Error(ERROR_INVALID_DATA);
    manifest.schema = value == L"1" ? 1 : 2;
    if (!Field(text, L"version", manifest.version) ||
        (manifest.version != kVersion && manifest.version != L"0.6.0")) return Error(ERROR_REVISION_MISMATCH);
    if (!Field(text, L"appDirectory", recordedApp)) return false;
    // The original directory (or even its drive) need not still exist when
    // uninstalling. Validate this recorded path lexically, without opening it.
    if (!Normalize(recordedApp, manifest.app, false) || !EqualPath(recordedApp, manifest.app) ||
        !Field(text, kManagerName, value) || !Digest(value, manifest.managerHash)) return Error(ERROR_INVALID_DATA);
    for (std::size_t i = 0; i < kPayloadCount; ++i)
        if (!Field(text, kNames[i], value) || !Digest(value, manifest.hashes[i])) return false;
    if (manifest.schema == 2) {
        if (!Field(text, L"desktop", value) || !UnhexBytes(value, manifest.shortcuts[0]) ||
            !Field(text, L"programs", value) || !UnhexBytes(value, manifest.shortcuts[1])) return false;
    }
    if (text != ManifestText(locations, manifest)) return Error(ERROR_INVALID_DATA);
    for (const std::wstring& path : {locations.installDirectory, locations.desktopDirectory, locations.programsDirectory})
        if (Overlap(path, manifest.app)) return Error(ERROR_BAD_PATHNAME);
    return true;
}

struct LockedFile {
    std::wstring path;
    std::wstring hash;
    Handle handle;
    BY_HANDLE_FILE_INFORMATION identity{};
    bool remove = true;
};

bool LockFile(const std::wstring& path, bool remove, LockedFile& file) {
    file.path = path;
    file.remove = remove;
    if (!OpenRegular(path, GENERIC_READ | (remove ? DELETE : 0), file.handle, file.identity)) return false;
    file.hash = HashHandle(file.handle.get());
    if (file.hash.empty()) return false;
    if (remove && (file.identity.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) return Error(ERROR_ACCESS_DENIED);
    return true;
}

bool MarkDeleted(HANDLE handle) {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
}

bool DeleteLocked(LockedFile& file) {
    BY_HANDLE_FILE_INFORMATION current{};
    if (!RegularInfo(file.handle.get(), current) || !SameIdentity(file.identity, current)) return Error(ERROR_INVALID_DATA);
    if (HashHandle(file.handle.get()) != file.hash) return Error(ERROR_CRC);
    if (!MarkDeleted(file.handle.get())) return false;
    file.handle.reset();
    return true;
}

bool ShortcutPropertiesMatch(IShellLinkW* link, const Locations& locations, const std::wstring& app,
                             bool legacy) {
    std::vector<wchar_t> target(32768), arguments(32768), working(32768), description(32768), icon(32768);
    int iconIndex = 0, show = 0;
    HRESULT hr = link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH);
    if (SUCCEEDED(hr)) hr = link->GetArguments(arguments.data(), static_cast<int>(arguments.size()));
    if (SUCCEEDED(hr)) hr = link->GetWorkingDirectory(working.data(), static_cast<int>(working.size()));
    if (SUCCEEDED(hr)) hr = link->GetDescription(description.data(), static_cast<int>(description.size()));
    if (SUCCEEDED(hr)) hr = link->GetIconLocation(icon.data(), static_cast<int>(icon.size()), &iconIndex);
    if (SUCCEEDED(hr)) hr = link->GetShowCmd(&show);
    if (FAILED(hr)) return Error(static_cast<DWORD>(hr));
    return EqualPath(target.data(), Join(locations.installDirectory, kNames[0])) &&
        std::wstring(arguments.data()) == Arguments(app) && EqualPath(working.data(), app) &&
        (legacy || (std::wstring(description.data()) == kDisplayName &&
            EqualPath(icon.data(), Join(locations.installDirectory, kNames[0])) && iconIndex == 0 && show == SW_SHOWNORMAL))
        ? true : Error(ERROR_INVALID_DATA);
}

bool ShortcutMatches(const std::wstring& path, const Locations& locations, const std::wstring& app, bool legacy) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> persistence;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  reinterpret_cast<void**>(link.out()));
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(persistence.out()));
    if (SUCCEEDED(hr)) hr = persistence->Load(path.c_str(), STGM_READ);
    return SUCCEEDED(hr) ? ShortcutPropertiesMatch(link.get(), locations, app, legacy) : Error(static_cast<DWORD>(hr));
}

bool ShortcutTemplateSafe(const std::vector<unsigned char>& bytes, const Locations& locations, const std::wstring& app) {
    ComPtr<IStream> stream;
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistStream> persistence;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, stream.out());
    ULONG written = 0;
    if (SUCCEEDED(hr)) hr = stream->Write(bytes.data(), static_cast<ULONG>(bytes.size()), &written);
    if (FAILED(hr) || written != bytes.size()) return Error(ERROR_WRITE_FAULT);
    LARGE_INTEGER zero{};
    hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                            reinterpret_cast<void**>(link.out()));
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_IPersistStream, reinterpret_cast<void**>(persistence.out()));
    if (SUCCEEDED(hr)) hr = persistence->Load(stream.get());
    return SUCCEEDED(hr) ? ShortcutPropertiesMatch(link.get(), locations, app, false) : Error(static_cast<DWORD>(hr));
}

struct RegValue {
    std::wstring name;
    DWORD type = REG_SZ;
    std::vector<unsigned char> data;
};

RegValue StringValue(const wchar_t* name, const std::wstring& value) {
    RegValue result;
    result.name = name;
    result.data.resize((value.size() + 1) * sizeof(wchar_t));
    std::memcpy(result.data.data(), value.c_str(), result.data.size());
    return result;
}

RegValue DwordValue(const wchar_t* name, DWORD value) {
    RegValue result;
    result.name = name;
    result.type = REG_DWORD;
    result.data.resize(sizeof(value));
    std::memcpy(result.data.data(), &value, sizeof(value));
    return result;
}

std::vector<RegValue> RegistrationValues(const Locations& locations, const std::wstring& version = kVersion) {
    return {StringValue(L"DisplayName", kDisplayName), StringValue(L"DisplayVersion", version),
        StringValue(L"Publisher", kProject), StringValue(L"InstallLocation", locations.installDirectory),
        StringValue(L"UninstallString", L"\"" + Join(locations.installDirectory, kManagerName) + L"\" --uninstall"),
        DwordValue(L"NoModify", 1), DwordValue(L"NoRepair", 1)};
}

bool RegistryMatches(HKEY key, const std::vector<RegValue>& expected) {
    DWORD subkeys = 0, values = 0;
    LSTATUS status = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
                                    &values, nullptr, nullptr, nullptr, nullptr);
    if (status != ERROR_SUCCESS) return Error(status);
    if (subkeys || values != expected.size()) return Error(ERROR_INVALID_DATA);
    for (const RegValue& value : expected) {
        DWORD type = 0, size = 0;
        status = RegQueryValueExW(key, value.name.c_str(), nullptr, &type, nullptr, &size);
        if (status != ERROR_SUCCESS) return Error(status);
        if (type != value.type || size != value.data.size()) return Error(ERROR_INVALID_DATA);
        std::vector<unsigned char> data(size);
        status = RegQueryValueExW(key, value.name.c_str(), nullptr, &type, data.data(), &size);
        if (status != ERROR_SUCCESS) return Error(status);
        if (type != value.type || data != value.data || size != data.size()) return Error(ERROR_INVALID_DATA);
    }
    return true;
}

enum class RegistrationState { Missing, Owned, Foreign };

bool InspectRegistrationAt(const Locations& locations, const std::wstring& path,
                           const std::wstring& version, RegistrationState& state) {
    state = RegistrationState::Missing;
    if (!locations.registerUninstall) return true;
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), REG_OPTION_OPEN_LINK,
        KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &key);
    if (Missing(status)) return true;
    if (status != ERROR_SUCCESS) return Error(status);
    const bool matches = RegistryMatches(key, RegistrationValues(locations, version));
    const DWORD error = GetLastError();
    RegCloseKey(key);
    if (!matches && error != ERROR_INVALID_DATA && !Missing(error) && error != ERROR_MORE_DATA) return Error(error);
    state = matches ? RegistrationState::Owned : RegistrationState::Foreign;
    return true;
}

bool InspectRegistration(const Locations& locations, const std::wstring& version, RegistrationState& state) {
    return InspectRegistrationAt(locations, RegistryKey(locations), version, state);
}

struct CreatedFile {
    std::wstring path;
    std::wstring hash;
    BY_HANDLE_FILE_INFORMATION identity{};
};
struct CreatedDirectory {
    std::wstring path;
    BY_HANDLE_FILE_INFORMATION identity{};
};

class DirectoryPins {
public:
    bool add(const std::wstring& path, bool deletable, BY_HANDLE_FILE_INFORMATION* identity = nullptr) {
        for (const Pin& pin : pins_) {
            if (!EqualPath(pin.path, path)) continue;
            if (deletable && !pin.deletable) return Error(ERROR_ACCESS_DENIED);
            if (identity) *identity = pin.identity;
            return true;
        }
        if (!PathSafe(path, true, false)) return false;
        Pin pin;
        pin.path = path;
        pin.deletable = deletable;
        // No write/delete sharing: ancestors cannot be replaced, renamed or
        // turned into junctions between validation and a subsequent CREATE_NEW.
        // This does not deny ordinary creation of files inside the directory.
        pin.handle = Handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!pin.handle.valid() || !GetFileInformationByHandle(pin.handle.get(), &pin.identity)) return false;
        if (!(pin.identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (pin.identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return Error(ERROR_REPARSE_TAG_INVALID);
        if (identity) *identity = pin.identity;
        pins_.push_back(std::move(pin));
        return true;
    }

    bool existingAncestors(const std::wstring& path, bool deleteFinal = false) {
        std::size_t end = path.find(L'\\', 3);
        for (;;) {
            const bool final = end == std::wstring::npos;
            const std::wstring prefix = final ? path : path.substr(0, end);
            if (GetFileAttributesW(prefix.c_str()) == INVALID_FILE_ATTRIBUTES) {
                const DWORD error = GetLastError();
                return Missing(error) ? true : Error(error);
            }
            if (!add(prefix, final && deleteFinal)) return false;
            if (final) return true;
            end = path.find(L'\\', end + 1);
        }
    }

    bool removeEmpty(const std::wstring& path, const BY_HANDLE_FILE_INFORMATION* expected = nullptr) {
        for (Pin& pin : pins_) {
            if (!EqualPath(pin.path, path) || !pin.deletable || !pin.handle.valid()) continue;
            // Do not hold directory DELETE access during Shell/FindFirstFile
            // operations: their readers need not share DELETE. Upgrade only
            // now, and reject a replacement object by its recorded identity.
            pin.handle.reset();
            Handle deletion(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION current{};
            if (!deletion.valid() || !GetFileInformationByHandle(deletion.get(), &current)) return false;
            if (!SameIdentity(pin.identity, current) || (expected && !SameIdentity(*expected, current)) ||
                !(current.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (current.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return Error(ERROR_INVALID_DATA);
            // Like RemoveDirectory, disposition of a directory is empty-only.
            // Delete this verified handle, never an unchecked pathname.
            if (!MarkDeleted(deletion.get())) return false;
            return true;
        }
        return Error(ERROR_INVALID_HANDLE);
    }
private:
    struct Pin {
        std::wstring path;
        Handle handle;
        BY_HANDLE_FILE_INFORMATION identity{};
        bool deletable = false;
    };
    std::vector<Pin> pins_;
};

class Transaction {
public:
    ~Transaction() { if (!committed) rollback(); }
    bool committed = false;
    DirectoryPins pins;
    std::vector<CreatedFile> files;
    std::vector<CreatedDirectory> directories;
    bool registryCreated = false;
    std::wstring registryKey;
    std::vector<RegValue> registryValues;
private:
    void rollback() {
        const DWORD saved = GetLastError();
        if (registryCreated) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, registryKey.c_str(), REG_OPTION_OPEN_LINK,
                KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | DELETE, &key) == ERROR_SUCCESS) {
                const bool unchanged = RegistryMatches(key, registryValues);
                RegCloseKey(key);
                if (unchanged) RegDeleteKeyW(HKEY_CURRENT_USER, registryKey.c_str());
            }
        }
        for (auto it = files.rbegin(); it != files.rend(); ++it) {
            LockedFile current;
            if (LockFile(it->path, true, current) && SameIdentity(it->identity, current.identity) && current.hash == it->hash)
                DeleteLocked(current);
        }
        for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
            pins.removeEmpty(it->path, &it->identity); // Never removes a nonempty directory.
        }
        SetLastError(saved);
    }
};

bool RecordDirectory(Transaction& transaction, const std::wstring& path) {
    CreatedDirectory created;
    created.path = path;
    if (!transaction.pins.add(path, true, &created.identity)) return false;
    transaction.directories.push_back(std::move(created));
    return true;
}

bool EnsureDirectory(Transaction& transaction, const std::wstring& path) {
    if (path.size() == 3) return PathSafe(path, true, false);
    if (!PathSafe(path, true, true)) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return transaction.pins.add(path, false);
    if (!Missing(GetLastError()) || !EnsureDirectory(transaction, Parent(path))) return false;
    if (!CreateDirectoryW(path.c_str(), nullptr)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
        return transaction.pins.add(path, false);
    }
    return RecordDirectory(transaction, path);
}

bool WriteNew(Transaction& transaction, const std::wstring& path, const unsigned char* data,
              std::size_t size, const std::wstring& expectedHash, HANDLE source = INVALID_HANDLE_VALUE) {
    if (!PathSafe(path, false, true)) return false;
    Handle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.valid()) return false;
    CreatedFile created;
    created.path = path;
    bool ok = RegularInfo(file.get(), created.identity);
    DWORD error = ok ? ERROR_SUCCESS : LastErrorOr();
    unsigned char buffer[65536];
    LARGE_INTEGER zero{};
    if (ok && source != INVALID_HANDLE_VALUE && !SetFilePointerEx(source, zero, nullptr, FILE_BEGIN)) {
        ok = false; error = LastErrorOr();
    }
    for (std::size_t offset = 0; ok;) {
        DWORD bytes = 0;
        const unsigned char* chunk = data;
        if (source != INVALID_HANDLE_VALUE) {
            if (!ReadFile(source, buffer, sizeof(buffer), &bytes, nullptr)) { ok = false; error = LastErrorOr(); break; }
            chunk = buffer;
        } else {
            bytes = static_cast<DWORD>((std::min)(size - offset, std::size_t(sizeof(buffer))));
            if (bytes) chunk = data + offset;
        }
        if (!bytes) break;
        DWORD written = 0;
        if (!WriteFile(file.get(), chunk, bytes, &written, nullptr) || written != bytes) {
            ok = false; error = LastErrorOr(ERROR_WRITE_FAULT); break;
        }
        offset += bytes;
    }
    if (ok && !FlushFileBuffers(file.get())) { ok = false; error = LastErrorOr(); }
    if (ok) {
        created.hash = HashHandle(file.get());
        if (created.hash.empty() || created.hash != expectedHash) { ok = false; error = ERROR_CRC; }
    }
    if (!ok) {
        // This CREATE_NEW handle has never permitted another writer or renamer;
        // even an incomplete write is ours, not a preexisting or edited file.
        MarkDeleted(file.get());
        return Error(error);
    }
    transaction.files.push_back(std::move(created));
    return true;
}

bool SerializeShortcut(const Locations& locations, const std::wstring& app, std::vector<unsigned char>& bytes) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistStream> persistence;
    ComPtr<IStream> stream;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  reinterpret_cast<void**>(link.out()));
    const std::wstring launcher = Join(locations.installDirectory, kNames[0]);
    if (SUCCEEDED(hr)) hr = link->SetPath(launcher.c_str());
    if (SUCCEEDED(hr)) hr = link->SetArguments(Arguments(app).c_str());
    if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(app.c_str());
    if (SUCCEEDED(hr)) hr = link->SetDescription(kDisplayName);
    if (SUCCEEDED(hr)) hr = link->SetIconLocation(launcher.c_str(), 0);
    if (SUCCEEDED(hr)) hr = link->SetShowCmd(SW_SHOWNORMAL);
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_IPersistStream, reinterpret_cast<void**>(persistence.out()));
    if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, stream.out());
    if (SUCCEEDED(hr)) hr = persistence->Save(stream.get(), TRUE);
    STATSTG stat{};
    if (SUCCEEDED(hr)) hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr)) return Error(static_cast<DWORD>(hr));
    if (stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > 8192) return Error(ERROR_INVALID_DATA);
    bytes.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    if (SUCCEEDED(hr)) hr = stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
    if (FAILED(hr) || read != bytes.size()) return Error(ERROR_READ_FAULT);
    return true;
}

bool CreateRegistrationAt(Transaction& transaction, const Locations& locations, const std::wstring& path,
                          const std::wstring& version) {
    if (!locations.registerUninstall) return true;
    HKEY key = nullptr;
    DWORD disposition = 0;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_ENUMERATE_SUB_KEYS | DELETE, nullptr, &key, &disposition);
    if (status != ERROR_SUCCESS) return Error(status);
    if (disposition != REG_CREATED_NEW_KEY) {
        RegCloseKey(key);
        return Error(ERROR_ALREADY_EXISTS); // No writes and no rollback deletion of an old key.
    }
    transaction.registryCreated = true;
    transaction.registryKey = path;
    for (const RegValue& value : RegistrationValues(locations, version)) {
        status = RegSetValueExW(key, value.name.c_str(), 0, value.type, value.data.data(), static_cast<DWORD>(value.data.size()));
        if (status != ERROR_SUCCESS) break;
        transaction.registryValues.push_back(value);
    }
    if (status == ERROR_SUCCESS) status = FlushRegistration(locations, key);
    RegCloseKey(key);
    return status == ERROR_SUCCESS ? true : Error(status);
}

bool CreateRegistration(Transaction& transaction, const Locations& locations) {
    return CreateRegistrationAt(transaction, locations, RegistryKey(locations), kVersion);
}

struct VerifiedInstallation {
    Manifest manifest;
    LockedFile receipt;
    std::array<LockedFile, kPayloadCount + 3> files; // Payloads, manager, desktop, programs.
    bool editedConfiguration = false;
    bool preservedFiles = false;
    std::vector<std::wstring> preservedLabels; // Fixed own names/roles, never enumerate private user files.
    RegistrationState registration = RegistrationState::Missing;
};

bool ReadReceipt(const Locations& locations, VerifiedInstallation& installation) {
    if (!LockFile(Join(locations.installDirectory, kManifestName), false, installation.receipt)) return false;
    std::wstring text;
    if (!ReadSmallText(installation.receipt.handle.get(), text) || !ParseManifest(text, locations, installation.manifest)) return false;
    if (installation.manifest.schema == 2)
        for (const auto& bytes : installation.manifest.shortcuts)
            if (!ShortcutTemplateSafe(bytes, locations, installation.manifest.app)) return false;
    return true;
}

bool LockOptional(const std::wstring& path, bool remove, LockedFile& file) {
    file.path = path;
    if (!PathSafe(path, false, true)) return false;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return Missing(GetLastError()) ? true : false;
    return LockFile(path, remove, file);
}

bool UpgradeDeletion(LockedFile& file) {
    const BY_HANDLE_FILE_INFORMATION identity = file.identity;
    const std::wstring hash = file.hash, path = file.path;
    file.handle.reset();
    if (!LockFile(path, true, file) || !SameIdentity(identity, file.identity) || file.hash != hash)
        return Error(ERROR_CRC);
    return true;
}

bool VerifyObjects(const Locations& locations, bool removing, VerifiedInstallation& installation) {
    for (std::size_t i = 0; i <= kPayloadCount; ++i) {
        const wchar_t* name = i == kPayloadCount ? kManagerName : kNames[i];
        const std::wstring& expected = i == kPayloadCount ? installation.manifest.managerHash : installation.manifest.hashes[i];
        const bool configuration = i == kConfigurationIndex;
        LockedFile& file = installation.files[i];
        if (!LockOptional(Join(locations.installDirectory, name), false, file)) return false;
        if (!file.handle.valid()) continue; // Missing files are already removed, or safely repairable.
        file.remove = false;
        if (file.hash != expected) {
            if (configuration) {
                installation.editedConfiguration = true;
                installation.preservedLabels.emplace_back(L"LakeWindowedFix.ini (editado)");
            } else {
                installation.preservedFiles = true;
                installation.preservedLabels.emplace_back(name);
            }
        } else if (removing && !UpgradeDeletion(file)) return false;
    }
    const std::array<std::wstring, 2> directories = {{locations.desktopDirectory, locations.programsDirectory}};
    for (std::size_t i = 0; i < directories.size(); ++i) {
        LockedFile& shortcut = installation.files[kPayloadCount + 1 + i];
        const std::wstring path = Join(directories[i], kShortcutName);
        // Shell's loader need not share DELETE. Inspect under a read-only pin,
        // then acquire the deletion pin and compare both file ID and bytes.
        if (!LockOptional(path, false, shortcut)) return false;
        if (!shortcut.handle.valid()) continue;
        shortcut.remove = false;
        const auto& bytes = installation.manifest.shortcuts[i];
        const bool owned = installation.manifest.schema == 2
            ? shortcut.hash == HashMemory(bytes.data(), bytes.size())
            : ShortcutMatches(path, locations, installation.manifest.app, true);
        if (!owned) {
            installation.preservedFiles = true;
            installation.preservedLabels.emplace_back(i == 0 ? L"atalho da area de trabalho" : L"atalho do menu Iniciar");
        }
        else if (removing && !UpgradeDeletion(shortcut)) return false;
    }
    if (!InspectRegistration(locations, installation.manifest.version, installation.registration)) return false;
    if (installation.registration == RegistrationState::Foreign) {
        installation.preservedFiles = true;
        installation.preservedLabels.emplace_back(L"registro de remocao alterado");
    }
    return true;
}

std::wstring PreservedSummary(const VerifiedInstallation& installation) {
    std::wstring result;
    for (const auto& label : installation.preservedLabels) {
        result += result.empty() ? L"\r\nObjetos conhecidos preservados: " : L"; ";
        result += label;
    }
    return result;
}

bool InstallationComplete(const Locations& locations, const VerifiedInstallation& installation) {
    if (installation.preservedFiles) return Error(ERROR_CRC);
    // The same verifier intentionally tolerates absence during preflight/removal,
    // but a successful install/repair must never certify an incomplete tree.
    for (const auto& file : installation.files)
        if (!file.handle.valid()) return Error(ERROR_FILE_NOT_FOUND);
    if (locations.registerUninstall && installation.registration != RegistrationState::Owned)
        return Error(ERROR_INVALID_DATA);
    return true;
}

bool Checkpoint(const Locations& locations, const wchar_t* name) {
    FaultInjection* fault = locations.faultInjection;
    if (!fault) return true;
    fault->lastStep = name;
    ++fault->completedSteps;
    if (fault->observe) fault->observe(name, fault->completedSteps, fault->context);
    return fault->failAfterStep != fault->completedSteps ? true : Error(ERROR_OPERATION_ABORTED);
}

bool WriteTextNew(Transaction& transaction, const std::wstring& path, const std::wstring& text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t size = text.size() * sizeof(wchar_t);
    const std::wstring hash = HashMemory(bytes, size);
    return !hash.empty() && WriteNew(transaction, path, bytes, size, hash);
}

std::wstring RemovalText(const std::wstring& receiptHash) {
    return L"\xfeff[recovery]\r\nschema=1\r\naction=remove\r\nmanifestSha256=" + receiptHash + L"\r\n";
}

bool RemovalJournal(const Locations& locations, const std::wstring& receiptHash, bool create,
                    LockedFile& journal, bool& created) {
    created = false;
    const std::wstring path = Join(locations.installDirectory, kRemovalJournalName);
    if (!LockOptional(path, true, journal)) return false;
    if (!journal.handle.valid() && create) {
        Transaction transaction;
        if (!WriteTextNew(transaction, path, RemovalText(receiptHash))) return false;
        transaction.committed = true; // A flushed marker must outlive partial removals.
        created = true;
        if (!LockFile(path, true, journal)) return false;
    }
    if (!journal.handle.valid()) return true;
    std::wstring text;
    return ReadSmallText(journal.handle.get(), text) && text == RemovalText(receiptHash)
        ? true : Error(ERROR_INVALID_DATA);
}

bool ReadBytes(HANDLE file, std::vector<unsigned char>& bytes) {
    LARGE_INTEGER size{}, zero{};
    if (!GetFileSizeEx(file, &size)) return false;
    if (size.QuadPart < 0 || size.QuadPart > LAKE_FIX_MAX_INI_BYTES) return Error(ERROR_INVALID_DATA);
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return false;
    DWORD read = 0;
    return ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
}

struct IniEncoding { bool utf16 = false; bool bom = false; };

bool DecodeIni(const unsigned char* bytes, std::size_t size, std::wstring& text, IniEncoding& encoding) {
    if (size >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
        if (size % 2) return Error(ERROR_INVALID_DATA);
        encoding = {true, true};
        text.resize((size - 2) / 2);
        if (size > 2) std::memcpy(text.data(), bytes + 2, size - 2);
    } else {
        encoding.bom = size >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf;
        if (encoding.bom) { bytes += 3; size -= 3; }
        if (size) {
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes), static_cast<int>(size), nullptr, 0);
            if (!length) return false;
            text.resize(length);
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes), static_cast<int>(size), text.data(), length)) return false;
        }
    }
    return text.find(L'\0') == std::wstring::npos ? true : Error(ERROR_INVALID_DATA);
}

bool EncodeIni(const std::wstring& text, const IniEncoding& encoding, std::vector<unsigned char>& bytes) {
    if (encoding.utf16) {
        bytes.resize(2 + text.size() * sizeof(wchar_t));
        bytes[0] = 0xff; bytes[1] = 0xfe;
        std::memcpy(bytes.data() + 2, text.data(), text.size() * sizeof(wchar_t));
    } else {
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (!length && !text.empty()) return false;
        const std::size_t offset = encoding.bom ? 3 : 0;
        bytes.resize(offset + length);
        if (offset) { bytes[0] = 0xef; bytes[1] = 0xbb; bytes[2] = 0xbf; }
        if (length && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
            reinterpret_cast<char*>(bytes.data() + offset), length, nullptr, nullptr)) return false;
    }
    return bytes.size() <= LAKE_FIX_MAX_INI_BYTES ? true : Error(ERROR_INVALID_DATA);
}

std::wstring Trim(const std::wstring& text) {
    const std::size_t start = text.find_first_not_of(L" \t\r");
    return start == std::wstring::npos ? L"" : text.substr(start, text.find_last_not_of(L" \t\r") - start + 1);
}

struct IniKey { std::wstring name, line; };
struct IniSection { std::wstring name; std::size_t insert; std::vector<IniKey> keys; };

bool IniSections(const std::wstring& text, std::vector<IniSection>& sections) {
    sections.push_back({L"", text.size(), {}});
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t end = text.find(L'\n', at);
        const std::wstring line = Trim(text.substr(at, end == std::wstring::npos ? end : end - at));
        if (!line.empty() && line[0] != L';' && line[0] != L'#') {
            if (line[0] == L'[') {
                const std::size_t close = line.find(L']');
                if (close == std::wstring::npos) return Error(ERROR_INVALID_DATA);
                const std::wstring name = Trim(line.substr(1, close - 1)), tail = Trim(line.substr(close + 1));
                if (name.empty() || (!tail.empty() && tail[0] != L';' && tail[0] != L'#')) return Error(ERROR_INVALID_DATA);
                // Repeated sections have ambiguous Win32 INI semantics; do not
                // "fix" them by appending a second, potentially ignored section.
                for (const auto& section : sections) if (EqualPath(section.name, name)) return Error(ERROR_INVALID_DATA);
                sections.back().insert = at;
                sections.push_back({name, text.size(), {}});
            } else {
                const std::size_t equals = line.find(L'=');
                if (equals != std::wstring::npos && !Trim(line.substr(0, equals)).empty())
                    sections.back().keys.push_back({Trim(line.substr(0, equals)), line});
            }
        }
        if (end == std::wstring::npos) break;
        at = end + 1;
    }
    return true;
}

bool MergeDefaults(const std::vector<unsigned char>& original, const PayloadFile& defaults,
                   std::vector<unsigned char>& merged, bool& changed) {
    std::wstring text, defaultText;
    IniEncoding encoding, defaultEncoding;
    if (!DecodeIni(original.data(), original.size(), text, encoding) ||
        !DecodeIni(defaults.data, defaults.size, defaultText, defaultEncoding)) return false;
    std::vector<IniSection> existing, supplied;
    if (!IniSections(text, existing) || !IniSections(defaultText, supplied)) return false;
    std::vector<std::pair<std::size_t, std::wstring>> insertions;
    for (const auto& section : supplied) {
        const IniSection* target = nullptr;
        for (const auto& candidate : existing) if (EqualPath(candidate.name, section.name)) { target = &candidate; break; }
        std::wstring added;
        for (const auto& key : section.keys) {
            bool present = false;
            if (target) for (const auto& candidate : target->keys) if (EqualPath(candidate.name, key.name)) present = true;
            if (!present) added += key.line + L"\r\n";
        }
        if (added.empty()) continue;
        const std::size_t at = target ? target->insert : text.size();
        if (!target && !section.name.empty()) added = L"[" + section.name + L"]\r\n" + added;
        if (at && text[at - 1] != L'\n') added = L"\r\n" + added;
        insertions.push_back({at, std::move(added)});
    }
    changed = !insertions.empty();
    if (!changed) return true;
    std::stable_sort(insertions.begin(), insertions.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto it = insertions.rbegin(); it != insertions.rend(); ++it) text.insert(it->first, it->second);
    return EncodeIni(text, encoding, merged);
}

std::wstring MigrationText(const std::wstring& receiptHash, const std::wstring& before, const std::wstring& after) {
    return L"\xfeff[recovery]\r\nschema=1\r\naction=config-migration\r\nmanifestSha256=" + receiptHash +
        L"\r\nbeforeSha256=" + before + L"\r\nafterSha256=" + after + L"\r\n";
}

std::wstring BackupPath(const Locations& locations, const std::wstring& before) {
    return Join(locations.installDirectory, (L"LakeWindowedFix.ini.before-" + before + L".bak").c_str());
}

bool RenameLocked(LockedFile& file, const std::wstring& destination) {
    if (!PathSafe(destination, false, true)) return false;
    BY_HANDLE_FILE_INFORMATION current{};
    if (!RegularInfo(file.handle.get(), current) || !SameIdentity(current, file.identity) || HashHandle(file.handle.get()) != file.hash)
        return Error(ERROR_CRC);
    const std::size_t size = sizeof(FILE_RENAME_INFO) + destination.size() * sizeof(wchar_t);
    std::vector<unsigned char> buffer(size, 0);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->ReplaceIfExists = FALSE; // Never replaces a foreign backup or INI.
    rename->FileNameLength = static_cast<DWORD>(destination.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, destination.data(), rename->FileNameLength);
    if (!SetFileInformationByHandle(file.handle.get(), FileRenameInfo, rename, static_cast<DWORD>(size))) return false;
    file.path = destination;
    return true;
}

bool RecoverMigration(const Locations& locations, const std::wstring& receiptHash, bool& migrated) {
    LockedFile journal;
    if (!LockOptional(Join(locations.installDirectory, kMigrationJournalName), true, journal)) return false;
    if (!journal.handle.valid()) return true;
    std::wstring text, before, after, value;
    if (!ReadSmallText(journal.handle.get(), text) || !Field(text, L"beforeSha256", value) || !Digest(value, before) ||
        !Field(text, L"afterSha256", value) || !Digest(value, after) || before == after ||
        text != MigrationText(receiptHash, before, after)) return Error(ERROR_INVALID_DATA);
    LockedFile configuration, backup, stage;
    const std::wstring path = Join(locations.installDirectory, kNames[kConfigurationIndex]);
    if (!LockOptional(path, false, configuration) || !LockOptional(BackupPath(locations, before), false, backup) ||
        !LockOptional(Join(locations.installDirectory, kConfigurationStageName), true, stage)) return false;
    if ((configuration.handle.valid() && configuration.hash != before && configuration.hash != after) ||
        (backup.handle.valid() && backup.hash != before) || (stage.handle.valid() && stage.hash != after)) return Error(ERROR_CRC);
    if (configuration.handle.valid() && configuration.hash == before) {
        if (backup.handle.valid() || !stage.handle.valid()) return Error(ERROR_INVALID_DATA);
        if (!UpgradeDeletion(configuration) || !RenameLocked(configuration, BackupPath(locations, before))) return false;
        // The original bytes/identity now live at the identifiable backup path.
        if (!Checkpoint(locations, L"configuration-backed-up")) return false;
    } else if (!configuration.handle.valid()) {
        if (!backup.handle.valid() || !stage.handle.valid()) return Error(ERROR_INVALID_DATA);
    } else if (!backup.handle.valid() || stage.handle.valid()) return Error(ERROR_INVALID_DATA);
    if (stage.handle.valid()) {
        if (!RenameLocked(stage, path)) return false;
        if (!Checkpoint(locations, L"configuration-published")) return false;
    }
    migrated = true;
    if (!DeleteLocked(journal) || !Checkpoint(locations, L"configuration-journal-removed")) return false;
    return true;
}

bool MigrateConfiguration(const Locations& locations, const std::wstring& receiptHash,
                          const PayloadFile& defaults, bool& migrated) {
    LockedFile configuration;
    if (!LockOptional(Join(locations.installDirectory, kNames[kConfigurationIndex]), false, configuration)) return false;
    if (!configuration.handle.valid()) return true;
    std::vector<unsigned char> original, merged;
    bool changed = false;
    if (!ReadBytes(configuration.handle.get(), original) || !MergeDefaults(original, defaults, merged, changed)) return false;
    if (!changed) return true;
    const std::wstring before = configuration.hash, after = HashMemory(merged.data(), merged.size());
    if (after.empty()) return false;
    const std::wstring backup = BackupPath(locations, before);
    if (!PathSafe(backup, false, true)) return false;
    if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) return Error(ERROR_ALREADY_EXISTS);
    if (!Missing(GetLastError())) return false;
    Transaction preparation;
    if (!WriteNew(preparation, Join(locations.installDirectory, kConfigurationStageName), merged.data(), merged.size(), after) ||
        !WriteTextNew(preparation, Join(locations.installDirectory, kMigrationJournalName), MigrationText(receiptHash, before, after))) return false;
    preparation.committed = true; // Flush stage + journal BEFORE either rename.
    configuration.handle.reset();
    if (!Checkpoint(locations, L"configuration-prepared")) return false;
    return RecoverMigration(locations, receiptHash, migrated);
}

Result PendingFailure(const wchar_t* message, DWORD error) {
    Result result = Failure(message, error);
    result.recoveryPending = true;
    return result;
}

// One retained upgrade generation. No runtime ABI/path change: all user data
// stays beside the active runtime. Immutable archives + rename-only promotion
// permit either roll-forward or explicit rollback after every individual swap.
constexpr std::size_t kUpgradeSlots = kPayloadCount + 5;
constexpr std::size_t kUpgradeManager = 6, kUpgradeDesktop = 7, kUpgradePrograms = 8;
constexpr std::size_t kUpgradeRemoval = 9, kUpgradeReceipt = 10;

std::wstring UpgradePath(const Locations& locations, const wchar_t* name) {
    return Join(Join(locations.installDirectory, kUpgradeDirectoryName), name);
}

std::wstring UpgradeData(const Locations& locations, const wchar_t* role, std::size_t index) {
    const std::wstring name = std::wstring(role) + L"-" + std::to_wstring(index) +
        (index == kConfigurationIndex ? L".ini.bak" : L".bin");
    return UpgradePath(locations, name.c_str());
}

std::wstring ActiveSlot(const Locations& locations, std::size_t index) {
    if (index < kPayloadCount) return Join(locations.installDirectory, kNames[index]);
    if (index == kUpgradeManager) return Join(locations.installDirectory, kManagerName);
    if (index == kUpgradeDesktop) return Join(locations.desktopDirectory, kShortcutName);
    if (index == kUpgradePrograms) return Join(locations.programsDirectory, kShortcutName);
    return Join(locations.installDirectory, index == kUpgradeRemoval ? kRemovalJournalName : kManifestName);
}

struct UpgradePlan {
    LockedFile journal;
    std::array<std::wstring, kUpgradeSlots> before, after;
    std::array<LockedFile, kUpgradeSlots> beforeFiles, afterFiles;
    LockedFile defaults;
    Manifest oldManifest, newManifest;
    bool registryBefore = false;
};

std::wstring UpgradePlanText(const Locations& locations, const UpgradePlan& plan) {
    std::wstring text = L"\xfeff[upgrade]\r\nschema=1\r\nproject=LakeWindowedFix\r\nregistryKey=" + RegistryKey(locations) +
        L"\r\nregisterUninstall=" + (locations.registerUninstall ? std::wstring(L"1") : std::wstring(L"0")) +
        L"\r\nregistryBefore=" + (plan.registryBefore ? std::wstring(L"1") : std::wstring(L"0")) + L"\r\n[slots]\r\n";
    for (std::size_t i = 0; i < kUpgradeSlots; ++i) {
        text += L"before" + std::to_wstring(i) + L"=" + (plan.before[i].empty() ? L"-" : plan.before[i]) + L"\r\n";
        text += L"after" + std::to_wstring(i) + L"=" + (plan.after[i].empty() ? L"-" : plan.after[i]) + L"\r\n";
    }
    return text;
}

bool MatchArchive(const std::wstring& path, const std::wstring& hash, LockedFile& file) {
    if (!LockOptional(path, false, file)) return false;
    return (hash.empty() ? !file.handle.valid() : file.handle.valid() && file.hash == hash) ? true : Error(ERROR_CRC);
}

bool ReadUpgradePlanHeader(const Locations& locations, UpgradePlan& plan) {
    if (!LockFile(UpgradePath(locations, L"plan.ini"), false, plan.journal)) return false;
    std::wstring text, value;
    if (!ReadSmallText(plan.journal.handle.get(), text) || !Field(text, L"registryBefore", value) || (value != L"0" && value != L"1"))
        return Error(ERROR_INVALID_DATA);
    plan.registryBefore = value == L"1";
    if (plan.registryBefore && !locations.registerUninstall) return Error(ERROR_INVALID_DATA);
    for (std::size_t i = 0; i < kUpgradeSlots; ++i) {
        if (!Field(text, L"before" + std::to_wstring(i), value) || (value != L"-" && !Digest(value, plan.before[i]))) return false;
        if (!Field(text, L"after" + std::to_wstring(i), value) || (value != L"-" && !Digest(value, plan.after[i]))) return false;
    }
    if (text != UpgradePlanText(locations, plan) || plan.before[kUpgradeReceipt].empty() || plan.after[kUpgradeReceipt].empty() ||
        !plan.after[kUpgradeRemoval].empty()) return Error(ERROR_INVALID_DATA);
    return true;
}

bool LoadUpgradePlan(const Locations& locations, UpgradePlan& plan) {
    if (!ReadUpgradePlanHeader(locations, plan)) return false;
    std::wstring text;
    for (std::size_t i = 0; i < kUpgradeSlots; ++i)
        if (!MatchArchive(UpgradeData(locations, L"before", i), plan.before[i], plan.beforeFiles[i]) ||
            !MatchArchive(UpgradeData(locations, L"after", i), plan.after[i], plan.afterFiles[i])) return false;
    if (!ReadSmallText(plan.beforeFiles[kUpgradeReceipt].handle.get(), text) || !ParseManifest(text, locations, plan.oldManifest) ||
        !ReadSmallText(plan.afterFiles[kUpgradeReceipt].handle.get(), text) || !ParseManifest(text, locations, plan.newManifest) ||
        plan.newManifest.schema != 2 || plan.newManifest.version != kVersion || !EqualPath(plan.oldManifest.app, plan.newManifest.app))
        return Error(ERROR_INVALID_DATA);
    for (std::size_t i = 0; i <= kUpgradeManager; ++i) {
        if (i == kConfigurationIndex) continue;
        const std::wstring oldHash = i == kUpgradeManager ? plan.oldManifest.managerHash : plan.oldManifest.hashes[i];
        const std::wstring newHash = i == kUpgradeManager ? plan.newManifest.managerHash : plan.newManifest.hashes[i];
        if ((!plan.before[i].empty() && plan.before[i] != oldHash) || plan.after[i] != newHash) return Error(ERROR_CRC);
    }
    for (std::size_t i = 0; i < 2; ++i) {
        const std::size_t slot = kUpgradeDesktop + i;
        const auto& bytes = plan.newManifest.shortcuts[i];
        if (!ShortcutTemplateSafe(bytes, locations, plan.newManifest.app) || plan.after[slot] != HashMemory(bytes.data(), bytes.size())) return Error(ERROR_CRC);
        if (!plan.before[slot].empty()) {
            const auto& old = plan.oldManifest.shortcuts[i];
            if (plan.oldManifest.schema == 2 ? plan.before[slot] != HashMemory(old.data(), old.size()) :
                !ShortcutMatches(plan.beforeFiles[slot].path, locations, plan.oldManifest.app, false)) return Error(ERROR_CRC);
        }
    }
    if (!plan.before[kUpgradeRemoval].empty() &&
        (!ReadSmallText(plan.beforeFiles[kUpgradeRemoval].handle.get(), text) || text != RemovalText(plan.before[kUpgradeReceipt]))) return Error(ERROR_INVALID_DATA);
    if (!LockFile(UpgradePath(locations, L"defaults.ini"), false, plan.defaults) || plan.defaults.hash != plan.newManifest.hashes[kConfigurationIndex])
        return Error(ERROR_CRC);
    std::vector<unsigned char> defaults, original, merged;
    if (!ReadBytes(plan.defaults.handle.get(), defaults)) return false;
    if (plan.before[kConfigurationIndex].empty()) merged = defaults;
    else {
        if (!ReadBytes(plan.beforeFiles[kConfigurationIndex].handle.get(), original)) return false;
        const PayloadFile file{kNames[kConfigurationIndex], defaults.data(), defaults.size(), plan.defaults.hash.c_str()};
        bool changed = false;
        if (!MergeDefaults(original, file, merged, changed)) return false;
        if (!changed) merged = original;
    }
    return !plan.after[kConfigurationIndex].empty() && HashMemory(merged.data(), merged.size()) == plan.after[kConfigurationIndex]
        ? true : Error(ERROR_CRC);
}

std::wstring UpgradeMarkerText(const std::wstring& hash, const wchar_t* phase) {
    return L"\xfeff[upgrade-state]\r\nschema=1\r\nplanSha256=" + hash + L"\r\nphase=" + phase + L"\r\n";
}

bool UpgradeMarker(const Locations& locations, const UpgradePlan& plan, const wchar_t* phase, bool create, bool& present) {
    LockedFile marker;
    const std::wstring path = UpgradePath(locations, (std::wstring(phase) + L".ini").c_str());
    if (!LockOptional(path, false, marker)) return false;
    present = marker.handle.valid();
    const std::wstring expected = UpgradeMarkerText(plan.journal.hash, phase);
    if (!present && create) {
        Transaction transaction;
        if (!WriteTextNew(transaction, path, expected)) return false;
        transaction.committed = true;
        if (!LockFile(path, false, marker)) return false;
        present = true;
    }
    if (!present) return true;
    std::wstring text;
    return ReadSmallText(marker.handle.get(), text) && text == expected ? true : Error(ERROR_INVALID_DATA);
}

bool UpgradePending(const Locations& locations) {
    const std::wstring path = Join(locations.installDirectory, kUpgradeDirectoryName);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return !Missing(GetLastError());
    UpgradePlan plan;
    // Completed history is not a runtime dependency. Missing archived files
    // disable rollback, but must not prevent removal/repair of the active tree.
    if (!ReadUpgradePlanHeader(locations, plan)) return true;
    bool committed = false, rollback = false, reverted = false;
    if (!UpgradeMarker(locations, plan, L"committed", false, committed) || !UpgradeMarker(locations, plan, L"rollback", false, rollback) ||
        !UpgradeMarker(locations, plan, L"reverted", false, reverted)) return true;
    return rollback ? !reverted : !committed;
}

bool CopyArchive(Transaction& transaction, const std::wstring& path, LockedFile& source) {
    return WriteNew(transaction, path, nullptr, 0, source.hash, source.handle.get());
}

bool PrepareUpgrade(const Locations& locations, const Payloads& payloads, HANDLE manager, const std::wstring& managerHash) {
    VerifiedInstallation old;
    if (!ReadReceipt(locations, old) || !VerifyObjects(locations, false, old) || old.preservedFiles) return Error(ERROR_CRC);
    if (old.manifest.schema == 1)
        for (std::size_t i : {kUpgradeDesktop, kUpgradePrograms})
            if (old.files[i].handle.valid() && !ShortcutMatches(old.files[i].path, locations, old.manifest.app, false)) return false;
    LockedFile removal, migration;
    bool unused = false;
    if (!RemovalJournal(locations, old.receipt.hash, false, removal, unused) ||
        !LockOptional(Join(locations.installDirectory, kMigrationJournalName), false, migration)) return false;
    if (migration.handle.valid()) return Error(ERROR_BUSY);
    Manifest next;
    next.app = old.manifest.app;
    next.managerHash = managerHash;
    next.hashes = payloads.hashes;
    for (auto& bytes : next.shortcuts) if (!SerializeShortcut(locations, next.app, bytes)) return false;
    UpgradePlan plan;
    plan.registryBefore = old.registration == RegistrationState::Owned;
    Transaction preparation;
    const std::wstring directory = Join(locations.installDirectory, kUpgradeDirectoryName);
    if (!CreateDirectoryW(directory.c_str(), nullptr) || !RecordDirectory(preparation, directory)) return false;
    for (std::size_t i = 0; i < kUpgradeSlots; ++i) {
        LockedFile& source = i < old.files.size() ? old.files[i] : i == kUpgradeRemoval ? removal : old.receipt;
        if (source.handle.valid()) {
            plan.before[i] = source.hash;
            if (!CopyArchive(preparation, UpgradeData(locations, L"before", i), source) || !Checkpoint(locations, L"upgrade-staging-old")) return false;
        }
        if (i == kUpgradeRemoval) continue;
        std::vector<unsigned char> config;
        std::wstring manifestText;
        const unsigned char* bytes = nullptr;
        std::size_t size = 0;
        HANDLE stream = INVALID_HANDLE_VALUE;
        if (i < kPayloadCount) {
            const PayloadFile& file = *payloads.files[i];
            bytes = file.data; size = file.size;
            if (i == kConfigurationIndex && source.handle.valid()) {
                std::vector<unsigned char> original;
                bool changed = false;
                if (!ReadBytes(source.handle.get(), original) || !MergeDefaults(original, file, config, changed)) return false;
                if (!changed) config = original;
                bytes = config.data(); size = config.size();
            }
        } else if (i == kUpgradeManager) stream = manager;
        else if (i == kUpgradeDesktop || i == kUpgradePrograms) {
            const auto& link = next.shortcuts[i - kUpgradeDesktop]; bytes = link.data(); size = link.size();
        } else {
            manifestText = ManifestText(locations, next);
            bytes = reinterpret_cast<const unsigned char*>(manifestText.data()); size = manifestText.size() * sizeof(wchar_t);
        }
        plan.after[i] = stream == INVALID_HANDLE_VALUE ? HashMemory(bytes, size) : managerHash;
        if (plan.after[i].empty() || !WriteNew(preparation, UpgradeData(locations, L"after", i), bytes, size, plan.after[i], stream)) return false;
        LockedFile archived;
        if (!LockFile(UpgradeData(locations, L"after", i), false, archived) ||
            !CopyArchive(preparation, UpgradeData(locations, L"next", i), archived) || !Checkpoint(locations, L"upgrade-staging-new")) return false;
    }
    const PayloadFile& defaults = *payloads.files[kConfigurationIndex];
    if (!WriteNew(preparation, UpgradePath(locations, L"defaults.ini"), defaults.data, defaults.size, payloads.hashes[kConfigurationIndex])) return false;
    if (locations.registerUninstall && old.manifest.version != next.version) {
        if (!CreateRegistrationAt(preparation, locations, RegistryKey(locations) + L".next", next.version) || !Checkpoint(locations, L"upgrade-staging-registry")) return false;
    }
    if (!WriteTextNew(preparation, UpgradePath(locations, L"plan.ini"), UpgradePlanText(locations, plan))) return false;
    preparation.committed = true;
    return Checkpoint(locations, L"upgrade-prepared");
}

bool RenameRegistration(const Locations& locations, const std::wstring& from, const std::wstring& to,
                        const std::wstring& version) {
    RegistrationState source, destination;
    if (!InspectRegistrationAt(locations, from, version, source) || !InspectRegistrationAt(locations, to, version, destination)) return false;
    if (source != RegistrationState::Owned || destination != RegistrationState::Missing) return Error(ERROR_ALREADY_EXISTS);
    const std::size_t split = from.find_last_of(L'\\');
    if (split == std::wstring::npos || to.substr(0, split + 1) != from.substr(0, split + 1)) return Error(ERROR_INVALID_DATA);
    using Rename = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR);
    const FARPROC address = GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "RegRenameKey");
    Rename rename = nullptr;
    static_assert(sizeof(rename) == sizeof(address), "registry procedure pointer");
    std::memcpy(&rename, &address, sizeof(rename));
    if (!rename) return Error(ERROR_CALL_NOT_IMPLEMENTED);
    HKEY parent = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, from.substr(0, split).c_str(), REG_OPTION_OPEN_LINK,
                                   KEY_READ | KEY_WRITE, &parent);
    if (status != ERROR_SUCCESS) return Error(status);
    status = rename(parent, from.substr(split + 1).c_str(), to.substr(split + 1).c_str());
    if (status == ERROR_SUCCESS) status = FlushRegistration(locations, parent);
    RegCloseKey(parent);
    return status == ERROR_SUCCESS ? true : Error(status);
}

bool RegistryUpgradeState(const Locations& locations, const UpgradePlan& plan, bool rollback) {
    if (!locations.registerUninstall) return true;
    const std::wstring base = RegistryKey(locations);
    const std::wstring oldVersion = plan.oldManifest.version, newVersion = plan.newManifest.version;
    RegistrationState old, next, previous, activeNew, undo;
    if (!InspectRegistrationAt(locations, base, oldVersion, old) || !InspectRegistrationAt(locations, base, newVersion, activeNew) ||
        !InspectRegistrationAt(locations, base + L".next", newVersion, next) ||
        !InspectRegistrationAt(locations, base + L".previous", oldVersion, previous) ||
        !InspectRegistrationAt(locations, base + L".upgraded", newVersion, undo)) return false;
    if (next == RegistrationState::Foreign || previous == RegistrationState::Foreign || undo == RegistrationState::Foreign ||
        (old == RegistrationState::Foreign && activeNew == RegistrationState::Foreign)) return Error(ERROR_INVALID_DATA);
    if (oldVersion == newVersion) {
        if (rollback && !plan.registryBefore) {
            if (activeNew == RegistrationState::Owned && undo == RegistrationState::Owned) return Error(ERROR_ALREADY_EXISTS);
        }
        return true;
    }
    if (rollback && old == RegistrationState::Owned && !plan.registryBefore) return Error(ERROR_INVALID_DATA);
    return true;
}

bool PromoteRegistration(const Locations& locations, const UpgradePlan& plan, bool rollback) {
    if (!locations.registerUninstall) return true;
    const std::wstring base = RegistryKey(locations);
    const std::wstring wanted = rollback ? plan.oldManifest.version : plan.newManifest.version;
    const std::wstring other = rollback ? plan.newManifest.version : plan.oldManifest.version;
    RegistrationState target, activeOther;
    if (!InspectRegistrationAt(locations, base, wanted, target) || !InspectRegistrationAt(locations, base, other, activeOther)) return false;
    const bool wantKey = !rollback || plan.registryBefore;
    if (wantKey && target == RegistrationState::Owned) return true;
    if (!wantKey && target == RegistrationState::Missing) return true;
    const std::wstring retired = base + (rollback ? L".upgraded" : L".previous");
    if (activeOther == RegistrationState::Owned) {
        if (!RenameRegistration(locations, base, retired, other) || !Checkpoint(locations, rollback ? L"rollback-registry-retired" : L"upgrade-registry-retired")) return false;
    } else if (target != RegistrationState::Missing) return Error(ERROR_INVALID_DATA);
    if (!wantKey) return true;
    const std::wstring staged = base + (rollback ? L".previous" : L".next");
    RegistrationState stage;
    if (!InspectRegistrationAt(locations, staged, wanted, stage)) return false;
    if (stage == RegistrationState::Missing) {
        Transaction transaction;
        if (!CreateRegistrationAt(transaction, locations, staged, wanted)) return false;
        transaction.committed = true;
    } else if (stage != RegistrationState::Owned) return Error(ERROR_INVALID_DATA);
    return RenameRegistration(locations, staged, base, wanted) &&
        Checkpoint(locations, rollback ? L"rollback-registry-published" : L"upgrade-registry-published");
}

struct UpgradeSwap {
    LockedFile active, next, previous, restore, undo;
};

bool PreflightUpgrade(const Locations& locations, const UpgradePlan& plan, bool rollback,
                      std::array<UpgradeSwap, kUpgradeSlots>& swaps) {
    for (std::size_t i = 0; i < kUpgradeSlots; ++i) {
        UpgradeSwap& slot = swaps[i];
        if (!LockOptional(ActiveSlot(locations, i), false, slot.active) ||
            !LockOptional(UpgradeData(locations, L"next", i), false, slot.next) ||
            !LockOptional(UpgradeData(locations, L"previous", i), false, slot.previous) ||
            !LockOptional(UpgradeData(locations, L"restore", i), false, slot.restore) ||
            !LockOptional(UpgradeData(locations, L"undo", i), false, slot.undo)) return false;
        const std::wstring& before = plan.before[i];
        const std::wstring& after = plan.after[i];
        if ((slot.active.handle.valid() && slot.active.hash != before && slot.active.hash != after && !(rollback && i == kConfigurationIndex)) ||
            (slot.next.handle.valid() && slot.next.hash != after) || (slot.previous.handle.valid() && slot.previous.hash != before) ||
            (slot.restore.handle.valid() && slot.restore.hash != before) || (slot.undo.handle.valid() && slot.undo.hash != after)) return Error(ERROR_CRC);
        if (!rollback && slot.active.handle.valid() && slot.active.hash != after && slot.previous.handle.valid()) return Error(ERROR_ALREADY_EXISTS);
        if (rollback && i != kConfigurationIndex && slot.active.handle.valid() && slot.active.hash != before && slot.undo.handle.valid()) return Error(ERROR_ALREADY_EXISTS);
    }
    return RegistryUpgradeState(locations, plan, rollback);
}

bool SwapUpgradeSlot(const Locations& locations, UpgradePlan& plan, UpgradeSwap& slot, std::size_t index, bool rollback) {
    if (rollback && index == kConfigurationIndex && slot.active.handle.valid()) return true; // Persistent user data, not a binary generation.
    const std::wstring& target = rollback ? plan.before[index] : plan.after[index];
    if (slot.active.handle.valid() && slot.active.hash == target) return true;
    if (!slot.active.handle.valid() && target.empty()) return true;
    LockedFile& staging = rollback ? slot.restore : slot.next;
    LockedFile& retired = rollback ? slot.undo : slot.previous;
    const wchar_t* role = rollback ? L"restore" : L"next";
    if (!target.empty() && !staging.handle.valid()) {
        Transaction transaction;
        LockedFile& archive = rollback ? plan.beforeFiles[index] : plan.afterFiles[index];
        if (!CopyArchive(transaction, UpgradeData(locations, role, index), archive)) return false;
        transaction.committed = true;
        if (!LockFile(UpgradeData(locations, role, index), false, staging)) return false;
    }
    if (slot.active.handle.valid()) {
        if (retired.handle.valid() || !UpgradeDeletion(slot.active) ||
            !RenameLocked(slot.active, UpgradeData(locations, rollback ? L"undo" : L"previous", index))) return false;
        if (!Checkpoint(locations, rollback ? L"rollback-active-retired" : L"upgrade-active-retired")) return false;
    }
    if (!target.empty()) {
        if (!UpgradeDeletion(staging) || !RenameLocked(staging, ActiveSlot(locations, index))) return false;
        if (!Checkpoint(locations, rollback ? L"rollback-active-published" : L"upgrade-active-published")) return false;
    }
    return true;
}

bool VerifyUpgradeTarget(const Locations& locations, const UpgradePlan& plan, bool rollback) {
    for (std::size_t i = 0; i < kUpgradeSlots; ++i) {
        LockedFile file;
        if (!LockOptional(ActiveSlot(locations, i), false, file)) return false;
        const std::wstring& hash = rollback ? plan.before[i] : plan.after[i];
        if (rollback && i == kConfigurationIndex && file.handle.valid()) continue;
        if (hash.empty() ? file.handle.valid() : !file.handle.valid() || file.hash != hash) return Error(ERROR_CRC);
    }
    RegistrationState registration;
    if (!InspectRegistration(locations, rollback ? plan.oldManifest.version : plan.newManifest.version, registration)) return false;
    const bool required = locations.registerUninstall && (!rollback || plan.registryBefore);
    return registration == (required ? RegistrationState::Owned : RegistrationState::Missing) ? true : Error(ERROR_INVALID_DATA);
}

Result ResumeUpgrade(const Locations& locations, UpgradePlan& plan, bool rollback) {
    bool committed = false, intent = false, reverted = false;
    if (!UpgradeMarker(locations, plan, L"committed", false, committed) || !UpgradeMarker(locations, plan, L"rollback", false, intent) ||
        !UpgradeMarker(locations, plan, L"reverted", false, reverted)) return PendingFailure(L"Marcador de atualização desconhecido. Nada foi substituído.", LastErrorOr());
    if (!rollback && intent) {
        if (reverted) {
            Result result = Failure(L"Rollback já concluído. O histórico foi preservado; reaplicar/rotacionar esta geração ainda não é suportado.", ERROR_NOT_SUPPORTED);
            result.updateBlocked = true;
            return result;
        }
        return PendingFailure(L"Rollback solicitado. Conclua com --rollback antes de outra operação.", ERROR_BUSY);
    }
    if ((!rollback && committed) || (rollback && reverted)) {
        if (!VerifyUpgradeTarget(locations, plan, rollback)) return Failure(L"O estado concluído foi alterado. Os arquivos atuais e o histórico foram preservados.", LastErrorOr());
        Result result = Success(rollback ? L"A versão anterior já foi restaurada. Configuração e logs mantidos." : L"A atualização já foi concluída.");
        result.alreadyInstalled = true;
        result.rolledBack = rollback; result.upgraded = !rollback;
        return result;
    }
    {
        std::array<UpgradeSwap, kUpgradeSlots> swaps;
        if (!PreflightUpgrade(locations, plan, rollback, swaps))
            return PendingFailure(L"Há arquivo/atalho/registro alterado no estado ativo ou no staging. Foi preservado; promoção/rollback recusado.", LastErrorOr());
        Result stopped = CheckStopped();
        if (!stopped.ok) return stopped;
        Transaction directories;
        for (const std::wstring& directory : {locations.desktopDirectory, locations.programsDirectory})
            if (!EnsureDirectory(directories, directory))
                return PendingFailure(L"Não foi possível recriar a pasta de um atalho ausente.", LastErrorOr());
        directories.committed = true;
        if (rollback && !intent) {
            if (!UpgradeMarker(locations, plan, L"rollback", true, intent) || !Checkpoint(locations, L"rollback-requested"))
                return PendingFailure(L"Pedido de rollback registrado. Execute --rollback novamente para continuar.", LastErrorOr());
        }
        // Runtime/INI/links first; registry, manager, removal intent and receipt
        // last. Every rename is no-replace and every original stays archived.
        for (std::size_t i = 0; i < kUpgradeSlots; ++i) {
            if (i == kUpgradeManager || i >= kUpgradeRemoval) continue;
            if (!SwapUpgradeSlot(locations, plan, swaps[i], i, rollback))
                return PendingFailure(L"Troca interrompida. Mantenha o Lake fechado; retome --upgrade ou solicite --rollback.", LastErrorOr());
        }
        if (!PromoteRegistration(locations, plan, rollback))
            return PendingFailure(L"Troca do registro interrompida; manager e recibos permanecem recuperáveis.", LastErrorOr());
        for (std::size_t i : {kUpgradeManager, kUpgradeRemoval, kUpgradeReceipt})
            if (!SwapUpgradeSlot(locations, plan, swaps[i], i, rollback))
                return PendingFailure(L"Confirmação de versão interrompida. Use --upgrade/--rollback para recuperar.", LastErrorOr());
    }
    if (!VerifyUpgradeTarget(locations, plan, rollback)) return PendingFailure(L"A verificação da versão promovida falhou. Histórico anterior mantido.", LastErrorOr());
    bool complete = false;
    if (!UpgradeMarker(locations, plan, rollback ? L"reverted" : L"committed", true, complete) ||
        !Checkpoint(locations, rollback ? L"rollback-committed" : L"upgrade-committed"))
        return PendingFailure(L"A troca terminou, mas a confirmação foi interrompida. Execute a mesma operação novamente.", LastErrorOr());
    Result result = Success(rollback ? L"Versão anterior restaurada. INI atual, logs e histórico da atualização preservados."
                                    : L"Atualização concluída. Configuração migrada sem trocar edições; versão anterior preservada para --rollback.");
    result.upgraded = !rollback; result.rolledBack = rollback;
    result.preservedConfiguration = true;
    return result;
}

bool ManagerIsExternal(const Locations& locations) {
    std::vector<wchar_t> module(32768);
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    std::wstring path;
    if (!length || length >= module.size() || !Normalize(std::wstring(module.data(), length), path)) return false;
    return !Within(path, locations.installDirectory) ? true : Error(ERROR_BUSY);
}

void NotifyShortcuts(const Locations& locations, LONG event) {
    // Sandbox callers explicitly opt out of registry AND global shell events.
    if (!locations.registerUninstall || !locations.testRegistryKey.empty()) return;
    for (const std::wstring& directory : {locations.desktopDirectory, locations.programsDirectory})
        SHChangeNotify(event, SHCNF_PATHW, Join(directory, kShortcutName).c_str(), nullptr);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path);
    std::wstring result;
    if (SUCCEEDED(hr) && path) result = path;
    if (path) CoTaskMemFree(path);
    if (FAILED(hr)) SetLastError(static_cast<DWORD>(hr));
    return result;
}
} // namespace

const SupportedBuild& LakeBuild() {
    static const SupportedBuild build{
        LAKE_FIX_WIDEN(LAKE_BUILD007_EXE_SHA256),
        LAKE_FIX_WIDEN(LAKE_BUILD007_CONTOUR_SHA256)
    };
    return build;
}

Locations DefaultLocations() {
    Locations locations;
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    locations.desktopDirectory = KnownFolder(FOLDERID_Desktop);
    locations.programsDirectory = KnownFolder(FOLDERID_Programs);
    if (!local.empty()) locations.installDirectory = local + L"\\LakeWindowedFix\\v0.6";
    return locations;
}

std::wstring FileSha256(const std::wstring& path) {
    std::wstring normalized;
    if (!Normalize(path, normalized)) return {};
    Handle file;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!OpenRegular(normalized, GENERIC_READ, file, info)) return {};
    return HashHandle(file.get());
}

bool LakeIsRunning() {
    bool running = false;
    // Fail closed when the process inventory cannot be read; never infer that
    // it is safe to modify an installation from an unavailable snapshot.
    return !Running(running) || running;
}

Result ValidateApplication(const std::wstring& appDirectory, const SupportedBuild& build) {
    Handle executable, renderer;
    return ReadApplication(appDirectory, build, executable, renderer);
}

Result VerifyPayload(const PayloadFile* files, std::size_t count) {
    Payloads checked;
    return CheckPayloads(files, count, checked) ? Success(L"Os seis payloads embutidos têm nomes e SHA-256 válidos.")
        : Failure(L"Payload embutido inválido: nome, contagem ou SHA-256 não confere.", LastErrorOr());
}

Result Upgrade(const Locations& input, const std::wstring& appDirectory,
               const PayloadFile* files, std::size_t count, const std::wstring& setupExecutable, const SupportedBuild& build) {
    Result stopped = CheckStopped();
    if (!stopped.ok) return stopped;
    Payloads payloads;
    if (!CheckPayloads(files, count, payloads)) return Failure(L"Payload da atualização inválido.", LastErrorOr());
    std::wstring app, setup, selectedApp = appDirectory;
    Locations locations;
    if (!CheckLocations(input, L"", locations)) return Failure(L"Destinos da atualização inválidos.", LastErrorOr());
    if (selectedApp.empty()) {
        const DWORD attributes = GetFileAttributesW(UpgradePath(locations, L"plan.ini").c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            UpgradePlan recorded;
            if (!LoadUpgradePlan(locations, recorded)) return Failure(L"Não foi possível ler o Lake registrado no journal.", LastErrorOr());
            selectedApp = recorded.newManifest.app;
        } else {
            VerifiedInstallation recorded;
            if (!ReadReceipt(locations, recorded)) return Failure(L"Não foi possível ler a pasta original do recibo.", LastErrorOr());
            selectedApp = recorded.manifest.app;
        }
    }
    if (!Normalize(selectedApp, app) || !Normalize(setupExecutable, setup)) return Failure(L"Caminho da atualização inválido.", LastErrorOr());
    Handle executable, renderer, manager;
    Result application = ReadApplication(app, build, executable, renderer);
    if (!application.ok) return application;
    if (!CheckLocations(input, app, locations) || !ManagerIsExternal(locations) || Within(setup, locations.installDirectory) || Within(setup, app))
        return Failure(L"Execute o setup novo fora da pasta instalada/original para atualizar; mantenha o Lake fechado.", LastErrorOr(ERROR_BUSY));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!OpenRegular(setup, GENERIC_READ, manager, info)) return Failure(L"Não foi possível ler o setup novo.", LastErrorOr());
    const std::wstring managerHash = HashHandle(manager.get());
    if (managerHash.empty() || EqualPath(managerHash, build.executableSha256) || EqualPath(managerHash, build.rendererSha256))
        return Failure(L"O gerenciador da atualização é inválido.", ERROR_INVALID_DATA);
    Apartment apartment;
    if (!apartment.ok()) return Failure(L"Não foi possível preparar os atalhos.", apartment.error());
    DirectoryPins pins;
    for (const std::wstring& directory : {locations.installDirectory, locations.desktopDirectory, locations.programsDirectory})
        if (!pins.existingAncestors(directory)) return Failure(L"Não foi possível proteger os caminhos da atualização.", LastErrorOr());
    if (!TreeSafe(locations.installDirectory)) return Failure(L"A pasta da atualização contém caminho inseguro.", LastErrorOr());
    const std::wstring directory = Join(locations.installDirectory, kUpgradeDirectoryName);
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (!Missing(GetLastError())) return Failure(L"Não foi possível verificar o staging.", LastErrorOr());
        {
            VerifiedInstallation old;
            if (!ReadReceipt(locations, old) || !EqualPath(old.manifest.app, app)) return Failure(L"Recibo anterior inválido ou pertencente a outro Lake.", LastErrorOr());
            if (old.manifest.schema == 2 && old.manifest.version == kVersion && old.manifest.managerHash == managerHash &&
                old.manifest.hashes == payloads.hashes) {
                // Do not consume the one retained rollback generation for a
                // no-op. Ordinary repair already handles this exact package.
                old.receipt.handle.reset();
                return Install(input, app, files, count, setupExecutable, build);
            }
        }
        if (!PrepareUpgrade(locations, payloads, manager.get(), managerHash))
            return PendingFailure(L"Preparação da atualização interrompida. A versão ativa não foi substituída; execute --upgrade novamente.", LastErrorOr());
    }
    if (!pins.existingAncestors(directory)) return Failure(L"Não foi possível proteger o journal da atualização.", LastErrorOr());
    UpgradePlan plan;
    if (!LoadUpgradePlan(locations, plan)) return PendingFailure(L"Journal/arquivos da atualização desconhecidos ou alterados. Nada foi adotado.", LastErrorOr());
    if (!EqualPath(plan.newManifest.app, app) || plan.newManifest.managerHash != managerHash || plan.newManifest.hashes != payloads.hashes)
        return Failure(L"Este histórico pertence a outro pacote. Use o mesmo setup para retomar ou --rollback para reverter.", ERROR_REVISION_MISMATCH);
    return ResumeUpgrade(locations, plan, false);
}

Result Rollback(const Locations& input) {
    Result stopped = CheckStopped();
    if (!stopped.ok) return stopped;
    Locations locations;
    if (!CheckLocations(input, L"", locations) || !ManagerIsExternal(locations))
        return Failure(L"Execute --rollback pelo setup externo, fora da pasta instalada.", LastErrorOr(ERROR_BUSY));
    Apartment apartment;
    if (!apartment.ok()) return Failure(L"Não foi possível verificar os atalhos.", apartment.error());
    DirectoryPins pins;
    for (const std::wstring& directory : {locations.installDirectory, locations.desktopDirectory, locations.programsDirectory,
                                        Join(locations.installDirectory, kUpgradeDirectoryName)})
        if (!pins.existingAncestors(directory)) return Failure(L"Não foi possível proteger os caminhos do rollback.", LastErrorOr());
    if (!TreeSafe(locations.installDirectory)) return Failure(L"Rollback recusado: caminho inseguro.", LastErrorOr());
    UpgradePlan plan;
    if (!LoadUpgradePlan(locations, plan)) return Failure(L"Não há histórico de atualização íntegro para reverter.", LastErrorOr());
    return ResumeUpgrade(locations, plan, true);
}

Result Install(const Locations& input, const std::wstring& appDirectory,
               const PayloadFile* files, std::size_t count,
               const std::wstring& setupExecutable, const SupportedBuild& build) {
    Result stopped = CheckStopped();
    if (!stopped.ok) return stopped;
    Payloads payloads;
    if (!CheckPayloads(files, count, payloads))
        return Failure(L"Os arquivos do instalador são inválidos ou estão corrompidos.", LastErrorOr());
    std::wstring app, setup;
    if (!Normalize(appDirectory, app)) return Failure(L"A pasta original deve ser um caminho local absoluto, sem '..'.", LastErrorOr());
    // Keep both read-only original handles throughout installation so the build
    // cannot be replaced after it has been checked. No app directory is written.
    Handle originalExecutable, originalRenderer;
    Result application = ReadApplication(app, build, originalExecutable, originalRenderer);
    if (!application.ok) return application;
    Locations locations;
    if (!CheckLocations(input, app, locations))
        return Failure(L"As pastas de instalação e atalhos devem ser distintas, sem links e fora da pasta original, do Windows e de Program Files.", LastErrorOr());
    if (UpgradePending(locations)) return PendingFailure(L"Há atualização/rollback pendente. Use --upgrade ou --rollback antes de instalar/reparar.", ERROR_BUSY);
    if (!Normalize(setupExecutable, setup)) return Failure(L"O caminho do instalador é inválido.", LastErrorOr());
    if (Within(setup, app))
        return Failure(L"O gerenciador deve ser o próprio instalador, fora da pasta original do Lake. Nenhum arquivo proprietário será copiado.", ERROR_BAD_PATHNAME);
    Handle source;
    BY_HANDLE_FILE_INFORMATION sourceInfo{};
    if (!OpenRegular(setup, GENERIC_READ, source, sourceInfo))
        return Failure(L"Não foi possível ler o próprio instalador.", LastErrorOr());
    const std::wstring managerHash = HashHandle(source.get());
    if (managerHash.empty()) return Failure(L"Não foi possível verificar o próprio instalador.", LastErrorOr());
    if (EqualPath(managerHash, build.executableSha256) || EqualPath(managerHash, build.rendererSha256))
        return Failure(L"Um arquivo original do Lake não pode ser usado como gerenciador de instalação.", ERROR_INVALID_DATA);
    Apartment apartment;
    if (!apartment.ok()) return Failure(L"Não foi possível preparar os atalhos do Windows.", apartment.error());

    Transaction transaction;
    for (const std::wstring& directory : {locations.installDirectory, locations.desktopDirectory, locations.programsDirectory})
        if (!transaction.pins.existingAncestors(directory))
            return Failure(L"Não foi possível proteger as pastas contra alterações durante a instalação.", LastErrorOr());

    const DWORD attributes = GetFileAttributesW(locations.installDirectory.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        VerifiedInstallation existing;
        if (!TreeSafe(locations.installDirectory) || !ReadReceipt(locations, existing))
            return Failure(L"A pasta já existe sem um recibo de propriedade válido. Nada foi adotado ou sobrescrito.", LastErrorOr());
        if (!EqualPath(existing.manifest.app, app))
            return Failure(L"Esta instalação pertence a outra pasta original do Lake. Nada foi alterado.");
        bool compatible = existing.manifest.version == kVersion && existing.manifest.schema == 2 &&
            existing.manifest.managerHash == managerHash;
        for (std::size_t i = 0; i < kPayloadCount; ++i)
            if (i != kConfigurationIndex && existing.manifest.hashes[i] != payloads.hashes[i]) compatible = false;
        if (!compatible) {
            Result result = Failure(L"Este pacote é diferente da instalação anterior. Use --upgrade explicitamente para atualizar com histórico e rollback.", ERROR_REVISION_MISMATCH);
            result.updateBlocked = true;
            return result;
        }
        if (!VerifyObjects(locations, false, existing))
            return Failure(L"Não foi possível proteger os arquivos existentes. Nada foi sobrescrito.", LastErrorOr());
        if (existing.preservedFiles) {
            Result result = Failure(L"Reparo recusado: há arquivo, atalho ou registro alterado/desconhecido. Foi preservado; somente ausências podem ser recriadas.", ERROR_CRC);
            result.message += PreservedSummary(existing);
            result.preservedFiles = true;
            return result;
        }
        LockedFile removal;
        bool created = false;
        if (!RemovalJournal(locations, existing.receipt.hash, false, removal, created))
            return Failure(L"O marcador de remoção não corresponde ao recibo. Foi preservado.", LastErrorOr());
        bool repaired = removal.handle.valid() || (locations.registerUninstall && existing.registration == RegistrationState::Missing);
        for (const auto& file : existing.files) if (!file.handle.valid()) repaired = true;
        stopped = CheckStopped();
        if (!stopped.ok) return stopped;
        // The config swap owns its own durable journal. Release only this read
        // pin; all other existing objects and the receipt stay protected.
        existing.files[kConfigurationIndex].handle.reset();
        bool migrated = false;
        if (!RecoverMigration(locations, existing.receipt.hash, migrated) ||
            !MigrateConfiguration(locations, existing.receipt.hash, *payloads.files[kConfigurationIndex], migrated))
            return PendingFailure(L"A configuração não pôde ser migrada com segurança. Originais/backup e journal foram preservados; execute novamente para recuperar.", LastErrorOr());
        if (!LockOptional(Join(locations.installDirectory, kNames[kConfigurationIndex]), false, existing.files[kConfigurationIndex]))
            return PendingFailure(L"Não foi possível verificar a configuração recuperada.", LastErrorOr());
        for (std::size_t i = 0; i <= kPayloadCount; ++i) {
            if (existing.files[i].handle.valid()) continue;
            const bool manager = i == kPayloadCount;
            const PayloadFile* file = manager ? nullptr : payloads.files[i];
            if (!WriteNew(transaction, Join(locations.installDirectory, manager ? kManagerName : kNames[i]),
                manager ? nullptr : file->data, manager ? 0 : file->size,
                manager ? managerHash : payloads.hashes[i], manager ? source.get() : INVALID_HANDLE_VALUE))
                return Failure(L"O reparo não pôde recriar uma ausência. Somente os novos arquivos inalterados serão revertidos.", LastErrorOr());
        }
        const std::array<std::wstring, 2> directories = {{locations.desktopDirectory, locations.programsDirectory}};
        for (std::size_t i = 0; i < directories.size(); ++i) {
            if (existing.files[kPayloadCount + 1 + i].handle.valid()) continue;
            const auto& bytes = existing.manifest.shortcuts[i];
            const std::wstring hash = HashMemory(bytes.data(), bytes.size());
            if (hash.empty() || !EnsureDirectory(transaction, directories[i]) ||
                !WriteNew(transaction, Join(directories[i], kShortcutName), bytes.data(), bytes.size(), hash))
                return Failure(L"O reparo não pôde criar um atalho ausente. Nenhum atalho existente foi substituído.", LastErrorOr());
        }
        if (existing.registration == RegistrationState::Missing && !CreateRegistration(transaction, locations))
            return Failure(L"Não foi possível recriar o registro ausente; os novos arquivos serão revertidos.", LastErrorOr());
        VerifiedInstallation complete;
        if (!ReadReceipt(locations, complete) || !VerifyObjects(locations, false, complete) || !InstallationComplete(locations, complete))
            return Failure(L"A verificação do reparo falhou; apenas novas criações inalteradas serão revertidas.", LastErrorOr());
        stopped = CheckStopped();
        if (!stopped.ok) return stopped;
        transaction.committed = true;
        // Removal intent is cleared LAST, only after a fully verified reinstall.
        // A failure here leaves either an installed tree + marker, or a complete
        // installed tree without marker; both are safely recognized next time.
        if (removal.handle.valid() && (!DeleteLocked(removal) || !Checkpoint(locations, L"removal-marker-cleared")))
            return PendingFailure(L"Os arquivos foram reinstalados, mas a confirmação final foi interrompida. Execute novamente.", LastErrorOr());
        Result result = Success(repaired || migrated
            ? L"Instalação recuperada. Apenas arquivos/atalhos ausentes foram recriados; edições e dados pessoais foram preservados."
            : L"O modo janela já está instalado. A configuração existente foi mantida.");
        result.alreadyInstalled = !repaired && !migrated;
        result.repaired = repaired || migrated;
        result.configurationMigrated = migrated;
        result.preservedConfiguration = complete.editedConfiguration;
        if (result.repaired) NotifyShortcuts(locations, SHCNE_CREATE);
        return result;
    }
    if (!Missing(GetLastError())) return Failure(L"Não foi possível verificar a pasta de instalação.", LastErrorOr());

    if (!EnsureDirectory(transaction, Parent(locations.installDirectory)) ||
        !CreateDirectoryW(locations.installDirectory.c_str(), nullptr) ||
        !RecordDirectory(transaction, locations.installDirectory))
        return Failure(L"Não foi possível criar uma pasta exclusiva para o modo janela.", LastErrorOr());
    for (std::size_t i = 0; i < kPayloadCount; ++i) {
        const PayloadFile& file = *payloads.files[i];
        if (!WriteNew(transaction, Join(locations.installDirectory, kNames[i]), file.data, file.size, payloads.hashes[i]))
            return Failure(L"Não foi possível gravar os arquivos próprios. A instalação foi cancelada sem sobrescrever arquivos existentes.", LastErrorOr());
    }
    if (!WriteNew(transaction, Join(locations.installDirectory, kManagerName), nullptr, 0, managerHash, source.get()))
        return Failure(L"Não foi possível copiar o gerenciador de instalação.", LastErrorOr());
    Manifest manifest;
    manifest.app = app;
    manifest.managerHash = managerHash;
    manifest.hashes = payloads.hashes;
    const std::array<std::wstring, 2> directories = {{locations.desktopDirectory, locations.programsDirectory}};
    for (std::size_t i = 0; i < directories.size(); ++i) {
        auto& bytes = manifest.shortcuts[i];
        if (!EnsureDirectory(transaction, directories[i]) || !SerializeShortcut(locations, app, bytes) ||
            !WriteNew(transaction, Join(directories[i], kShortcutName), bytes.data(), bytes.size(), HashMemory(bytes.data(), bytes.size())))
            return Failure(L"Não foi possível criar os atalhos. Um atalho já existente nunca será substituído.", LastErrorOr());
    }
    if (!WriteTextNew(transaction, Join(locations.installDirectory, kManifestName), ManifestText(locations, manifest)))
        return Failure(L"Não foi possível gravar o recibo de propriedade da instalação.", LastErrorOr());
    if (!CreateRegistration(transaction, locations))
        return Failure(L"Não foi possível registrar a remoção para este usuário. Um registro preexistente não foi substituído.", LastErrorOr());
    VerifiedInstallation installed;
    if (!ReadReceipt(locations, installed) || !VerifyObjects(locations, false, installed) || !InstallationComplete(locations, installed))
        return Failure(L"A verificação final falhou. Somente arquivos próprios ainda inalterados podem ser revertidos.", LastErrorOr());
    stopped = CheckStopped();
    if (!stopped.ok) return stopped;
    transaction.committed = true;
    NotifyShortcuts(locations, SHCNE_CREATE);
    return Success(L"Modo janela instalado. Use o atalho Lake Controller - Janela. O Lake original não foi alterado nem iniciado.");
}

Result Uninstall(const Locations& input, const PayloadFile* files, std::size_t count) {
    Result stopped = CheckStopped();
    if (!stopped.ok) return stopped;
    Payloads payloads;
    if (!CheckPayloads(files, count, payloads)) return Failure(L"A identificação dos arquivos próprios é inválida.", LastErrorOr());
    Locations locations;
    if (!CheckLocations(input, L"", locations)) return Failure(L"As pastas de remoção são inválidas ou se sobrepõem.", LastErrorOr());
    if (UpgradePending(locations)) return PendingFailure(L"Há atualização/rollback pendente. Recupere com --upgrade ou --rollback antes de remover.", ERROR_BUSY);
    std::vector<wchar_t> module(32768);
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    std::wstring executable;
    if (!length || length >= module.size() || !Normalize(std::wstring(module.data(), length), executable))
        return Failure(L"Não foi possível verificar o local do gerenciador de remoção.", LastErrorOr());
    if (Within(executable, locations.installDirectory))
        return Failure(L"A remoção deve ser executada pela cópia temporária do instalador, fora da pasta instalada.", ERROR_BUSY);
    Apartment apartment;
    if (!apartment.ok()) return Failure(L"Não foi possível verificar os atalhos do Windows.", apartment.error());
    DirectoryPins pins;
    if (!pins.existingAncestors(locations.installDirectory, true) ||
        !pins.existingAncestors(locations.desktopDirectory) || !pins.existingAncestors(locations.programsDirectory))
        return Failure(L"Não foi possível proteger as pastas contra alterações durante a remoção. Nada foi removido.", LastErrorOr());
    VerifiedInstallation installation;
    if (!TreeSafe(locations.installDirectory) || !ReadReceipt(locations, installation))
        return Failure(L"A remoção foi recusada: recibo de propriedade inválido ou caminho inseguro. Nada foi removido.", LastErrorOr());
    LockedFile journal;
    bool created = false;
    if (!RemovalJournal(locations, installation.receipt.hash, false, journal, created))
        return Failure(L"O marcador de remoção é desconhecido e foi preservado. Nada foi removido.", LastErrorOr());
    bool migrated = false;
    if (!RecoverMigration(locations, installation.receipt.hash, migrated))
        return PendingFailure(L"Há uma migração de configuração pendente. O backup e os journals foram mantidos; recupere antes de remover.", LastErrorOr());
    if (!VerifyObjects(locations, true, installation))
        return Failure(L"Não foi possível proteger os arquivos para remoção. Nada foi removido.", LastErrorOr());
    // No original application hash is required here: removing our files must
    // remain possible after the original application has been moved/deleted.
    stopped = CheckStopped();
    if (!stopped.ok) return stopped;
    if (!TreeSafe(locations.installDirectory))
        return Failure(L"A instalação mudou durante a verificação. Nada foi removido.", LastErrorOr());

    if (!journal.handle.valid()) {
        if (!RemovalJournal(locations, installation.receipt.hash, true, journal, created))
            return Failure(L"Não foi possível gravar o journal de remoção. Nada foi removido.", LastErrorOr());
        if (!Checkpoint(locations, L"removal-prepared"))
            return PendingFailure(L"Remoção interrompida antes das exclusões. Execute novamente para retomar.", LastErrorOr());
    }
    // The immutable receipt and intent marker are NEVER removed by uninstall.
    // All unchanged objects are pinned; absence is accepted on every retry.
    // Keep the manager until files, shortcuts AND registration are processed.
    for (std::size_t i = 0; i < installation.files.size(); ++i) {
        if (i == kPayloadCount) continue;
        LockedFile& file = installation.files[i];
        if (file.handle.valid() && file.remove && (!DeleteLocked(file) || !Checkpoint(locations, L"owned-file-removed")))
            return PendingFailure(L"Remoção parcial: recibo, journal e gerenciador foram mantidos. Execute novamente; ausências não bloqueiam a recuperação.", LastErrorOr());
    }
    if (locations.registerUninstall) {
        // A foreign/modified value or a new subkey is never recursively removed.
        RegistrationState state;
        if (!InspectRegistration(locations, installation.manifest.version, state))
            return PendingFailure(L"O registro não pôde ser verificado. O gerenciador e o journal foram mantidos.", LastErrorOr());
        if (state == RegistrationState::Foreign) installation.preservedFiles = true;
        if (state == RegistrationState::Owned) {
            const LSTATUS status = RegDeleteKeyW(HKEY_CURRENT_USER, RegistryKey(locations).c_str());
            if (status != ERROR_SUCCESS && !Missing(status))
                return PendingFailure(L"O registro de remoção foi preservado; o gerenciador permite tentar novamente.", status);
        }
    }
    LockedFile& manager = installation.files[kPayloadCount];
    if (manager.handle.valid() && manager.remove && (!DeleteLocked(manager) || !Checkpoint(locations, L"manager-removed-last")))
        return PendingFailure(L"A etapa final da remoção foi interrompida. Recibo e journal permitem repetir com segurança.", LastErrorOr());
    NotifyShortcuts(locations, SHCNE_DELETE);
    Result result = Success(installation.preservedFiles
        ? L"Modo janela removido parcialmente: arquivos/atalhos/registro alterados foram preservados. Recibo e journal permitem nova tentativa ou reinstalação compatível."
        : L"Modo janela removido. INI editado, logs e arquivos pessoais foram preservados. O recibo e o marcador permanecem para reinstalação/recuperação; o Lake original não foi alterado.");
    result.preservedConfiguration = installation.editedConfiguration;
    result.preservedFiles = installation.preservedFiles;
    result.message += PreservedSummary(installation);
    return result;
}
} // namespace lakefix::setup
