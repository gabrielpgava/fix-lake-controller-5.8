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
// operational registry, notifications, original Lake, PowerShell, windows or input devices.
// Explicit registry cases use fresh GUID keys under Software\LakeWindowedFix.SetupTests.
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
        return !Exists(installed(kManagerName)) && Exists(installed(kManifestName)) && Exists(installed(kRemovalJournalName)) &&
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
    std::array<std::wstring, 13> hashes;
    explicit Snapshot(const Fixture& fixture) {
        for (std::size_t i = 0; i < kNames.size(); ++i) hashes[i] = FileSha256(fixture.installed(kNames[i]));
        hashes[6] = FileSha256(fixture.installed(kManagerName));
        hashes[7] = FileSha256(fixture.installed(kManifestName));
        hashes[8] = FileSha256(fixture.desktopLink());
        hashes[9] = FileSha256(fixture.programsLink());
        hashes[10] = FileSha256(fixture.installed(kRemovalJournalName));
        hashes[11] = FileSha256(fixture.installed(kMigrationJournalName));
        hashes[12] = FileSha256(fixture.installed(kConfigurationStageName));
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
    const std::wstring log = f.installed(L"LakeWindowedFix.log");
    const std::wstring unknownDirectory = f.installed(L"user-files");
    const std::wstring nested = Join(unknownDirectory, L"personal.txt");
    if (!Require(RewriteText(f.installed(kNames[2]), edited) && NewText(unknown, "personal notes\r\n") && NewText(log, "synthetic runtime log\r\n") &&
        CreateDirectoryW(unknownDirectory.c_str(), nullptr) && NewText(nested, "nested personal notes\r\n"),
        "edit the fixture INI and add unknown user files")) return;
    const Snapshot changed(f);
    const Result editedRepeat = f.install();
    Check(editedRepeat.ok && editedRepeat.alreadyInstalled && editedRepeat.preservedConfiguration && changed.unchanged(f),
          "idempotent installation preserves edited INI and unknown files without a repair");
    Check(editedRepeat.Message(Language::English) == Text(TextId::AlreadyInstalled, Language::English),
          "successful engine result is available in English without changing installation behavior");
    const Result removed = f.uninstall();
    Report(removed);
    Check(removed.ok && removed.preservedConfiguration, "uninstall reports an edited configuration as preserved");
    Check(removed.Message(Language::English).find(L"LakeWindowedFix.ini (edited)") != std::wstring::npos,
          "English removal summary localizes the edited-INI role without renaming its file");
    Check(ReadBytes(f.installed(kNames[2])) == edited && ReadBytes(unknown) == "personal notes\r\n" &&
          ReadBytes(nested) == "nested personal notes\r\n", "edited INI and unfamiliar files/subdirectories survive byte-for-byte");
    Check(f.ownedGone(true) && Exists(f.locations.installDirectory), "owned resources removed; receipt/marker and nonempty user folder survive");
    const Snapshot removedSnapshot(f);
    const Result removeAgain = f.uninstall();
    Check(removeAgain.ok && removedSnapshot.unchanged(f), "removal with preserved data is idempotent even with all owned binaries/links absent");
    const Result reinstalled = f.install();
    Report(reinstalled);
    Check(reinstalled.ok && reinstalled.repaired && !reinstalled.alreadyInstalled && !Exists(f.installed(kRemovalJournalName)),
          "receipt permits reinstall after uninstall, clearing removal intent only after verification");
    Check(ReadBytes(f.installed(kNames[2])) == edited && ReadBytes(log) == "synthetic runtime log\r\n" &&
          ReadBytes(unknown) == "personal notes\r\n" && ReadBytes(nested) == "nested personal notes\r\n",
          "install/edit/generate-log/uninstall/reinstall preserves INI, logs and unknown nested files byte-for-byte");
    Check(f.install().alreadyInstalled, "reinstalled tree is recognized without another mutation");
    Check(f.originalsUnchanged(), "uninstall never changes either original application file");
}

void TestNormalRemoval(const std::wstring& root) {
    {
        Fixture f(root, L"normal-uninstall");
        if (ReadyInstalled(f)) {
            const Result result = f.uninstall();
            Report(result);
            Check(result.ok && !result.preservedConfiguration && f.ownedGone() && FileCount(f.locations.installDirectory) == 2,
                  "normal uninstall removes resources but retains only immutable receipt and removal marker");
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
            Check(result.ok && f.ownedGone(),
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
            const Result result = f.install();
            Check(RefusedWithoutChanges(f, result, before), "install does not repair or overwrite a corrupt existing binary");
            Check(result.Message(Language::English).find(L"Repair refused:") == 0 && result.error == ERROR_CRC,
                  "English error text preserves the same refusal and Win32 error code");
            const Result removed = f.uninstall();
            Check(removed.ok && removed.preservedFiles && ReadBytes(f.installed(kNames[1])) == "corrupted fabricated DLL bytes" &&
                  Exists(f.installed(kManifestName)) && Exists(f.installed(kRemovalJournalName)) && !Exists(f.installed(kManagerName)) &&
                   !Exists(f.desktopLink()) && !Exists(f.programsLink()), "uninstall preserves altered binary but removes other owned resources with a recovery receipt");
            Check(removed.message.find(L"LakeWindowedFix.dll") != std::wstring::npos,
                  "partial removal names the exact known binary it preserved, without enumerating private files");
            Check(removed.Message(Language::English).find(L"Preserved known objects: LakeWindowedFix.dll") != std::wstring::npos,
                  "English partial-removal text preserves the exact known binary name");
        }
    }
    {
        Fixture f(root, L"corrupt-manager");
        if (ReadyInstalled(f) && Require(RewriteText(f.installed(kManagerName), "corrupted fabricated manager bytes"), "tamper the fixture-owned manager")) {
            const Result result = f.uninstall();
            Check(result.ok && result.preservedFiles && ReadBytes(f.installed(kManagerName)) == "corrupted fabricated manager bytes" &&
                   !Exists(f.installed(kNames[0])) && !Exists(f.desktopLink()), "altered manager is preserved, never treated as a deletable owned executable");
            Check(result.message.find(kManagerName) != std::wstring::npos, "partial removal identifies a preserved manager explicitly");
            Check(result.Message(Language::English).find(kManagerName) != std::wstring::npos,
                  "English partial-removal text identifies the same preserved manager");
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
        if (ReadyInstalled(f) && Require(EditManifest(f, std::wstring(L"version=") + kVersion, L"version=9.9.9"), "tamper the manifest product version")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.uninstall(), before), "an unknown manifest product version is not adopted for removal");
        }
    }
    {
        Fixture f(root, L"manifest-payload-hash-tamper");
        if (ReadyInstalled(f) && Require(EditManifest(f, std::wstring(kNames[1]) + L"=" + f.hashes[1],
            std::wstring(kNames[1]) + L"=" + std::wstring(64, L'0')), "tamper a manifest payload hash")) {
            const std::wstring binaryHash = FileSha256(f.installed(kNames[1]));
            const Result result = f.uninstall();
            Check(result.ok && result.preservedFiles && FileSha256(f.installed(kNames[1])) == binaryHash && !Exists(f.installed(kNames[0])),
                  "a hash claim that does not match an existing file cannot authorize deleting that file");
        }
    }
}

