#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#include "../src/setup_engine.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

// Standalone filesystem/COM tests only. Every .exe/.dll below is fabricated
// plain text and is NEVER executed or loaded. No real user shell folders,
// registry, notifications, original Lake, PowerShell, windows or input devices.
// Evidence is retained in a GUID-named, marked GetTempPath sandbox. There is
// intentionally no recursive cleanup, even when a test fails halfway through.
namespace {
using namespace lakefix::setup;
constexpr std::array<const wchar_t*, 6> kNames = {{
    L"LakeWindowedLauncher.exe", L"LakeWindowedFix.dll", L"LakeWindowedFix.ini",
    L"LICENSE", L"Measure-LakeResources.ps1", L"LakeResourceMetrics.psm1"
}};
int checks = 0;
int failures = 0;
int skips = 0;

class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE && value_) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

void Check(bool condition, const char* name) {
    ++checks;
    printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
    fflush(stdout);
}

bool Require(bool condition, const char* operation) {
    if (!condition) {
        const DWORD error = GetLastError();
        Check(false, operation);
        fprintf(stderr, "  fixture infrastructure error: %lu\n", error);
    }
    return condition;
}

void Report(const Result& result) {
    if (!result.ok) fwprintf(stderr, L"  engine: %ls (error %lu)\n", result.message.c_str(), result.error);
}

std::wstring Join(const std::wstring& path, const wchar_t* name) { return path + L"\\" + name; }
bool Equal(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool NewFile(const std::wstring& path, const void* data, std::size_t size) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE || size > 1048576) return false;
    DWORD written = 0;
    return WriteFile(file.get(), data, static_cast<DWORD>(size), &written, nullptr) && written == size &&
        FlushFileBuffers(file.get());
}

bool NewText(const std::wstring& path, const std::string& text) { return NewFile(path, text.data(), text.size()); }

// Only called for a specifically named fixture-owned file, never an arbitrary
// existing path. Check the final object before truncating it; do not follow links.
bool Rewrite(const std::wstring& path, const void* data, std::size_t size) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (file.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.get(), &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1 || size > 1048576) return false;
    DWORD written = 0;
    return WriteFile(file.get(), data, static_cast<DWORD>(size), &written, nullptr) && written == size &&
        SetEndOfFile(file.get()) && FlushFileBuffers(file.get());
}

bool RewriteText(const std::wstring& path, const std::string& text) { return Rewrite(path, text.data(), text.size()); }

std::string ReadBytes(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    LARGE_INTEGER size{};
    BY_HANDLE_FILE_INFORMATION info{};
    if (file.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.get(), &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || size.QuadPart > 1048576) return {};
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD bytes = 0;
    if (!ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &bytes, nullptr) || bytes != text.size()) return {};
    return text;
}

