#pragma once
#include <windows.h>
#include <cwchar>
#include <cerrno>
#include "product_version.h"
#include "file_identity.h"

namespace lakefix {
inline constexpr DWORD kConfigMissing = MAXDWORD;
enum class ConfigKey : unsigned { NativeResize, ScaleRendering, WindowWidth, WindowHeight, MinWidth, MinHeight,
                                  CanvasWidth, CanvasHeight, SmoothFonts, SystemCursor, DetailedInput, Count };
enum class ConfigOrigin { MissingDefault, InvalidDefault, Requested };
enum class ConfigResolution { None, CompatibilityMode, ResourceUnavailable };
struct ConfigDefinition { const wchar_t* section; const wchar_t* name; LONG fallback, minimum, maximum; };
inline constexpr ConfigDefinition configDefinitions[] = {
    {L"compatibility", L"NativeResize", 0, 0, 1}, {L"compatibility", L"ScaleRendering", 1, 0, 1},
    {L"window", L"Width", 1024, 320, 8192}, {L"window", L"Height", 576, 180, 8192},
    {L"window", L"MinWidth", 800, 320, 8192}, {L"window", L"MinHeight", 450, 180, 8192},
    {L"render", L"Width", 1088, 1024, 4096}, {L"render", L"Height", 612, 600, 4096},
    {L"render", L"SmoothFonts", 1, 0, 1}, {L"input", L"SystemCursor", 1, 0, 1},
    {L"diagnostics", L"DetailedInput", 0, 0, 1}
};
struct ConfigValue {
    LONG requested = 0, applied = 0;
    ConfigOrigin origin = ConfigOrigin::MissingDefault;
    ConfigResolution resolution = ConfigResolution::None;
};
struct SessionConfig {
    ConfigValue values[static_cast<unsigned>(ConfigKey::Count)]{};
    char effectiveSha256[65]{};
    bool adjacentIni = false;
    LONG Get(ConfigKey key) const noexcept { return values[static_cast<unsigned>(key)].applied; }
};

inline bool HashEffectiveConfig(SessionConfig& config) noexcept {
    LONG effective[static_cast<unsigned>(ConfigKey::Count) + 1]{LAKE_FIX_CONFIG_SCHEMA};
    for (unsigned i = 0; i < static_cast<unsigned>(ConfigKey::Count); ++i) effective[i + 1] = config.values[i].applied;
    return Sha256Bytes(reinterpret_cast<const BYTE*>(effective), sizeof(effective), config.effectiveSha256);
}

inline ConfigValue ParseConfigValue(const ConfigDefinition& definition, const wchar_t* text, bool missing) noexcept {
    ConfigValue result{definition.fallback, definition.fallback, ConfigOrigin::MissingDefault};
    if (missing) return result;
    result.origin = ConfigOrigin::InvalidDefault;
    if (!text || !*text) return result;
    errno = 0;
    wchar_t* end{};
    const long long value = wcstoll(text, &end, 10);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (errno || end == text || !end || *end || value < definition.minimum || value > definition.maximum) return result;
    result.requested = result.applied = static_cast<LONG>(value);
    result.origin = ConfigOrigin::Requested;
    return result;
}

// Query is injected for policy tests. Production holds the INI without write or
// delete sharing for the entire snapshot; no option is re-read during a session.
template<class Query> bool ReadSessionConfig(Query query, SessionConfig& config) noexcept {
    wchar_t text[128]{};
    const DWORD schemaLength = query(L"session", L"Schema", text, ARRAYSIZE(text));
    if (schemaLength != kConfigMissing && (schemaLength >= ARRAYSIZE(text) - 1 || wcscmp(text, L"1") != 0)) return false;
    for (unsigned i = 0; i < static_cast<unsigned>(ConfigKey::Count); ++i) {
        const auto& definition = configDefinitions[i];
        const DWORD length = query(definition.section, definition.name, text, ARRAYSIZE(text));
        config.values[i] = ParseConfigValue(definition, length >= ARRAYSIZE(text) - 1 ? L"" : text, length == kConfigMissing);
    }
    if (config.Get(ConfigKey::NativeResize)) {
        auto& scale = config.values[static_cast<unsigned>(ConfigKey::ScaleRendering)];
        if (scale.applied) { scale.applied = 0; scale.resolution = ConfigResolution::CompatibilityMode; }
        auto& width = config.values[static_cast<unsigned>(ConfigKey::WindowWidth)];
        auto& height = config.values[static_cast<unsigned>(ConfigKey::WindowHeight)];
        if (width.applied < 800) { width.applied = 1024; width.resolution = ConfigResolution::CompatibilityMode; }
        if (height.applied < 600) { height.applied = 768; height.resolution = ConfigResolution::CompatibilityMode; }
    }
    if (!config.Get(ConfigKey::ScaleRendering)) {
        for (const auto key : {ConfigKey::SmoothFonts, ConfigKey::SystemCursor, ConfigKey::DetailedInput}) {
            auto& value = config.values[static_cast<unsigned>(key)];
            if (value.applied) { value.applied = 0; value.resolution = ConfigResolution::CompatibilityMode; }
        }
    }
    return HashEffectiveConfig(config);
}
} // namespace lakefix