void TestShortcutOwnership(const std::wstring& root) {
    {
        Fixture f(root, L"retargeted-shortcut");
        if (ReadyInstalled(f) && Require(RetargetOwnedLink(f.programsLink(), Join(f.app, L"Lake Controller.exe"),
            L"--app-dir \"" + f.app + L"\"", f.app), "retarget only the fixture start-menu shortcut")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.install(), before), "idempotent install never overwrites a retargeted shortcut");
            const std::string foreign = ReadBytes(f.programsLink());
            const Result result = f.uninstall();
            Check(result.ok && result.preservedFiles && ReadBytes(f.programsLink()) == foreign && !Exists(f.desktopLink()) &&
                  !Exists(f.installed(kNames[0])), "retargeted shortcut is preserved without blocking removal of other owned resources");
        }
    }
    {
        Fixture f(root, L"changed-shortcut-arguments");
        if (ReadyInstalled(f) && Require(RetargetOwnedLink(f.desktopLink(), f.installed(kNames[0]),
            L"--app-dir \"" + Join(f.root, L"different-original") + L"\"", f.app), "change only the fixture shortcut arguments")) {
            const std::string foreign = ReadBytes(f.desktopLink());
            const Result result = f.uninstall();
            Check(result.ok && result.preservedFiles && ReadBytes(f.desktopLink()) == foreign && !Exists(f.programsLink()),
                  "a correct launcher target with foreign --app-dir arguments is preserved");
        }
    }
    {
        Fixture f(root, L"changed-shortcut-working-directory");
        if (ReadyInstalled(f) && Require(RetargetOwnedLink(f.desktopLink(), f.installed(kNames[0]),
            L"--app-dir \"" + f.app + L"\"", f.root), "change only the fixture shortcut working directory")) {
            const std::string foreign = ReadBytes(f.desktopLink());
            const Result result = f.uninstall();
            Check(result.ok && result.preservedFiles && ReadBytes(f.desktopLink()) == foreign && !Exists(f.programsLink()),
                  "a shortcut with a foreign working directory is preserved");
        }
    }
    {
        Fixture f(root, L"same-properties-changed-link-bytes");
        if (ReadyInstalled(f)) {
            const std::string altered = ReadBytes(f.desktopLink()) + "foreign trailing link data";
            if (!Require(RewriteText(f.desktopLink(), altered), "append only to a fixture-owned shortcut")) return;
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.install(), before), "even a byte edit with unchanged shortcut properties blocks overwrite by repair");
            const Result result = f.uninstall();
            Check(result.ok && result.preservedFiles && ReadBytes(f.desktopLink()) == altered && !Exists(f.programsLink()),
                  "schema 2 hashes preserve altered shortcut bytes, not merely retargeted links");
        }
    }
    {
        Fixture f(root, L"foreign-link-after-uninstall");
        if (ReadyInstalled(f) && Require(f.uninstall().ok && NewText(f.desktopLink(), "new foreign link after removal"),
                                        "reserve a foreign name after fixture uninstall")) {
            const Snapshot before(f);
            const Result result = f.install();
            Check(!result.ok && result.preservedFiles && before.unchanged(f) && !Exists(f.installed(kManagerName)) &&
                  ReadBytes(f.desktopLink()) == "new foreign link after removal", "reinstall refuses a foreign occupied link name without consuming removal intent");
        }
    }
}

bool ReplacePayload(Fixture& fixture, std::size_t index, const std::string& data) {
    const std::wstring path = Join(fixture.root, (L"replacement-payload-" + std::to_wstring(index) + L".fixture").c_str());
    if (!Require(NewText(path, data), "write replacement inert payload source")) return false;
    fixture.data[index] = data;
    fixture.hashes[index] = FileSha256(path);
    fixture.payloads[index] = {kNames[index], reinterpret_cast<const unsigned char*>(fixture.data[index].data()),
                              fixture.data[index].size(), fixture.hashes[index].c_str()};
    return Require(fixture.hashes[index].size() == 64, "hash replacement inert payload source");
}

void TestMissingRecovery(const std::wstring& root) {
    for (unsigned i = 0; i < 9; ++i) {
        Fixture repair(root, (L"missing-repair-" + std::to_wstring(i)).c_str());
        if (ReadyInstalled(repair)) {
            const std::wstring path = i < 6 ? repair.installed(kNames[i]) : i == 6 ? repair.installed(kManagerName) :
                i == 7 ? repair.desktopLink() : repair.programsLink();
            const std::wstring receiptHash = FileSha256(repair.installed(kManifestName));
            const std::wstring objectHash = FileSha256(path);
            if (!Require(DeleteFileW(path.c_str()) != FALSE, "remove exactly one fixture-owned resource before repair")) return;
            const Result result = repair.install();
            Report(result);
            Check(result.ok && result.repaired && !result.alreadyInstalled && FileSha256(path) == objectHash && repair.filesMatch() &&
                  FileSha256(repair.installed(kManifestName)) == receiptHash, "each missing payload/manager/shortcut is restored with its exact recorded bytes and immutable receipt");
            Check(repair.originalsUnchanged(), "missing-resource repair never writes the original application");
        }
        Fixture remove(root, (L"missing-remove-" + std::to_wstring(i)).c_str());
        if (ReadyInstalled(remove)) {
            const std::wstring path = i < 6 ? remove.installed(kNames[i]) : i == 6 ? remove.installed(kManagerName) :
                i == 7 ? remove.desktopLink() : remove.programsLink();
            if (!Require(DeleteFileW(path.c_str()) != FALSE, "remove exactly one fixture-owned resource before uninstall")) return;
            const Result result = remove.uninstall();
            Report(result);
            Check(result.ok && remove.ownedGone() && remove.originalsUnchanged(), "each absent payload/manager/shortcut is harmless during uninstall");
        }
    }
    Fixture f(root, L"missing-shortcut-directory");
    if (ReadyInstalled(f) && Require(DeleteFileW(f.desktopLink().c_str()) && RemoveDirectoryW(f.locations.desktopDirectory.c_str()),
                                    "remove fixture desktop link and its now-empty private directory")) {
        const Result result = f.install();
        Check(result.ok && result.repaired && ReadLink(f.desktopLink()).ok && f.filesMatch(), "repair safely recreates an absent shortcut directory as well as its link");
    }
    Fixture missingDirectory(root, L"remove-missing-shortcut-directory");
    if (ReadyInstalled(missingDirectory) && Require(DeleteFileW(missingDirectory.desktopLink().c_str()) &&
        RemoveDirectoryW(missingDirectory.locations.desktopDirectory.c_str()), "remove only the second fixture desktop tree")) {
        const Result result = missingDirectory.uninstall();
        Check(result.ok && missingDirectory.ownedGone(), "uninstall also accepts a completely absent shortcut directory");
    }
}

