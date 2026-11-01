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

bool CheckLocations(const Locations& input, const std::wstring& app, Locations& locations) {
    locations.registerUninstall = input.registerUninstall;
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
};

std::wstring ManifestText(const Locations& locations, const Manifest& manifest, const Payloads& payloads) {
    std::wstring text = L"\xfeff[setup]\r\nschema=1\r\nproject=";
    text += kProject;
    text += L"\r\nversion="; text += kVersion;
    text += L"\r\nappDirectory=" + manifest.app;
    text += L"\r\ninstallDirectory=" + locations.installDirectory;
    text += L"\r\ndesktopDirectory=" + locations.desktopDirectory;
    text += L"\r\nprogramsDirectory=" + locations.programsDirectory;
    text += locations.registerUninstall ? L"\r\nregisterUninstall=1" : L"\r\nregisterUninstall=0";
    text += L"\r\nfileCount=7\r\n[files]\r\n";
    for (std::size_t i = 0; i < kPayloadCount; ++i) text += std::wstring(kNames[i]) + L"=" + payloads.hashes[i] + L"\r\n";
    text += std::wstring(kManagerName) + L"=" + manifest.managerHash + L"\r\n";
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

bool ParseManifest(const std::wstring& text, const Locations& locations, const Payloads& payloads,
                   Manifest& manifest) {
    // Canonical round-trip rejects duplicate/unknown fields, alternate sections,
    // path-bearing file names, a changed count and trailing hidden INI content.
    const std::wstring appKey = L"\r\nappDirectory=";
    const std::wstring managerKey = std::wstring(L"\r\n") + kManagerName + L"=";
    const std::size_t appAt = text.find(appKey);
    const std::size_t managerAt = text.find(managerKey);
    if (appAt == std::wstring::npos || managerAt == std::wstring::npos) return Error(ERROR_INVALID_DATA);
    const std::size_t appStart = appAt + appKey.size();
    const std::size_t appEnd = text.find(L"\r\n", appStart);
    const std::size_t hashStart = managerAt + managerKey.size();
    const std::size_t hashEnd = text.find(L"\r\n", hashStart);
    if (appEnd == std::wstring::npos || hashEnd == std::wstring::npos) return Error(ERROR_INVALID_DATA);
    const std::wstring recordedApp = text.substr(appStart, appEnd - appStart);
    // The original directory (or even its drive) need not still exist when
    // uninstalling. Validate this recorded path lexically, without opening it.
    if (!Normalize(recordedApp, manifest.app, false) || !EqualPath(recordedApp, manifest.app) ||
        !Digest(text.substr(hashStart, hashEnd - hashStart), manifest.managerHash)) return Error(ERROR_INVALID_DATA);
    if (text != ManifestText(locations, manifest, payloads)) return Error(ERROR_INVALID_DATA);
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

bool ShortcutMatches(const std::wstring& path, const Locations& locations, const std::wstring& app) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> persistence;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  reinterpret_cast<void**>(link.out()));
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(persistence.out()));
    if (SUCCEEDED(hr)) hr = persistence->Load(path.c_str(), STGM_READ);
    if (FAILED(hr)) return Error(static_cast<DWORD>(hr));
    std::vector<wchar_t> target(32768), arguments(32768), working(32768);
    hr = link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH);
    if (SUCCEEDED(hr)) hr = link->GetArguments(arguments.data(), static_cast<int>(arguments.size()));
    if (SUCCEEDED(hr)) hr = link->GetWorkingDirectory(working.data(), static_cast<int>(working.size()));
    if (FAILED(hr)) return Error(static_cast<DWORD>(hr));
    return EqualPath(target.data(), Join(locations.installDirectory, kNames[0])) &&
        std::wstring(arguments.data()) == Arguments(app) && EqualPath(working.data(), app)
        ? true : Error(ERROR_INVALID_DATA);
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