unsigned FileCount(const std::wstring& directory) {
    WIN32_FIND_DATAW entry{};
    const HANDLE search = FindFirstFileW(Join(directory, L"*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return 0;
    unsigned count = 0;
    do {
        if (std::wcscmp(entry.cFileName, L".") && std::wcscmp(entry.cFileName, L"..")) ++count;
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    return count;
}

std::wstring CreateSandbox() {
    wchar_t temporary[32768]{};
    const DWORD length = GetTempPathW(ARRAYSIZE(temporary), temporary);
    GUID guid{};
    wchar_t identity[64]{};
    if (!length || length >= ARRAYSIZE(temporary) || FAILED(CoCreateGuid(&guid)) ||
        !StringFromGUID2(guid, identity, ARRAYSIZE(identity))) return {};
    const std::wstring root = std::wstring(temporary) + L"LakeSetupEngineTest-" + identity;
    if (!CreateDirectoryW(root.c_str(), nullptr)) return {};
    if (!NewText(Join(root, L".setup-engine-test-owned"),
        "LakeWindowedFix setup_engine_test owned sandbox; never recursively delete unknown paths.\r\n")) return {};
    return root;
}

struct Fixture {
    std::wstring root;
    std::wstring app;
    std::wstring source;
    std::wstring executableHash;
    std::wstring rendererHash;
    std::wstring setupHash;
    Locations locations;
    SupportedBuild build;
    std::array<std::string, 6> data;
    std::array<std::wstring, 6> hashes;
    std::array<PayloadFile, 6> payloads{};
    bool ready = false;

    Fixture(const std::wstring& sandbox, const wchar_t* name) {
        root = Join(sandbox, name);
        app = Join(root, L"original application");
        source = Join(root, L"source-setup.exe");
        locations.installDirectory = Join(root, L"installed");
        locations.desktopDirectory = Join(root, L"sandbox desktop");
        locations.programsDirectory = Join(root, L"sandbox programs");
        locations.registerUninstall = false; // Never touch the real HKCU/shell.
        if (!Require(CreateDirectoryW(root.c_str(), nullptr) && CreateDirectoryW(app.c_str(), nullptr) &&
            CreateDirectoryW(locations.desktopDirectory.c_str(), nullptr) &&
            CreateDirectoryW(locations.programsDirectory.c_str(), nullptr), "create private fixture directories")) return;
        if (!Require(NewText(Join(app, L"Lake Controller.exe"), "fabricated Lake executable text, NEVER EXECUTE\r\n") &&
            NewText(Join(app, L"Contour.dll"), "fabricated renderer text, NEVER LOAD\r\n") &&
            NewText(source, "fabricated setup text, NEVER EXECUTE\r\n"), "write inert fixture originals and manager")) return;
        executableHash = FileSha256(Join(app, L"Lake Controller.exe"));
        rendererHash = FileSha256(Join(app, L"Contour.dll"));
        setupHash = FileSha256(source);
        if (!Require(executableHash.size() == 64 && rendererHash.size() == 64 && setupHash.size() == 64,
                     "hash inert fixture originals")) return;
        build = {executableHash, rendererHash};
        data = {{"synthetic launcher text, NEVER EXECUTE\r\n", "synthetic fix text, NEVER LOAD\r\n",
                 "[fixture]\r\nwindowed=1\r\n", "synthetic fixture license\r\n",
                 "# synthetic measurement script, NEVER EXECUTE\r\n", "# synthetic module, NEVER IMPORT\r\n"}};
        for (std::size_t i = 0; i < data.size(); ++i) {
            const std::wstring path = Join(root, (L"payload-source-" + std::to_wstring(i) + L".fixture").c_str());
            if (!Require(NewText(path, data[i]), "write inert payload hash fixture")) return;
            hashes[i] = FileSha256(path);
            if (!Require(hashes[i].size() == 64, "hash inert payload fixture")) return;
            payloads[i] = {kNames[i], reinterpret_cast<const unsigned char*>(data[i].data()), data[i].size(), hashes[i].c_str()};
        }
        ready = true;
    }

    Result install() const { return Install(locations, app, payloads.data(), payloads.size(), source, build); }
    Result uninstall() const { return Uninstall(locations, payloads.data(), payloads.size()); }
    std::wstring installed(const wchar_t* name) const { return Join(locations.installDirectory, name); }
    std::wstring desktopLink() const { return Join(locations.desktopDirectory, kShortcutName); }
    std::wstring programsLink() const { return Join(locations.programsDirectory, kShortcutName); }
    bool originalsUnchanged() const {
        return FileSha256(Join(app, L"Lake Controller.exe")) == executableHash &&
            FileSha256(Join(app, L"Contour.dll")) == rendererHash;
    }
    bool noInstallOutput() const {
        return !Exists(locations.installDirectory) && !Exists(desktopLink()) && !Exists(programsLink());
    }
    bool filesMatch() const {
        for (std::size_t i = 0; i < payloads.size(); ++i)
            if (FileSha256(installed(kNames[i])) != hashes[i]) return false;
        return FileSha256(installed(kManagerName)) == setupHash &&
            Exists(installed(kManifestName)) && FileCount(locations.installDirectory) == 8;
    }
    bool ownedGone(bool preserveIni = false) const {
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            if ((i != 2 || !preserveIni) && Exists(installed(kNames[i]))) return false;
        }
        return !Exists(installed(kManagerName)) && !Exists(installed(kManifestName)) &&
            !Exists(desktopLink()) && !Exists(programsLink());
    }
};

bool ReadyInstalled(Fixture& fixture) {
    if (!fixture.ready) return false;
    const Result result = fixture.install();
    Report(result);
    return Require(result.ok && !result.alreadyInstalled, "prepare isolated installed fixture");
}

struct Snapshot {
    std::array<std::wstring, 10> hashes;
    explicit Snapshot(const Fixture& fixture) {
        for (std::size_t i = 0; i < kNames.size(); ++i) hashes[i] = FileSha256(fixture.installed(kNames[i]));
        hashes[6] = FileSha256(fixture.installed(kManagerName));
        hashes[7] = FileSha256(fixture.installed(kManifestName));
        hashes[8] = FileSha256(fixture.desktopLink());
        hashes[9] = FileSha256(fixture.programsLink());
    }
    bool unchanged(const Fixture& fixture) const { return hashes == Snapshot(fixture).hashes; }
};

struct LinkInfo {
    bool ok = false;
    std::wstring target;
    std::wstring arguments;
    std::wstring workingDirectory;
};

LinkInfo ReadLink(const std::wstring& path) {
    LinkInfo result;
    IShellLinkW* link = nullptr;
    IPersistFile* persistence = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, reinterpret_cast<void**>(&link));
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persistence));
    if (SUCCEEDED(hr)) hr = persistence->Load(path.c_str(), STGM_READ);
    std::vector<wchar_t> target(32768), arguments(32768), working(32768);
    if (SUCCEEDED(hr)) hr = link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH);
    if (SUCCEEDED(hr)) hr = link->GetArguments(arguments.data(), static_cast<int>(arguments.size()));
    if (SUCCEEDED(hr)) hr = link->GetWorkingDirectory(working.data(), static_cast<int>(working.size()));
    if (SUCCEEDED(hr)) {
        result.ok = true;
        result.target = target.data();
        result.arguments = arguments.data();
        result.workingDirectory = working.data();
    }
    if (persistence) persistence->Release();
    if (link) link->Release();
    return result;
}