void TestRemovalCheckpoints(const std::wstring& root) {
    // Fresh uninstall: durable intent, six payloads, two links, manager LAST.
    for (unsigned step = 1; step <= 10; ++step) {
        Fixture f(root, (L"interrupted-removal-" + std::to_wstring(step)).c_str());
        if (!ReadyInstalled(f)) return;
        const std::wstring receiptHash = FileSha256(f.installed(kManifestName));
        FaultInjection fault{step};
        f.locations.faultInjection = &fault;
        const Result interrupted = f.uninstall();
        Check(!interrupted.ok && interrupted.error == ERROR_OPERATION_ABORTED && interrupted.recoveryPending && fault.completedSteps == step &&
              Exists(f.installed(kManifestName)) && Exists(f.installed(kRemovalJournalName)) && f.originalsUnchanged(),
              "every destructive uninstall checkpoint retains a recognizable, restartable ownership state");
        Check((step == 10) == !Exists(f.installed(kManagerName)), "manager is present through every partial removal except its own final checkpoint");
        f.locations.faultInjection = nullptr;
        const Result resumed = f.uninstall();
        Report(resumed);
        const Snapshot removed(f);
        Check(resumed.ok && f.ownedGone() && f.uninstall().ok && removed.unchanged(f), "retry after each interrupted deletion finishes and repeated removal is read-only/idempotent");
        const Result reinstalled = f.install();
        Report(reinstalled);
        Check(reinstalled.ok && reinstalled.repaired && f.filesMatch() && FileSha256(f.installed(kManifestName)) == receiptHash,
              "reinstall after every partial removal restores resources without changing the receipt");
        Fixture direct(root, (L"direct-reinstall-partial-" + std::to_wstring(step)).c_str());
        if (!ReadyInstalled(direct)) return;
        FaultInjection directFault{step};
        direct.locations.faultInjection = &directFault;
        const Result directInterrupted = direct.uninstall();
        direct.locations.faultInjection = nullptr;
        const Result directRepair = direct.install();
        Check(!directInterrupted.ok && directInterrupted.recoveryPending && directRepair.ok && directRepair.repaired && direct.filesMatch(),
              "each partial removal can alternatively be repaired directly, without finishing uninstall first");
    }
    Fixture f(root, L"interrupted-reinstall-commit");
    if (ReadyInstalled(f) && Require(f.uninstall().ok, "prepare removed fixture for commit checkpoint")) {
        FaultInjection fault{1};
        f.locations.faultInjection = &fault;
        const Result interrupted = f.install();
        Check(!interrupted.ok && interrupted.recoveryPending && f.filesMatch() && !Exists(f.installed(kRemovalJournalName)),
              "failure immediately after removal-marker deletion leaves a complete verified install, never rolled-back files without intent");
        f.locations.faultInjection = nullptr;
        const Result retry = f.install();
        Check(retry.ok && retry.alreadyInstalled && f.filesMatch(), "retry after the final reinstall checkpoint recognizes the already committed tree");
    }
}

