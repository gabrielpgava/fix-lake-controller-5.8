#pragma once

#include <windows.h>
#include <cstddef>
#include <string>
#include "product_version.h"
#include "setup_text.h"

namespace lakefix::setup {
struct PayloadFile {
    const wchar_t* name;
    const unsigned char* data;
    std::size_t size;
    const wchar_t* sha256;
};

// Explicit sandbox-only crash checkpoints; never enabled by environment/CLI.
// Returning at a checkpoint leaves durable recovery records for the next call.
struct FaultInjection {
    unsigned failAfterStep = 0;
    unsigned completedSteps = 0;
    const wchar_t* lastStep = nullptr;
    void (*observe)(const wchar_t*, unsigned, void*) = nullptr;
    void* context = nullptr;
};

struct Locations {
    std::wstring installDirectory;
    std::wstring desktopDirectory;
    std::wstring programsDirectory;
    bool registerUninstall = true;
    // Empty selects the operational key. Nonempty is restricted to a GUID test
    // key under Software\LakeWindowedFix.SetupTests, never the operational key.
    std::wstring testRegistryKey;
    FaultInjection* faultInjection = nullptr;
};

struct SupportedBuild {
    std::wstring executableSha256;
    std::wstring rendererSha256;
};

struct Result {
    bool ok = false;
    bool alreadyInstalled = false;
    bool preservedConfiguration = false;
    bool repaired = false;
    bool configurationMigrated = false;
    bool preservedFiles = false;
    bool recoveryPending = false;
    bool updateBlocked = false;
    bool upgraded = false;
    bool rolledBack = false;
    DWORD error = ERROR_SUCCESS;
    std::wstring message;
    std::wstring messageEnglish;
    const std::wstring& Message(Language language) const noexcept {
        return language == Language::Portuguese ? message : messageEnglish;
    }
};

const SupportedBuild& LakeBuild();
Locations DefaultLocations();
std::wstring FileSha256(const std::wstring& path);
bool LakeIsRunning();
Result VerifyPayload(const PayloadFile* files, std::size_t count);
Result ValidateApplication(const std::wstring& appDirectory, const SupportedBuild& build);
Result Install(const Locations& locations, const std::wstring& appDirectory,
               const PayloadFile* files, std::size_t count,
               const std::wstring& setupExecutable, const SupportedBuild& build);
// The caller must run outside installDirectory so the installed setup EXE can
// be removed last. Missing objects are harmless; altered objects are preserved.
// The immutable manifest and removal marker remain as a reinstall/recovery receipt.
Result Uninstall(const Locations& locations, const PayloadFile* files, std::size_t count);
// Explicit external-manager transactions. Staging does not modify the active
// version; interrupted promotion must be resumed with Upgrade or rolled back.
// Rollback never discards an edited live INI or logs. Neither API starts Lake.
Result Upgrade(const Locations& locations, const std::wstring& appDirectory,
               const PayloadFile* files, std::size_t count,
               const std::wstring& setupExecutable, const SupportedBuild& build);
Result Rollback(const Locations& locations);

inline constexpr wchar_t kVersion[] = LAKE_FIX_VERSION_W;
inline constexpr wchar_t kManagerName[] = L"LakeWindowedSetup.exe";
inline constexpr wchar_t kShortcutName[] = L"Lake Controller - Janela.lnk";
inline constexpr wchar_t kManifestName[] = L"lake-windowed-fix.setup.ini";
inline constexpr wchar_t kRemovalJournalName[] = L"lake-windowed-fix.removing.ini";
inline constexpr wchar_t kMigrationJournalName[] = L"lake-windowed-fix.config-migration.ini";
inline constexpr wchar_t kConfigurationStageName[] = L"LakeWindowedFix.ini.setup-new";
inline constexpr wchar_t kUpgradeDirectoryName[] = L".lake-windowed-upgrade";
// Keep the historical location/key stable so 0.6 receipts are discoverable.
inline constexpr wchar_t kRegistryKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LakeWindowedFix-v0.6";
} // namespace lakefix::setup