bool RetargetOwnedLink(const std::wstring& path, const std::wstring& target,
                       const std::wstring& arguments, const std::wstring& working) {
    // Explicitly edits only a shortcut this fixture just installed.
    IShellLinkW* link = nullptr;
    IPersistFile* persistence = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, reinterpret_cast<void**>(&link));
    if (SUCCEEDED(hr)) hr = link->SetPath(target.c_str());
    if (SUCCEEDED(hr)) hr = link->SetArguments(arguments.c_str());
    if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(working.c_str());
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persistence));
    if (SUCCEEDED(hr)) hr = persistence->Save(path.c_str(), TRUE);
    if (persistence) persistence->Release();
    if (link) link->Release();
    return SUCCEEDED(hr);
}

bool EditManifest(const Fixture& fixture, const std::wstring& from, const std::wstring& to) {
    const std::string bytes = ReadBytes(fixture.installed(kManifestName));
    if (bytes.empty() || bytes.size() % sizeof(wchar_t)) return false;
    std::wstring text(bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), bytes.data(), bytes.size());
    if (from.empty()) text += to;
    else {
        const std::size_t at = text.find(from);
        if (at == std::wstring::npos) return false;
        text.replace(at, from.size(), to);
    }
    return Rewrite(fixture.installed(kManifestName), text.data(), text.size() * sizeof(wchar_t));
}

bool RefusedWithoutChanges(const Fixture& fixture, const Result& result, const Snapshot& before) {
    return !result.ok && before.unchanged(fixture) && fixture.originalsUnchanged() &&
        Exists(fixture.installed(kManifestName)) && Exists(fixture.desktopLink()) && Exists(fixture.programsLink());
}

void TestIdentity(const std::wstring& root) {
    const std::wstring abc = Join(root, L"known-sha256.fixture");
    if (!Require(NewText(abc, "abc"), "create known SHA-256 fixture")) return;
    Check(FileSha256(abc) == L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "BCrypt SHA-256 matches the independent abc vector");
    Check(FileSha256(L"relative-file.fixture").empty(), "hash API refuses relative paths");
    Check(LakeBuild().executableSha256 == L"e9c9d291f9c20732904dd6b943737537c242508e546cda2afb1f72dfc2374713" &&
          LakeBuild().rendererSha256 == L"bb7d4126a83b775968d49ce52f885b588561838d97ffe062ec46a8208b94c283",
          "production build identity remains the two fixed original hashes");
}