void TestMigration(const std::wstring& root) {
    Fixture f(root, L"configuration-key-migration");
    const std::string edited = "; user comment\r\n[fixture]\r\nWINDOWED=0\r\nunknown=retain exactly\r\n[personal]\r\nvalue=private\r\n";
    if (ReadyInstalled(f) && Require(RewriteText(f.installed(kNames[2]), edited), "edit fixture config before a default-key migration") &&
        ReplacePayload(f, 2, "[fixture]\r\nwindowed=1\r\nAddedDefault=17\r\n[future]\r\nEnabled=1\r\n")) {
        const std::wstring beforeHash = FileSha256(f.installed(kNames[2]));
        const std::wstring backup = f.installed((L"LakeWindowedFix.ini.before-" + beforeHash + L".bak").c_str());
        const std::wstring receiptHash = FileSha256(f.installed(kManifestName));
        const Result result = f.install();
        Report(result);
        const std::string expected = "; user comment\r\n[fixture]\r\nWINDOWED=0\r\nunknown=retain exactly\r\nAddedDefault=17\r\n[personal]\r\nvalue=private\r\n[future]\r\nEnabled=1\r\n";
        Check(result.ok && result.configurationMigrated && result.repaired && result.preservedConfiguration &&
              ReadBytes(f.installed(kNames[2])) == expected && ReadBytes(backup) == edited,
              "migration adds only case-insensitively absent keys/sections, retaining comments, edited values and unknown data with a hash-named exact backup");
        wchar_t value[64]{};
        GetPrivateProfileStringW(L"fixture", L"AddedDefault", L"missing", value, ARRAYSIZE(value), f.installed(kNames[2]).c_str());
        Check(std::wstring(value) == L"17" && FileSha256(f.installed(kManifestName)) == receiptHash &&
              !Exists(f.installed(kMigrationJournalName)) && !Exists(f.installed(kConfigurationStageName)),
              "new keys are effective under Win32 INI reading and completed migration does not rewrite the ownership receipt");
        const Snapshot complete(f);
        const Result again = f.install();
        Check(again.ok && again.alreadyInstalled && !again.configurationMigrated && complete.unchanged(f) && ReadBytes(backup) == edited,
              "default-key migration is idempotent and never regenerates or overwrites its backup");
        const Result removed = f.uninstall();
        const Result reinstalled = f.install();
        Check(removed.ok && removed.preservedConfiguration && reinstalled.ok && reinstalled.repaired &&
              ReadBytes(f.installed(kNames[2])) == expected && ReadBytes(backup) == edited,
              "migrated config and identifiable backup survive uninstall/reinstall");
    }
    for (unsigned step = 1; step <= 4; ++step) {
        Fixture interrupted(root, (L"interrupted-config-" + std::to_wstring(step)).c_str());
        const std::string original = "; retain\r\n[fixture]\r\nunknown=user edit\r\n";
        if (!ReadyInstalled(interrupted) || !Require(RewriteText(interrupted.installed(kNames[2]), original), "prepare incomplete fixture INI")) return;
        const std::wstring beforeHash = FileSha256(interrupted.installed(kNames[2]));
        const std::wstring backup = interrupted.installed((L"LakeWindowedFix.ini.before-" + beforeHash + L".bak").c_str());
        FaultInjection fault{step};
        interrupted.locations.faultInjection = &fault;
        const Result result = interrupted.install();
        Check(!result.ok && result.recoveryPending && result.error == ERROR_OPERATION_ABORTED && fault.completedSteps == step &&
              (ReadBytes(interrupted.installed(kNames[2])) == original || ReadBytes(backup) == original) &&
              Exists(interrupted.installed(kManagerName)) && Exists(interrupted.installed(kManifestName)) && interrupted.originalsUnchanged(),
              "every config checkpoint keeps the exact original INI at its path or backup, plus recoverable intent until commit");
        interrupted.locations.faultInjection = nullptr;
        const Result retry = interrupted.install();
        Report(retry);
        Check(retry.ok && ReadBytes(backup) == original &&
              ReadBytes(interrupted.installed(kNames[2])) == original + "windowed=1\r\n" &&
              !Exists(interrupted.installed(kMigrationJournalName)) && !Exists(interrupted.installed(kConfigurationStageName)),
              "retry after each config interruption rolls forward to a verified merged INI without losing original edits");
    }
    for (unsigned encoding = 0; encoding < 2; ++encoding) {
        Fixture encoded(root, (L"encoded-config-" + std::to_wstring(encoding)).c_str());
        if (!ReadyInstalled(encoded)) return;
        const std::wstring wide = L"; caf\u00e9\n[fixture]\nunknown=preserve\n";
        const std::string utf8 = "\xef\xbb\xbf; caf\xc3\xa9\n[fixture]\nunknown=preserve\n";
        const std::wstring utf16 = L"\xfeff" + wide;
        const std::string original = encoding ? std::string(reinterpret_cast<const char*>(utf16.data()), utf16.size() * sizeof(wchar_t)) : utf8;
        if (!Require(RewriteText(encoded.installed(kNames[2]), original), "write a fixture-owned BOM-marked INI")) return;
        const Result result = encoded.install();
        const std::string migrated = ReadBytes(encoded.installed(kNames[2]));
        Check(result.ok && result.configurationMigrated && migrated.substr(0, original.size()) == original,
              "UTF-8 BOM/UTF-16LE BOM migration retains original Unicode bytes and line endings");
    }
    {
        Fixture interrupted(root, L"foreign-config-stage");
        if (!ReadyInstalled(interrupted) || !Require(RewriteText(interrupted.installed(kNames[2]), "[fixture]\r\nunknown=keep\r\n"), "prepare config stage collision fixture")) return;
        FaultInjection fault{1};
        interrupted.locations.faultInjection = &fault;
        const Result first = interrupted.install();
        if (!Require(!first.ok && first.recoveryPending && RewriteText(interrupted.installed(kConfigurationStageName), "foreign changed stage"), "alter only the fixture-owned pending stage")) return;
        interrupted.locations.faultInjection = nullptr;
        const Snapshot before(interrupted);
        const Result retry = interrupted.install();
        Check(!retry.ok && retry.recoveryPending && before.unchanged(interrupted) &&
              ReadBytes(interrupted.installed(kConfigurationStageName)) == "foreign changed stage",
              "a changed recovery stage is preserved and cannot authorize renaming/deleting the live config");
    }
    {
        Fixture f(root, L"remove-during-config-gap");
        const std::string editedIni = "[fixture]\r\nunknown=user data\r\n";
        if (ReadyInstalled(f) && Require(RewriteText(f.installed(kNames[2]), editedIni), "prepare interrupted migration for uninstall")) {
            FaultInjection fault{2};
            f.locations.faultInjection = &fault;
            const Result interrupted = f.install();
            f.locations.faultInjection = nullptr;
            const Result removed = f.uninstall();
            Check(!interrupted.ok && removed.ok && removed.preservedConfiguration && f.ownedGone(true) &&
                  ReadBytes(f.installed(kNames[2])) == editedIni + "windowed=1\r\n" && !Exists(f.installed(kMigrationJournalName)),
                  "uninstall recovers the config rename gap before deleting owned resources and preserves the merged user INI");
        }
    }
    {
        Fixture f(root, L"ambiguous-config-preserved");
        const std::string ambiguous = "[fixture]\r\nunknown=one\r\n[fixture]\r\nunknown=two\r\n";
        if (ReadyInstalled(f) && Require(RewriteText(f.installed(kNames[2]), ambiguous), "write ambiguous fixture INI")) {
            const Snapshot before(f);
            const Result result = f.install();
            Check(!result.ok && before.unchanged(f) && ReadBytes(f.installed(kNames[2])) == ambiguous &&
                  !Exists(f.installed(kMigrationJournalName)), "ambiguous repeated sections are preserved without guessing migration semantics or creating a journal");
        }
    }
}

void TestPreviousReceiptsAndUpdates(const std::wstring& root) {
    {
        Fixture f(root, L"previous-version-receipt");
        if (!ReadyInstalled(f)) return;
        const std::string bytes = ReadBytes(f.installed(kManifestName));
        std::wstring legacy(bytes.size() / sizeof(wchar_t), L'\0');
        std::memcpy(legacy.data(), bytes.data(), bytes.size());
        const std::size_t shortcuts = legacy.find(L"[shortcuts]\r\n");
        if (!Require(shortcuts != std::wstring::npos, "locate schema 2 links in the fixture receipt")) return;
        legacy.resize(shortcuts);
        legacy.replace(legacy.find(L"schema=2"), 8, L"schema=1");
        const std::wstring version = std::wstring(L"version=") + kVersion;
        legacy.replace(legacy.find(version), version.size(), L"version=0.6.0");
        if (!Require(Rewrite(f.installed(kManifestName), legacy.data(), legacy.size() * sizeof(wchar_t)), "write canonical baseline schema 1 fixture receipt") ||
            !ReplacePayload(f, 0, "new installer launcher payload, NEVER EXECUTE\r\n") ||
            !Require(RewriteText(f.source, "new inert setup bytes, NEVER EXECUTE\r\n"), "change the new package manager source")) return;
        const Snapshot before(f);
        const Result update = f.install();
        Check(update.updateBlocked && update.error == ERROR_REVISION_MISMATCH && RefusedWithoutChanges(f, update, before),
              "recognized 0.6 receipt explicitly blocks unsupported executable update without changing any installed resource");
        const Result removed = f.uninstall();
        Report(removed);
        Check(removed.ok && f.ownedGone() && f.originalsUnchanged(), "new installer uninstalls the previous version using its own manifest hashes, not the new payload hashes");
    }
    {
        Fixture f(root, L"same-version-payload-update-blocked");
        if (ReadyInstalled(f) && ReplacePayload(f, 1, "different newer synthetic DLL, NEVER LOAD\r\n")) {
            const Snapshot before(f);
            const Result result = f.install();
            Check(result.updateBlocked && result.error == ERROR_REVISION_MISMATCH && RefusedWithoutChanges(f, result, before),
                  "even same-version changed executable payloads are blocked explicitly, not half-updated");
        }
    }
    {
        Fixture f(root, L"foreign-removal-journal");
        if (ReadyInstalled(f) && Require(NewText(f.installed(kRemovalJournalName), "foreign journal bytes"), "create reserved-name foreign journal fixture")) {
            const Snapshot before(f);
            Check(RefusedWithoutChanges(f, f.uninstall(), before) && RefusedWithoutChanges(f, f.install(), before),
                  "an unknown removal journal is never adopted, overwritten or deleted");
        }
    }
    {
        Fixture f(root, L"fault-injection-registry-guard");
        if (!f.ready) return;
        FaultInjection fault{1};
        f.locations.faultInjection = &fault;
        f.locations.registerUninstall = true;
        const Result result = f.install();
        Check(!result.ok && result.error == ERROR_INVALID_PARAMETER && f.noInstallOutput(),
              "fault injection is rejected before any write when registry/global-shell operations are enabled");
    }
}