std::vector<RegValue> RegistrationValues(const Locations& locations) {
    return {StringValue(L"DisplayName", kDisplayName), StringValue(L"DisplayVersion", kVersion),
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

bool CheckRegistration(const Locations& locations, bool removing) {
    if (!locations.registerUninstall) return true;
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKey, REG_OPTION_OPEN_LINK,
        KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | (removing ? DELETE : 0), &key);
    if (status != ERROR_SUCCESS) return Error(status);
    const bool matches = RegistryMatches(key, RegistrationValues(locations));
    const DWORD error = GetLastError();
    RegCloseKey(key);
    return matches ? true : Error(error);
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
    std::vector<RegValue> registryValues;
private:
    void rollback() {
        const DWORD saved = GetLastError();
        if (registryCreated) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKey, REG_OPTION_OPEN_LINK,
                KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | DELETE, &key) == ERROR_SUCCESS) {
                const bool unchanged = RegistryMatches(key, registryValues);
                RegCloseKey(key);
                if (unchanged) RegDeleteKeyW(HKEY_CURRENT_USER, kRegistryKey);
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

bool CreateShortcut(Transaction& transaction, const std::wstring& path,
                    const Locations& locations, const std::wstring& app) {
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
    if (stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > 131072) return Error(ERROR_INVALID_DATA);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    if (SUCCEEDED(hr)) hr = stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
    if (FAILED(hr) || read != bytes.size()) return Error(ERROR_READ_FAULT);
    // IPersistFile::Save could overwrite a foreign .lnk. Serialize first, then
    // use the exact same CREATE_NEW ownership path as all other installed files.
    const std::wstring hash = HashMemory(bytes.data(), bytes.size());
    return !hash.empty() && WriteNew(transaction, path, bytes.data(), bytes.size(), hash);
}

bool CreateRegistration(Transaction& transaction, const Locations& locations) {
    if (!locations.registerUninstall) return true;
    HKEY key = nullptr;
    DWORD disposition = 0;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_ENUMERATE_SUB_KEYS | DELETE, nullptr, &key, &disposition);
    if (status != ERROR_SUCCESS) return Error(status);
    if (disposition != REG_CREATED_NEW_KEY) {
        RegCloseKey(key);
        return Error(ERROR_ALREADY_EXISTS); // No writes and no rollback deletion of an old key.
    }
    transaction.registryCreated = true;
    for (const RegValue& value : RegistrationValues(locations)) {
        status = RegSetValueExW(key, value.name.c_str(), 0, value.type, value.data.data(), static_cast<DWORD>(value.data.size()));
        if (status != ERROR_SUCCESS) break;
        transaction.registryValues.push_back(value);
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS ? true : Error(status);
}

struct VerifiedInstallation {
    Manifest manifest;
    std::vector<LockedFile> files; // Manifest first, then seven files, then two shortcuts.
    bool editedConfiguration = false;
};

bool VerifyInstallation(const Locations& locations, const Payloads& payloads, bool removing,
                        const std::wstring& expectedApp, const std::wstring& expectedManager,
                        VerifiedInstallation& installation) {
    if (!TreeSafe(locations.installDirectory)) return false;
    LockedFile manifest;
    if (!LockFile(Join(locations.installDirectory, kManifestName), removing, manifest)) return false;
    std::wstring text;
    if (!ReadSmallText(manifest.handle.get(), text) || !ParseManifest(text, locations, payloads, installation.manifest)) return false;
    if ((!expectedApp.empty() && !EqualPath(installation.manifest.app, expectedApp)) ||
        (!expectedManager.empty() && installation.manifest.managerHash != expectedManager)) return Error(ERROR_INVALID_DATA);
    installation.files.push_back(std::move(manifest));
    for (std::size_t i = 0; i <= kPayloadCount; ++i) {
        const wchar_t* name = i == kPayloadCount ? kManagerName : kNames[i];
        const std::wstring& expected = i == kPayloadCount ? installation.manifest.managerHash : payloads.hashes[i];
        const bool configuration = i == kConfigurationIndex;
        LockedFile file;
        // An edited INI is read-locked but never opened for deletion.
        if (!LockFile(Join(locations.installDirectory, name), removing && !configuration, file)) return false;
        if (file.hash != expected) {
            if (!configuration) return Error(ERROR_CRC);
            file.remove = false;
            installation.editedConfiguration = true;
        } else if (configuration && removing) {
            const BY_HANDLE_FILE_INFORMATION originalIdentity = file.identity;
            file.handle.reset();
            if (!LockFile(file.path, true, file) || !SameIdentity(originalIdentity, file.identity) || file.hash != expected)
                return Error(ERROR_CRC);
        }
        installation.files.push_back(std::move(file));
    }
    for (const std::wstring& directory : {locations.desktopDirectory, locations.programsDirectory}) {
        LockedFile shortcut;
        const std::wstring path = Join(directory, kShortcutName);
        // Shell's loader need not share DELETE. Inspect under a read-only pin,
        // then acquire the deletion pin and compare both file ID and bytes.
        if (!LockFile(path, false, shortcut) || !ShortcutMatches(path, locations, installation.manifest.app)) return false;
        if (removing) {
            const BY_HANDLE_FILE_INFORMATION identity = shortcut.identity;
            const std::wstring hash = shortcut.hash;
            shortcut.handle.reset();
            if (!LockFile(path, true, shortcut) || !SameIdentity(identity, shortcut.identity) || shortcut.hash != hash)
                return Error(ERROR_CRC);
        }
        installation.files.push_back(std::move(shortcut));
    }
    return CheckRegistration(locations, removing);
}

void NotifyShortcuts(const Locations& locations, LONG event) {
    // Sandbox callers explicitly opt out of registry AND global shell events.
    if (!locations.registerUninstall) return;
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
        L"e9c9d291f9c20732904dd6b943737537c242508e546cda2afb1f72dfc2374713",
        L"bb7d4126a83b775968d49ce52f885b588561838d97ffe062ec46a8208b94c283"
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
        if (!VerifyInstallation(locations, payloads, false, app, managerHash, existing))
            return Failure(L"A pasta já existe e não corresponde a esta instalação íntegra. Nada foi sobrescrito; não há reparo automático.", LastErrorOr());
        stopped = CheckStopped();
        if (!stopped.ok) return stopped;
        Result result = Success(L"O modo janela já está instalado. A configuração existente foi mantida.");
        result.alreadyInstalled = true;
        result.preservedConfiguration = existing.editedConfiguration;
        transaction.committed = true;
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
    Manifest manifest{app, managerHash};
    const std::wstring text = ManifestText(locations, manifest, payloads);
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t byteCount = text.size() * sizeof(wchar_t);
    const std::wstring manifestHash = HashMemory(bytes, byteCount);
    if (manifestHash.empty() || !WriteNew(transaction, Join(locations.installDirectory, kManifestName), bytes, byteCount, manifestHash))
        return Failure(L"Não foi possível gravar o registro de propriedade da instalação.", LastErrorOr());
    for (const std::wstring& directory : {locations.desktopDirectory, locations.programsDirectory}) {
        if (!EnsureDirectory(transaction, directory) || !CreateShortcut(transaction, Join(directory, kShortcutName), locations, app))
            return Failure(L"Não foi possível criar os atalhos. Um atalho já existente nunca será substituído.", LastErrorOr());
    }
    if (!CreateRegistration(transaction, locations))
        return Failure(L"Não foi possível registrar a remoção para este usuário. Um registro preexistente não foi substituído.", LastErrorOr());
    VerifiedInstallation installed;
    if (!VerifyInstallation(locations, payloads, false, app, managerHash, installed))
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
    if (!VerifyInstallation(locations, payloads, true, L"", L"", installation))
        return Failure(L"A remoção foi recusada: manifesto, arquivos, atalhos ou registro não correspondem à instalação. Nada foi removido.", LastErrorOr());
    // No original application hash is required here: removing our files must
    // remain possible after the original application has been moved/deleted.
    stopped = CheckStopped();
    if (!stopped.ok) return stopped;
    if (!TreeSafe(locations.installDirectory) || !CheckRegistration(locations, true))
        return Failure(L"A instalação mudou durante a verificação. Nada foi removido.", LastErrorOr());

    // All deletable objects are pinned by read+DELETE handles denying concurrent
    // writers and renames. Every preflight completes before the first deletion.
    for (std::size_t i = installation.files.size(); i-- > 1;) {
        LockedFile& file = installation.files[i];
        if (file.remove && !DeleteLocked(file))
            return Failure(L"O Windows impediu a conclusão da remoção. O manifesto foi mantido; nenhum arquivo desconhecido foi apagado.", LastErrorOr());
    }
    if (locations.registerUninstall) {
        // A foreign/modified value or a new subkey is never recursively removed.
        if (!CheckRegistration(locations, true))
            return Failure(L"O registro de remoção mudou e foi preservado. Alguns arquivos próprios já foram removidos.", LastErrorOr());
        const LSTATUS status = RegDeleteKeyW(HKEY_CURRENT_USER, kRegistryKey);
        if (status != ERROR_SUCCESS)
            return Failure(L"Os arquivos próprios foram removidos, mas o registro de remoção foi preservado.", status);
    }
    if (!DeleteLocked(installation.files[0]))
        return Failure(L"Os arquivos próprios foram removidos, mas não foi possível remover o manifesto.", LastErrorOr());
    for (LockedFile& file : installation.files) file.handle.reset();
    // Unknown files/subdirectories and edited configuration survive. The OS
    // checks emptiness atomically; there is intentionally no recursive cleanup.
    if (!pins.removeEmpty(locations.installDirectory)) {
        const DWORD error = GetLastError();
        if (error != ERROR_DIR_NOT_EMPTY && !Missing(error))
            return Failure(L"Os arquivos próprios foram removidos, mas a pasta vazia não pôde ser removida.", error);
    }
    NotifyShortcuts(locations, SHCNE_DELETE);
    Result result = Success(installation.editedConfiguration
        ? L"Modo janela removido. A configuração editada e quaisquer arquivos pessoais foram preservados."
        : L"Modo janela removido. Arquivos pessoais, se existentes, foram preservados; o Lake original não foi alterado.");
    result.preservedConfiguration = installation.editedConfiguration;
    return result;
}
} // namespace lakefix::setup