void TestInstallAndPreservation(const std::wstring& root) {
    Fixture f(root, L"good-install-and-preservation");
    if (!f.ready) return;
    Check(ValidateApplication(f.app, f.build).ok, "inert original files validate using explicit fixture identities");
    const Result installed = f.install();
    Report(installed);
    Check(installed.ok && !installed.alreadyInstalled, "fresh per-user sandbox installation succeeds");
    if (!installed.ok) return;
    Check(f.filesMatch(), "exactly six payloads, copied manager and manifest; every owned byte matches");
    const LinkInfo desktop = ReadLink(f.desktopLink());
    const LinkInfo programs = ReadLink(f.programsLink());
    const std::wstring arguments = L"--app-dir \"" + f.app + L"\"";
    Check(desktop.ok && Equal(desktop.target, f.installed(kNames[0])), "desktop shortcut targets the owned launcher, not Lake directly");
    Check(desktop.ok && desktop.arguments == arguments, "desktop shortcut quotes the full known original --app-dir");
    Check(desktop.ok && Equal(desktop.workingDirectory, f.app), "desktop shortcut working directory is the original application folder");
    Check(programs.ok && Equal(programs.target, desktop.target) && programs.arguments == arguments &&
          Equal(programs.workingDirectory, f.app), "start-menu shortcut has the same exact target, arguments and working directory");
    Check(f.originalsUnchanged() && !Exists(f.installed(L"Lake Controller.exe")) && !Exists(f.installed(L"Contour.dll")),
          "original proprietary files remain unchanged and are never copied");
    const Snapshot initial(f);
    const Result again = f.install();
    Check(again.ok && again.alreadyInstalled && !again.preservedConfiguration && initial.unchanged(f),
          "repeating the identical installation is a read-only alreadyInstalled result");

    const std::string edited = "[fixture]\r\nwindowed=0\r\nuser-edit=keep me\r\n";
    const std::wstring unknown = f.installed(L"user-notes.txt");
    const std::wstring unknownDirectory = f.installed(L"user-files");
    const std::wstring nested = Join(unknownDirectory, L"personal.txt");
    if (!Require(RewriteText(f.installed(kNames[2]), edited) && NewText(unknown, "personal notes\r\n") &&
        CreateDirectoryW(unknownDirectory.c_str(), nullptr) && NewText(nested, "nested personal notes\r\n"),
        "edit the fixture INI and add unknown user files")) return;
    const Snapshot changed(f);
    const Result editedRepeat = f.install();
    Check(editedRepeat.ok && editedRepeat.alreadyInstalled && editedRepeat.preservedConfiguration && changed.unchanged(f),
          "idempotent installation preserves edited INI and unknown files without a repair");
    const Result removed = f.uninstall();
    Report(removed);
    Check(removed.ok && removed.preservedConfiguration, "uninstall reports an edited configuration as preserved");
    Check(ReadBytes(f.installed(kNames[2])) == edited && ReadBytes(unknown) == "personal notes\r\n" &&
          ReadBytes(nested) == "nested personal notes\r\n", "edited INI and unfamiliar files/subdirectories survive byte-for-byte");
    Check(f.ownedGone(true) && Exists(f.locations.installDirectory), "only verified owned files/shortcuts are removed; nonempty user folder survives");
    Check(f.originalsUnchanged(), "uninstall never changes either original application file");
}

void TestNormalRemoval(const std::wstring& root) {
    {
        Fixture f(root, L"normal-uninstall");
        if (ReadyInstalled(f)) {
            const Result result = f.uninstall();
            Report(result);
            Check(result.ok && !result.preservedConfiguration && f.ownedGone() && !Exists(f.locations.installDirectory),
                  "normal uninstall removes the unchanged INI, manager, manifest, links and empty owned folder");
            Check(f.originalsUnchanged() && Exists(f.locations.desktopDirectory) && Exists(f.locations.programsDirectory),
                  "normal uninstall preserves originals and preexisting shortcut directories");
        }
    }
    {
        Fixture f(root, L"original-removed-before-uninstall");
        if (ReadyInstalled(f)) {
            if (!Require(DeleteFileW(Join(f.app, L"Lake Controller.exe").c_str()) &&
                DeleteFileW(Join(f.app, L"Contour.dll").c_str()) && RemoveDirectoryW(f.app.c_str()),
                "remove only inert fixture original files and their empty folder")) return;
            const Result result = f.uninstall();
            Check(result.ok && f.ownedGone() && !Exists(f.locations.installDirectory),
                  "uninstall succeeds after the original application folder has been removed");
        }
    }
}