bool LegacyReceipt(Fixture& f) {
    const std::string bytes = ReadBytes(f.installed(kManifestName));
    std::wstring text(bytes.size() / sizeof(wchar_t), L'\0');
    if (bytes.empty() || bytes.size() % sizeof(wchar_t)) return false;
    std::memcpy(text.data(), bytes.data(), bytes.size());
    const std::size_t shortcuts = text.find(L"[shortcuts]\r\n");
    if (shortcuts == std::wstring::npos) return false;
    text.resize(shortcuts);
    text.replace(text.find(L"schema=2"), 8, L"schema=1");
    const std::wstring version = std::wstring(L"version=") + kVersion;
    text.replace(text.find(version), version.size(), L"version=0.6.0");
    if (!Rewrite(f.installed(kManifestName), text.data(), text.size() * sizeof(wchar_t))) return false;
    if (f.locations.registerUninstall) {
        if (f.locations.testRegistryKey.empty()) return false;
        HKEY key = nullptr;
        LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, f.locations.testRegistryKey.c_str(), 0, KEY_SET_VALUE, &key);
        if (status != ERROR_SUCCESS) return false;
        constexpr wchar_t old[] = L"0.6.0";
        status = RegSetValueExW(key, L"DisplayVersion", 0, REG_SZ, reinterpret_cast<const BYTE*>(old), sizeof(old));
        RegCloseKey(key);
        if (status != ERROR_SUCCESS) return false;
    }
    return true;
}

bool EnablePrivateRegistry(Fixture& f) {
    GUID guid{};
    wchar_t text[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, text, ARRAYSIZE(text))) return false;
    f.locations.testRegistryKey = std::wstring(L"Software\\LakeWindowedFix.SetupTests\\") + text;
    f.locations.registerUninstall = true;
    return true;
}

constexpr char kUpgradeEditedIni[] = "; keep user comment\r\n[fixture]\r\nWINDOWED=0\r\nunknown=user data\r\n";
constexpr char kUpgradeMergedIni[] = "; keep user comment\r\n[fixture]\r\nWINDOWED=0\r\nunknown=user data\r\nUpgradeDefault=7\r\n";

bool PrepareUpgradeFixture(Fixture& f, bool registry = false, bool legacy = true) {
    if (!f.ready || (registry && !Require(EnablePrivateRegistry(f), "allocate exclusive GUID HKCU test key")) || !ReadyInstalled(f)) return false;
    if (!Require((!legacy || LegacyReceipt(f)) && RewriteText(f.installed(kNames[2]), kUpgradeEditedIni) &&
                 NewText(f.installed(L"upgrade-user.log"), "persistent user log\r\n"), "prepare actual schema 1 receipt, edited INI and persistent log")) return false;
    return ReplacePayload(f, 0, "new 0.7 launcher fixture, NEVER EXECUTE\r\n") &&
        ReplacePayload(f, 1, "new 0.7 DLL fixture, NEVER LOAD\r\n") &&
        ReplacePayload(f, 2, "[fixture]\r\nwindowed=1\r\nUpgradeDefault=7\r\n") &&
        Require(RewriteText(f.source, "new 0.7 setup fixture, NEVER EXECUTE\r\n"), "prepare new manager fixture outside installation");
}

Result UpgradeFixture(const Fixture& f) {
    // Empty app exercises discovery from the original manifest even if the
    // active receipt has been moved aside by an interrupted promotion.
    return Upgrade(f.locations, L"", f.payloads.data(), f.payloads.size(), f.source, f.build);
}

bool NewRuntimeMatches(const Fixture& f) {
    for (std::size_t i = 0; i < 6; ++i)
        if (i != 2 && FileSha256(f.installed(kNames[i])) != f.hashes[i]) return false;
    return FileSha256(f.installed(kManagerName)) == FileSha256(f.source) &&
        ReadBytes(f.installed(kNames[2])) == kUpgradeMergedIni && ReadLink(f.desktopLink()).ok && ReadLink(f.programsLink()).ok &&
        Exists(f.installed(kManifestName)) && !Exists(f.installed(kRemovalJournalName)) && f.originalsUnchanged() &&
        ReadBytes(f.installed(L"upgrade-user.log")) == "persistent user log\r\n";
}

bool OldRuntimeMatches(const Fixture& f, const Snapshot& old) {
    const Snapshot now(f);
    for (std::size_t i = 0; i < old.hashes.size(); ++i)
        if (i != 2 && old.hashes[i] != now.hashes[i]) return false;
    const std::string ini = ReadBytes(f.installed(kNames[2]));
    return ini.find("WINDOWED=0") != std::string::npos && ini.find("unknown=user data") != std::string::npos &&
        ReadBytes(f.installed(L"upgrade-user.log")) == "persistent user log\r\n" && f.originalsUnchanged();
}

std::wstring UpgradeArtifact(const Fixture& f, const wchar_t* name) { return Join(f.installed(kUpgradeDirectoryName), name); }

