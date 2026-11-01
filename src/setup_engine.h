#pragma once

#include <windows.h>
#include <cstddef>
#include <string>

namespace lakefix::setup {
struct PayloadFile {
    const wchar_t* name;
    const unsigned char* data;
    std::size_t size;
    const wchar_t* sha256;
};

struct Locations {
    std::wstring installDirectory;
    std::wstring desktopDirectory;
    std::wstring programsDirectory;
    bool registerUninstall = true;
};

struct SupportedBuild {
    std::wstring executableSha256;
    std::wstring rendererSha256;
};

struct Result {
    bool ok = false;
    bool alreadyInstalled = false;
    bool preservedConfiguration = false;
    DWORD error = ERROR_SUCCESS;
    std::wstring message;
};

const SupportedBuild& LakeBuild();
Locations DefaultLocations();
std::wstring FileSha256(const std::wstring& path);
bool LakeIsRunning();
Result ValidateApplication(const std::wstring& appDirectory, const SupportedBuild& build);
Result Install(const Locations& locations, const std::wstring& appDirectory,
               const PayloadFile* files, std::size_t count,
               const std::wstring& setupExecutable, const SupportedBuild& build);
// The caller must run outside installDirectory so the installed setup EXE can
// be removed. Only recorded, unchanged owned files/shortcuts/registry are removed.
Result Uninstall(const Locations& locations, const PayloadFile* files, std::size_t count);

inline constexpr wchar_t kVersion[] = L"0.6.0";
inline constexpr wchar_t kManagerName[] = L"LakeWindowedSetup.exe";
inline constexpr wchar_t kShortcutName[] = L"Lake Controller - Janela.lnk";
inline constexpr wchar_t kManifestName[] = L"lake-windowed-fix.setup.ini";
inline constexpr wchar_t kRegistryKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LakeWindowedFix-v0.6";
} // namespace lakefix::setup