void TestPayloadAndBuildRejection(const std::wstring& root) {
    {
        Fixture f(root, L"original-cannot-be-manager");
        const std::wstring disguised = Join(f.root, L"not-the-setup.exe");
        if (f.ready && Require(NewText(disguised, ReadBytes(Join(f.app, L"Lake Controller.exe"))), "create inert original bytes under a different fixture filename")) {
            const Result inside = Install(f.locations, f.app, f.payloads.data(), f.payloads.size(), Join(f.app, L"Lake Controller.exe"), f.build);
            const Result renamed = Install(f.locations, f.app, f.payloads.data(), f.payloads.size(), disguised, f.build);
            Check(!inside.ok && !renamed.ok && f.noInstallOutput() && f.originalsUnchanged(),
                  "original application files cannot be copied as the setup manager, even under a different filename");
        }
    }
    {
        Fixture f(root, L"wrong-executable-build");
        if (f.ready) {
            SupportedBuild wrong = f.build;
            wrong.executableSha256.assign(64, L'0');
            const Result validation = ValidateApplication(f.app, wrong);
            Check(!validation.ok && validation.error == ERROR_REVISION_MISMATCH, "a different executable build is rejected explicitly");
            const Result result = Install(f.locations, f.app, f.payloads.data(), f.payloads.size(), f.source, wrong);
            Check(!result.ok && f.noInstallOutput() && f.originalsUnchanged(), "unsupported executable build writes nothing at all");
        }
    }
    {
        Fixture f(root, L"wrong-renderer-build");
        if (f.ready) {
            SupportedBuild wrong = f.build;
            wrong.rendererSha256.assign(64, L'0');
            const Result result = Install(f.locations, f.app, f.payloads.data(), f.payloads.size(), f.source, wrong);
            Check(!result.ok && f.noInstallOutput() && f.originalsUnchanged(), "a different renderer build also writes nothing");
        }
    }
    {
        Fixture f(root, L"payload-hash-mismatch");
        if (f.ready) {
            auto payloads = f.payloads;
            const unsigned char wrong[] = "modified fabricated payload";
            payloads[0].data = wrong;
            payloads[0].size = sizeof(wrong) - 1;
            const Result result = Install(f.locations, f.app, payloads.data(), payloads.size(), f.source, f.build);
            Check(!result.ok && f.noInstallOutput(), "payload bytes with a mismatching hash are rejected before writes");
        }
    }
    {
        Fixture f(root, L"payload-duplicate");
        if (f.ready) {
            auto payloads = f.payloads;
            payloads[5].name = payloads[4].name;
            const Result result = Install(f.locations, f.app, payloads.data(), payloads.size(), f.source, f.build);
            Check(!result.ok && f.noInstallOutput(), "duplicate whitelisted payload names cannot alias an owned file");
        }
    }
    {
        Fixture f(root, L"payload-traversal");
        if (f.ready) {
            auto payloads = f.payloads;
            payloads[5].name = L"..\\escaped.txt";
            const Result result = Install(f.locations, f.app, payloads.data(), payloads.size(), f.source, f.build);
            Check(!result.ok && f.noInstallOutput() && !Exists(Join(f.root, L"escaped.txt")), "payload traversal is rejected, not normalized into a write");
        }
    }
    {
        Fixture f(root, L"unexpected-payload");
        if (f.ready) {
            auto payloads = f.payloads;
            payloads[0].name = L"Lake Controller.exe";
            const Result result = Install(f.locations, f.app, payloads.data(), payloads.size(), f.source, f.build);
            Check(!result.ok && f.noInstallOutput() && f.originalsUnchanged(), "proprietary/unexpected payload names are never accepted");
        }
    }
    {
        Fixture f(root, L"wrong-payload-count");
        if (f.ready) {
            const Result result = Install(f.locations, f.app, f.payloads.data(), f.payloads.size() - 1, f.source, f.build);
            Check(!result.ok && f.noInstallOutput(), "an incomplete payload whitelist is rejected");
        }
    }
    {
        Fixture f(root, L"malformed-payload-hash");
        if (f.ready) {
            auto payloads = f.payloads;
            payloads[0].sha256 = L"not-a-sha256";
            const Result result = Install(f.locations, f.app, payloads.data(), payloads.size(), f.source, f.build);
            Check(!result.ok && f.noInstallOutput(), "malformed payload hashes are rejected before writes");
        }
    }
}