void TestPayloadGate(const std::wstring& root) {
    Fixture f(root, L"pure-payload-verification");
    if (!f.ready) return;
    Check(VerifyPayload(f.payloads.data(), f.payloads.size()).ok && f.noInstallOutput(), "payload-only gate verifies exactly six embedded buffers without any install output");
    auto corrupt = f.payloads;
    const unsigned char wrong[] = "wrong bytes";
    corrupt[1].data = wrong; corrupt[1].size = sizeof(wrong) - 1;
    const Result crc = VerifyPayload(corrupt.data(), corrupt.size());
    corrupt = f.payloads; corrupt[1].name = L"foreign.exe";
    const Result name = VerifyPayload(corrupt.data(), corrupt.size());
    Check(!crc.ok && crc.error == ERROR_CRC && !name.ok && name.error == ERROR_INVALID_DATA &&
          !VerifyPayload(f.payloads.data(), 5).ok && f.noInstallOutput(), "payload gate returns CRC/invalid-data errors for altered bytes, foreign names and wrong count");
}

void RecordCheckpoint(const wchar_t* name, unsigned step, void* context) {
    auto* trace = static_cast<std::vector<std::pair<unsigned, std::wstring>>*>(context);
    trace->push_back({step, name});
}

bool RegistryCheckpoint(const std::vector<std::pair<unsigned, std::wstring>>& trace, unsigned step) {
    for (const auto& entry : trace) if (entry.first == step)
        return entry.second.find(L"registry") != std::wstring::npos || entry.second == L"upgrade-prepared" ||
            entry.second == L"rollback-requested" || entry.second == L"upgrade-committed" || entry.second == L"rollback-committed";
    return false;
}

void TestTransactionalUpgrade(const std::wstring& root) {
    for (unsigned registry = 0; registry < 2; ++registry) {
        Fixture complete(root, registry ? L"upgrade-registry-complete" : L"upgrade-files-complete");
        if (!PrepareUpgradeFixture(complete, registry != 0)) return;
        const Snapshot old(complete);
        FaultInjection measure{};
        std::vector<std::pair<unsigned, std::wstring>> trace;
        measure.observe = RecordCheckpoint; measure.context = &trace;
        complete.locations.faultInjection = &measure;
        const Result upgraded = UpgradeFixture(complete);
        Report(upgraded);
        if (!Require(upgraded.ok && upgraded.upgraded && NewRuntimeMatches(complete), "complete schema 1 to schema 2 upgrade succeeds before fault matrix")) return;
        const unsigned upgradeSteps = measure.completedSteps;
        const auto upgradeTrace = trace;
        Check(upgradeSteps > 20 && UpgradeFixture(complete).alreadyInstalled && complete.install().alreadyInstalled,
              "promoted installation is complete and recognized by both upgrade retry and ordinary installer");
        measure.completedSteps = 0;
        trace.clear();
        const Result reverted = Rollback(complete.locations);
        Report(reverted);
        if (!Require(reverted.ok && reverted.rolledBack && OldRuntimeMatches(complete, old), "complete explicit rollback restores the original generation")) return;
        const unsigned rollbackSteps = measure.completedSteps;
        const auto rollbackTrace = trace;
        printf("upgrade fault matrix (exclusive registry=%u): %u upgrade checkpoints, %u rollback checkpoints\n", registry, upgradeSteps, rollbackSteps);
        Check(rollbackSteps > 2 && Rollback(complete.locations).alreadyInstalled && ReadBytes(complete.installed(kNames[2])) == kUpgradeMergedIni,
              "explicit rollback is idempotent and retains the merged persistent INI, not old defaults");
        for (unsigned step = 1; step <= upgradeSteps; ++step) {
            if (registry && !RegistryCheckpoint(upgradeTrace, step)) continue;
            const std::wstring suffix = std::to_wstring(registry) + L"-" + std::to_wstring(step);
            Fixture f(root, (L"upgrade-fault-" + suffix).c_str());
            if (!PrepareUpgradeFixture(f, registry != 0)) return;
            const Snapshot before(f);
            FaultInjection fault{step};
            f.locations.faultInjection = &fault;
            const Result interrupted = UpgradeFixture(f);
            const bool staging = fault.lastStep && (std::wcsstr(fault.lastStep, L"upgrade-staging-") == fault.lastStep ||
                                                    !std::wcscmp(fault.lastStep, L"upgrade-prepared"));
            Check(!interrupted.ok && interrupted.recoveryPending && interrupted.error == ERROR_OPERATION_ABORTED && fault.completedSteps == step &&
                  (!staging || before.unchanged(f)) && f.originalsUnchanged(),
                  "every upgrade checkpoint is recoverable; all staging/prepared failures leave the old generation usable byte-for-byte");
            f.locations.faultInjection = nullptr;
            if (Exists(UpgradeArtifact(f, L"plan.ini")) && !Exists(UpgradeArtifact(f, L"committed.ini"))) {
                const Snapshot pending(f);
                Check(!f.install().ok && !f.uninstall().ok && pending.unchanged(f), "ordinary install/uninstall cannot mutate an unfinished upgrade");
            }
            const Result resumed = UpgradeFixture(f);
            Report(resumed);
            Check(resumed.ok && NewRuntimeMatches(f) && UpgradeFixture(f).alreadyInstalled,
                  "retry after each upgrade checkpoint completes verified new files, links, manager, registry and receipt");
            Check(Rollback(f.locations).ok && OldRuntimeMatches(f, before), "each resumed upgrade retains an explicit usable rollback generation");

            if (registry) continue; // Direct reverse path exercises the same file swaps without doubling registry evidence.
            Fixture direct(root, (L"upgrade-direct-rollback-" + suffix).c_str());
            if (!PrepareUpgradeFixture(direct)) return;
            const Snapshot directBefore(direct);
            FaultInjection directFault{step};
            direct.locations.faultInjection = &directFault;
            const Result first = UpgradeFixture(direct);
            direct.locations.faultInjection = nullptr;
            const bool hasPlan = Exists(UpgradeArtifact(direct, L"plan.ini"));
            const Result undone = hasPlan ? Rollback(direct.locations) : Result{};
            Check(!first.ok && (hasPlan ? undone.ok && OldRuntimeMatches(direct, directBefore) : directBefore.unchanged(direct)),
                  "every file-promotion interruption can be reversed directly; pre-journal staging failures roll back their own new files");
        }
        for (unsigned step = 1; step <= rollbackSteps; ++step) {
            if (registry && !RegistryCheckpoint(rollbackTrace, step)) continue;
            Fixture f(root, (L"rollback-fault-" + std::to_wstring(registry) + L"-" + std::to_wstring(step)).c_str());
            if (!PrepareUpgradeFixture(f, registry != 0)) return;
            const Snapshot before(f);
            if (!Require(UpgradeFixture(f).ok && RewriteText(f.installed(kNames[2]), std::string(kUpgradeMergedIni) + "postUpgradeEdit=keep\r\n"),
                         "prepare rollback with an additional user edit after promotion")) return;
            FaultInjection fault{step};
            f.locations.faultInjection = &fault;
            const Result interrupted = Rollback(f.locations);
            Check(!interrupted.ok && interrupted.recoveryPending && interrupted.error == ERROR_OPERATION_ABORTED && fault.completedSteps == step,
                  "every rollback checkpoint leaves durable reversal intent and verified archived originals");
            f.locations.faultInjection = nullptr;
            const Result resumed = Rollback(f.locations);
            Report(resumed);
            Check(resumed.ok && OldRuntimeMatches(f, before) && Rollback(f.locations).alreadyInstalled &&
                  ReadBytes(f.installed(kNames[2])) == std::string(kUpgradeMergedIni) + "postUpgradeEdit=keep\r\n",
                  "retry after every rollback checkpoint restores old binaries/links/receipt/registry and preserves post-upgrade INI edits/logs");
        }
    }
}

void TestUpgradeConflictSafety(const std::wstring& root) {
    for (unsigned collision = 0; collision < 4; ++collision) {
        Fixture f(root, (L"upgrade-before-conflict-" + std::to_wstring(collision)).c_str());
        if (!PrepareUpgradeFixture(f)) return;
        if (!Require(collision == 0 ? RewriteText(f.installed(kNames[1]), "foreign changed DLL") : collision == 1 ?
            RewriteText(f.desktopLink(), "foreign link bytes") : collision == 2 ? CreateDirectoryW(f.installed(kUpgradeDirectoryName).c_str(), nullptr) != FALSE :
            RetargetOwnedLink(f.desktopLink(), f.installed(kNames[0]), L"--app-dir \"" + f.app + L"\"", f.app),
            "create only an explicitly fixture-owned upgrade conflict")) return;
        const Snapshot before(f);
        const Result result = UpgradeFixture(f);
        Check(!result.ok && before.unchanged(f) && f.originalsUnchanged(), "upgrade never overwrites an altered binary/link or adopts an unowned staging directory");
    }
    {
        Fixture f(root, L"upgrade-missing-old-resources");
        if (!PrepareUpgradeFixture(f) || !Require(DeleteFileW(f.installed(kNames[1]).c_str()) && DeleteFileW(f.desktopLink().c_str()),
                                                 "remove specific old fixture resources before upgrade")) return;
        const Snapshot before(f);
        const Result result = UpgradeFixture(f);
        Check(result.ok && NewRuntimeMatches(f) && Rollback(f.locations).ok && OldRuntimeMatches(f, before),
              "old missing file/link do not block upgrade and rollback restores their exact absent state");
    }
    for (unsigned changed = 0; changed < 2; ++changed) {
        Fixture f(root, (L"upgrade-staged-conflict-" + std::to_wstring(changed)).c_str());
        if (!PrepareUpgradeFixture(f)) return;
        FaultInjection fault{21}; // Ten before archives + ten after/next pairs, then durable plan.
        f.locations.faultInjection = &fault;
        const Result prepared = UpgradeFixture(f);
        if (!Require(!prepared.ok && fault.lastStep && !std::wcscmp(fault.lastStep, L"upgrade-prepared"), "interrupt exactly after verified staging, before first promotion")) return;
        f.locations.faultInjection = nullptr;
        const std::wstring path = changed ? f.installed(kNames[1]) : UpgradeArtifact(f, L"after-1.bin");
        if (!Require(RewriteText(path, "foreign changed bytes after staging"), "change exactly the fixture-owned staged/live conflict target")) return;
        const Snapshot before(f);
        const Result resume = UpgradeFixture(f), reverse = Rollback(f.locations);
        Check(!resume.ok && !reverse.ok && before.unchanged(f) && ReadBytes(path) == "foreign changed bytes after staging",
              "altered archive or active binary blocks both directions before further destructive writes, retaining the changed bytes");
    }
}

void StopAtUpgradePrepared(const wchar_t* name, unsigned step, void* context) {
    if (!std::wcscmp(name, L"upgrade-prepared")) static_cast<FaultInjection*>(context)->failAfterStep = step;
}

struct ConfigGap { FaultInjection* fault; const Fixture* fixture; };
void StopAtUpgradeConfigGap(const wchar_t* name, unsigned step, void* context) {
    auto* gap = static_cast<ConfigGap*>(context);
    if (!std::wcscmp(name, L"upgrade-active-retired") && !Exists(gap->fixture->installed(kNames[2]))) gap->fault->failAfterStep = step;
}

void TestRollbackConfigGap(const std::wstring& root) {
    unsigned steps = 0;
    for (unsigned fail = 0; fail <= steps; ++fail) {
        Fixture f(root, (L"rollback-config-gap-" + std::to_wstring(fail)).c_str());
        if (!PrepareUpgradeFixture(f)) return;
        const Snapshot before(f);
        FaultInjection stop{};
        ConfigGap gap{&stop, &f};
        stop.observe = StopAtUpgradeConfigGap; stop.context = &gap;
        f.locations.faultInjection = &stop;
        const Result interrupted = UpgradeFixture(f);
        if (!Require(!interrupted.ok && !Exists(f.installed(kNames[2])), "interrupt exactly in the config promotion rename gap")) return;
        FaultInjection reverse{fail};
        f.locations.faultInjection = &reverse;
        const Result rollback = Rollback(f.locations);
        if (!fail) {
            if (!Require(rollback.ok, "measure successful rollback from a missing live INI")) return;
            steps = reverse.completedSteps;
        } else Check(!rollback.ok && rollback.recoveryPending && rollback.error == ERROR_OPERATION_ABORTED,
                     "each checkpoint of rollback from the config gap is recoverable, including original-INI publication");
        f.locations.faultInjection = nullptr;
        const Result resumed = Rollback(f.locations);
        Check(resumed.ok && OldRuntimeMatches(f, before) && ReadBytes(f.installed(kNames[2])) == kUpgradeEditedIni,
              "rollback from a missing promoted INI restores exact original edits after every interruption");
    }
}