void TestUnownedAndRollback(const std::wstring& root) {
    {
        Fixture f(root, L"unowned-empty-directory");
        if (f.ready && Require(CreateDirectoryW(f.locations.installDirectory.c_str(), nullptr), "create unowned empty fixture directory")) {
            const Result result = f.install();
            Check(!result.ok && Exists(f.locations.installDirectory) && FileCount(f.locations.installDirectory) == 0 &&
                  !Exists(f.desktopLink()) && !Exists(f.programsLink()), "even an empty unowned install directory is not adopted or removed");
        }
    }
    {
        Fixture f(root, L"unowned-user-directory");
        const std::wstring user = f.installed(L"user.txt");
        if (f.ready && Require(CreateDirectoryW(f.locations.installDirectory.c_str(), nullptr) && NewText(user, "keep unowned user bytes"),
                               "create occupied unowned fixture directory")) {
            const Result result = f.install();
            Check(!result.ok && ReadBytes(user) == "keep unowned user bytes" && FileCount(f.locations.installDirectory) == 1,
                  "an unowned folder and its unfamiliar contents are never overwritten or cleaned");
        }
    }
    {
        Fixture f(root, L"unowned-install-file");
        if (f.ready && Require(NewText(f.locations.installDirectory, "not an install folder"), "create unowned file at install path")) {
            const Result result = f.install();
            Check(!result.ok && ReadBytes(f.locations.installDirectory) == "not an install folder" && !Exists(f.desktopLink()),
                  "a file occupying the install-directory path is preserved");
        }
    }
    {
        Fixture f(root, L"foreign-desktop-link");
        if (f.ready && Require(NewText(f.desktopLink(), "foreign desktop shortcut bytes"), "reserve a foreign desktop shortcut name")) {
            const Result result = f.install();
            Check(!result.ok && !Exists(f.locations.installDirectory) && !Exists(f.programsLink()) &&
                  ReadBytes(f.desktopLink()) == "foreign desktop shortcut bytes", "foreign desktop shortcut is never overwritten; new files roll back");
        }
    }
    {
        Fixture f(root, L"second-link-rollback");
        const std::wstring desktopUser = Join(f.locations.desktopDirectory, L"personal.txt");
        const std::wstring programsUser = Join(f.locations.programsDirectory, L"personal.txt");
        if (f.ready && Require(NewText(f.programsLink(), "foreign second shortcut bytes") &&
            NewText(desktopUser, "desktop user work") && NewText(programsUser, "programs user work"), "reserve second shortcut and unrelated user files")) {
            const Result result = f.install();
            Check(!result.ok && !Exists(f.locations.installDirectory) && !Exists(f.desktopLink()) &&
                  ReadBytes(f.programsLink()) == "foreign second shortcut bytes", "second-shortcut failure rolls back the first shortcut and only newly owned files");
            Check(ReadBytes(desktopUser) == "desktop user work" && ReadBytes(programsUser) == "programs user work" &&
                  Exists(f.locations.desktopDirectory) && Exists(f.locations.programsDirectory),
                  "rollback never removes unfamiliar files or preexisting directories");
        }
    }
}

void TestPathRejection(const std::wstring& root) {
    {
        Fixture f(root, L"install-inside-original");
        if (f.ready) {
            Locations locations = f.locations;
            locations.installDirectory = Join(f.app, L"never-create-fix");
            const Result result = Install(locations, f.app, f.payloads.data(), f.payloads.size(), f.source, f.build);
            Check(!result.ok && !Exists(locations.installDirectory) && f.noInstallOutput() && f.originalsUnchanged(),
                  "installation inside the original application directory is refused");
        }
    }
    {
        Fixture f(root, L"shortcut-inside-original");
        if (f.ready) {
            Locations locations = f.locations;
            locations.desktopDirectory = f.app;
            const Result result = Install(locations, f.app, f.payloads.data(), f.payloads.size(), f.source, f.build);
            Check(!result.ok && f.noInstallOutput() && !Exists(Join(f.app, kShortcutName)) && f.originalsUnchanged(),
                  "shortcut creation inside the original application directory is also refused");
        }
    }
    {
        Fixture f(root, L"install-desktop-overlap");
        if (f.ready) {
            Locations locations = f.locations;
            locations.desktopDirectory = Join(locations.installDirectory, L"desktop");
            const Result result = Install(locations, f.app, f.payloads.data(), f.payloads.size(), f.source, f.build);
            Check(!result.ok && f.noInstallOutput(), "desktop nested inside the installation is refused");
        }
    }
    {
        Fixture f(root, L"install-programs-overlap");
        if (f.ready) {
            Locations locations = f.locations;
            locations.installDirectory = Join(locations.programsDirectory, L"installed");
            const Result result = Install(locations, f.app, f.payloads.data(), f.payloads.size(), f.source, f.build);
            Check(!result.ok && !Exists(locations.installDirectory) && f.noInstallOutput(), "installation nested inside programs is refused");
        }
    }
    {
        Fixture f(root, L"identical-shortcut-folders");
        if (f.ready) {
            Locations locations = f.locations;
            locations.desktopDirectory = locations.programsDirectory;
            const Result result = Install(locations, f.app, f.payloads.data(), f.payloads.size(), f.source, f.build);
            Check(!result.ok && f.noInstallOutput(), "identical desktop and programs folders are refused");
        }
    }
    {
        Fixture f(root, L"relative-location");
        if (f.ready) {
            Locations locations = f.locations;
            locations.installDirectory = L"relative-install-must-never-be-created";
            const Result result = Install(locations, f.app, f.payloads.data(), f.payloads.size(), f.source, f.build);
            Check(!result.ok && f.noInstallOutput(), "relative destination paths cannot depend on process working directory");
        }
    }
    {
        Fixture f(root, L"lexical-traversal-location");
        if (f.ready) {
            Locations locations = f.locations;
            locations.installDirectory = Join(f.root, L"missing\\..\\escaped");
            const Result result = Install(locations, f.app, f.payloads.data(), f.payloads.size(), f.source, f.build);
            Check(!result.ok && f.noInstallOutput() && !Exists(Join(f.root, L"missing")) && !Exists(Join(f.root, L"escaped")),
                  "lexical parent traversal is refused without creating any intermediate directory");
        }
    }
}

void TestCorruptPreflight(const std::wstring& root) {
    {
        Fixture f(root, L"different-original-target");
        const std::wstring other = Join(f.root, L"different original folder");
        if (ReadyInstalled(f) && Require(CreateDirectoryW(other.c_str(), nullptr) &&
            NewText(Join(other, L"Lake Controller.exe"), ReadBytes(Join(f.app, L"Lake Controller.exe"))) &&
            NewText(Join(other, L"Contour.dll"), ReadBytes(Join(f.app, L"Contour.dll"))), "create another inert original of the same fixture build")) {
            const Snapshot before(f);
            const Result result = Install(f.locations, other, f.payloads.data(), f.payloads.size(), f.source, f.build);
            Check(RefusedWithoutChanges(f, result, before), "same-build installation for a different original folder is not silently retargeted");
        }
    }
    {
        Fixture f(root, L"different-setup-manager");
        if (ReadyInstalled(f) && Require(RewriteText(f.source, "different synthetic installer, NEVER EXECUTE"), "change only the inert source installer")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.install(), before), "a different installer manager is not silently substituted at the same version");
        }
    }
    {
        Fixture f(root, L"corrupt-owned-binary");
        if (ReadyInstalled(f) && Require(RewriteText(f.installed(kNames[1]), "corrupted fabricated DLL bytes"), "tamper only the fixture-owned DLL")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.uninstall(), before), "corrupt owned binary refuses ALL uninstall changes, including both links and manifest");
            const Result result = f.install();
            Check(RefusedWithoutChanges(f, result, before), "install does not repair or overwrite a corrupt existing binary");
        }
    }
    {
        Fixture f(root, L"corrupt-manager");
        if (ReadyInstalled(f) && Require(RewriteText(f.installed(kManagerName), "corrupted fabricated manager bytes"), "tamper the fixture-owned manager")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.uninstall(), before), "manager hash is included in the complete uninstall preflight");
        }
    }
    {
        Fixture f(root, L"manifest-path-tamper");
        if (ReadyInstalled(f) && Require(EditManifest(f, L"appDirectory=" + f.app, L"appDirectory=" + f.app + L"\\..\\outside"),
                                       "insert parent traversal into the fixture manifest")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.uninstall(), before), "manifest path traversal refuses ALL deletions");
        }
    }
    {
        Fixture f(root, L"manifest-unknown-key");
        if (ReadyInstalled(f) && Require(EditManifest(f, L"", L"unexpectedOwnedPath=..\\not-owned\r\n"), "append an unknown manifest key")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.uninstall(), before), "unknown manifest fields cannot expand the uninstall ownership whitelist");
        }
    }
    {
        Fixture f(root, L"manifest-count-tamper");
        if (ReadyInstalled(f) && Require(EditManifest(f, L"fileCount=7", L"fileCount=8"), "tamper the manifest file count")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.uninstall(), before), "a changed manifest file count refuses ALL deletions");
        }
    }
    {
        Fixture f(root, L"manifest-version-tamper");
        if (ReadyInstalled(f) && Require(EditManifest(f, L"version=0.6.0", L"version=9.9.9"), "tamper the manifest product version")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.uninstall(), before), "an unknown manifest product version is not adopted for removal");
        }
    }
    {
        Fixture f(root, L"manifest-payload-hash-tamper");
        if (ReadyInstalled(f) && Require(EditManifest(f, std::wstring(kNames[1]) + L"=" + f.hashes[1],
            std::wstring(kNames[1]) + L"=" + std::wstring(64, L'0')), "tamper a manifest payload hash")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.uninstall(), before), "manifest payload hashes must match the compiled payload identities, not just its own claims");
        }
    }
}