void TestUpgradeLifecycleEdges(const std::wstring& root) {
    {
        Fixture f(root, L"identical-upgrade-is-repair");
        if (!ReadyInstalled(f)) return;
        const Snapshot before(f);
        const Result result = UpgradeFixture(f);
        Check(result.ok && result.alreadyInstalled && before.unchanged(f) && !Exists(f.installed(kUpgradeDirectoryName)),
              "requesting the identical package is read-only repair, not a consumed rollback generation");
    }
    {
        Fixture f(root, L"same-version-transactional-upgrade");
        if (!PrepareUpgradeFixture(f, false, false)) return;
        const Snapshot before(f);
        const Result result = UpgradeFixture(f);
        Check(result.ok && result.upgraded && NewRuntimeMatches(f) && Rollback(f.locations).ok && OldRuntimeMatches(f, before),
              "changed executable/manager payloads at the same product version use the same verified transaction and explicit rollback");
        const Snapshot reverted(f);
        const Result repeat = UpgradeFixture(f);
        Check(!repeat.ok && repeat.updateBlocked && repeat.error == ERROR_NOT_SUPPORTED && reverted.unchanged(f),
              "terminal rollback history reports the unsupported redo/rotation limit explicitly, without pretending recovery is still pending");
    }
    {
        Fixture f(root, L"upgrade-from-preserved-removed-legacy");
        if (!PrepareUpgradeFixture(f) || !Require(f.uninstall().ok, "remove old generation while retaining schema 1 receipt, INI and log")) return;
        const Snapshot removed(f);
        const Result result = UpgradeFixture(f);
        Check(result.ok && NewRuntimeMatches(f) && Rollback(f.locations).ok && OldRuntimeMatches(f, removed) &&
              Exists(f.installed(kRemovalJournalName)), "preserved removed legacy data can be upgraded; rollback restores the original removed state and intent");
    }
    {
        Fixture f(root, L"completed-history-not-runtime-dependency");
        if (!PrepareUpgradeFixture(f) || !Require(UpgradeFixture(f).ok && DeleteFileW(UpgradeArtifact(f, L"before-1.bin").c_str()),
                                                 "remove one explicit fixture-owned old archive after successful commit")) return;
        const Snapshot active(f);
        const Result rollback = Rollback(f.locations);
        Check(!rollback.ok && active.unchanged(f) && f.install().alreadyInstalled,
              "lost rollback archive prevents unsafe rollback but does not block read-only recognition of a healthy active installation");
        Check(f.uninstall().ok && f.install().ok && NewRuntimeMatches(f),
              "missing history archive cannot block safe active uninstall/reinstall from the current receipt");
    }
    {
        Fixture f(root, L"upgrade-with-missing-shortcut-directory");
        if (!PrepareUpgradeFixture(f) || !Require(DeleteFileW(f.desktopLink().c_str()) && RemoveDirectoryW(f.locations.desktopDirectory.c_str()),
                                                 "remove a fixture-owned shortcut and its empty private parent before upgrade")) return;
        const Snapshot before(f);
        const Result result = UpgradeFixture(f);
        Check(result.ok && NewRuntimeMatches(f) && Rollback(f.locations).ok && OldRuntimeMatches(f, before),
              "upgrade recreates an absent shortcut directory safely; rollback restores the former link absence");
    }
    {
        Fixture f(root, L"upgrade-with-missing-private-registry");
        if (!PrepareUpgradeFixture(f, true) || !Require(RegDeleteKeyW(HKEY_CURRENT_USER, f.locations.testRegistryKey.c_str()) == ERROR_SUCCESS,
                                                       "remove exactly the owned GUID test key before upgrade")) return;
        const Snapshot before(f);
        const Result result = UpgradeFixture(f);
        const Result rollback = Rollback(f.locations);
        HKEY key = nullptr;
        const LSTATUS absent = RegOpenKeyExW(HKEY_CURRENT_USER, f.locations.testRegistryKey.c_str(), 0, KEY_QUERY_VALUE, &key);
        if (key) RegCloseKey(key);
        Check(result.ok && rollback.ok && OldRuntimeMatches(f, before) && absent == ERROR_FILE_NOT_FOUND,
              "transaction records an absent old exclusive registry key and restores absence on rollback");
    }
    {
        Fixture f(root, L"foreign-private-registry-after-staging");
        if (!PrepareUpgradeFixture(f, true)) return;
        FaultInjection fault{};
        fault.observe = StopAtUpgradePrepared; fault.context = &fault;
        f.locations.faultInjection = &fault;
        const Result prepared = UpgradeFixture(f);
        f.locations.faultInjection = nullptr;
        if (!Require(!prepared.ok && fault.lastStep && !std::wcscmp(fault.lastStep, L"upgrade-prepared"), "prepare an exclusive registry conflict before promotion")) return;
        HKEY key = nullptr;
        LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, f.locations.testRegistryKey.c_str(), 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key);
        constexpr wchar_t foreign[] = L"foreign user-edited display name";
        if (status == ERROR_SUCCESS) status = RegSetValueExW(key, L"DisplayName", 0, REG_SZ, reinterpret_cast<const BYTE*>(foreign), sizeof(foreign));
        if (key) RegCloseKey(key);
        if (!Require(status == ERROR_SUCCESS, "edit only a value in the fixture-owned GUID registry key")) return;
        const Snapshot before(f);
        const Result forward = UpgradeFixture(f), reverse = Rollback(f.locations);
        wchar_t value[128]{};
        DWORD type = 0, size = sizeof(value);
        status = RegOpenKeyExW(HKEY_CURRENT_USER, f.locations.testRegistryKey.c_str(), 0, KEY_QUERY_VALUE, &key);
        if (status == ERROR_SUCCESS) {
            status = RegQueryValueExW(key, L"DisplayName", nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
            RegCloseKey(key);
        }
        Check(!forward.ok && !reverse.ok && before.unchanged(f) && status == ERROR_SUCCESS && type == REG_SZ && std::wstring(value) == foreign,
              "a foreign/edited exclusive registry key blocks promotion and rollback before changing any active file");
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

void TestHardLinkRejection(const std::wstring& root) {
    Fixture f(root, L"hard-linked-owned-resource");
    if (!ReadyInstalled(f)) return;
    const std::wstring second = Join(f.root, L"shared-fixture-bytes.dll");
    if (!Require(CreateHardLinkW(second.c_str(), f.installed(kNames[1]).c_str(), nullptr) != FALSE,
                 "create a hard link only to the fixture-owned inert DLL")) return;
    const Snapshot before(f);
    Check(RefusedWithoutChanges(f, f.install(), before) && RefusedWithoutChanges(f, f.uninstall(), before) &&
          ReadBytes(second) == f.data[1], "hard-linked resources cannot acquire private setup ownership or be unlinked by repair/uninstall");
    Require(DeleteFileW(second.c_str()) != FALSE, "remove only the explicit fixture-created extra hard link");
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
        TestMissingRecovery(root);
        TestRemovalCheckpoints(root);
        TestMigration(root);
        TestPreviousReceiptsAndUpdates(root);
        TestPayloadGate(root);
        TestTransactionalUpgrade(root);
        TestUpgradeConflictSafety(root);
        TestUpgradeLifecycleEdges(root);
        TestRollbackConfigGap(root);
        TestHardLinkRejection(root);
        TestReparseIfAvailable(root);
    }
    CoUninitialize();
    printf("setup_engine: %d checks, %d failures, %d optional skips (no skipped check counted as PASS)\n", checks, failures, skips);
    return failures ? 1 : 0;
}