void TestShortcutOwnership(const std::wstring& root) {
    {
        Fixture f(root, L"retargeted-shortcut");
        if (ReadyInstalled(f) && Require(RetargetOwnedLink(f.programsLink(), Join(f.app, L"Lake Controller.exe"),
            L"--app-dir \"" + f.app + L"\"", f.app), "retarget only the fixture start-menu shortcut")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.uninstall(), before), "retargeted shortcut refuses ALL uninstall changes despite its owned-looking filename");
            Check(RefusedWithoutChanges(f, f.install(), before), "idempotent install never overwrites a retargeted shortcut");
        }
    }
    {
        Fixture f(root, L"changed-shortcut-arguments");
        if (ReadyInstalled(f) && Require(RetargetOwnedLink(f.desktopLink(), f.installed(kNames[0]),
            L"--app-dir \"" + Join(f.root, L"different-original") + L"\"", f.app), "change only the fixture shortcut arguments")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.uninstall(), before), "a correct launcher target with foreign --app-dir arguments is not owned");
        }
    }
    {
        Fixture f(root, L"changed-shortcut-working-directory");
        if (ReadyInstalled(f) && Require(RetargetOwnedLink(f.desktopLink(), f.installed(kNames[0]),
            L"--app-dir \"" + f.app + L"\"", f.root), "change only the fixture shortcut working directory")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.uninstall(), before), "a shortcut with a foreign working directory also fails the all-or-nothing preflight");
        }
    }
}

void TestReparseIfAvailable(const std::wstring& root) {
    Fixture f(root, L"reparse-owned-tree");
    if (!ReadyInstalled(f)) return;
    const std::wstring target = Join(f.root, L"test-owned-link-target");
    const std::wstring link = f.installed(L"unfamiliar-directory-link");
    if (!Require(CreateDirectoryW(target.c_str(), nullptr) && NewText(Join(target, L"personal.txt"), "link target must survive"),
                 "create private reparse target")) return;
    // Developer Mode may permit this without elevation. Never request admin,
    // change privileges or use a helper process to obtain that capability.
    BOOLEAN created = CreateSymbolicLinkW(link.c_str(), target.c_str(), 0x1 | 0x2);
    if (!created && GetLastError() == ERROR_INVALID_PARAMETER)
        created = CreateSymbolicLinkW(link.c_str(), target.c_str(), 0x1);
    if (!created) {
        ++skips;
        printf("SKIP: directory reparse checks unavailable without admin (error %lu); not counted as PASS\n", GetLastError());
        return;
    }
    const Snapshot before(f);
    Check(RefusedWithoutChanges(f, f.install(), before), "any reparse point inside the owned tree refuses idempotent install");
    Check(RefusedWithoutChanges(f, f.uninstall(), before) && ReadBytes(Join(target, L"personal.txt")) == "link target must survive",
          "reparse preflight refuses ALL removal and never follows the link target");
    // Remove exactly the symlink just created, never the directory it targets.
    Require(RemoveDirectoryW(link.c_str()) != FALSE, "remove only the test-created directory symlink");
}
} // namespace

int wmain() {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!Require(SUCCEEDED(apartment), "initialize test-owned COM apartment")) return 1;
    const std::wstring root = CreateSandbox();
    if (!Require(!root.empty(), "create GUID-named and marked test-owned sandbox")) {
        CoUninitialize();
        return 1;
    }
    fwprintf(stderr, L"Test evidence retained (no recursive cleanup): %ls\n", root.c_str());
    Check(!LakeIsRunning(), "Lake must already be closed; the test never launches, closes or kills it");
    if (!failures) {
        TestIdentity(root);
        TestInstallAndPreservation(root);
        TestNormalRemoval(root);
        TestPayloadAndBuildRejection(root);
        TestUnownedAndRollback(root);
        TestPathRejection(root);
        TestCorruptPreflight(root);
        TestShortcutOwnership(root);
        TestReparseIfAvailable(root);
    }
    CoUninitialize();
    printf("setup_engine: %d checks, %d failures, %d optional skips (no skipped check counted as PASS)\n", checks, failures, skips);
    return failures ? 1 : 0;
}
