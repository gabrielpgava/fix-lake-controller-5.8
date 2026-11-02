#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/session_config.h"
#include "../src/layout_policy.h"
#include <cstdio>
#include <cstring>

int wmain() {
    int checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* name) { ++checks; if (!ok) ++failures; printf("%s: %s\n", ok ? "PASS" : "FAIL", name); };
    lakefix::SessionConfig defaults{}, legacy{};
    auto missing = [](const wchar_t*, const wchar_t*, wchar_t* text, DWORD) noexcept -> DWORD { *text = 0; return lakefix::kConfigMissing; };
    check(lakefix::ReadSessionConfig(missing, defaults), "missing INI yields a complete immutable default snapshot");
    check(defaults.Get(lakefix::ConfigKey::CanvasWidth) == 1088 && defaults.Get(lakefix::ConfigKey::CanvasHeight) == 612,
          "default fixed canvas remains 1088x612");
    check(defaults.Get(lakefix::ConfigKey::WindowWidth) == 1024 && defaults.Get(lakefix::ConfigKey::MinHeight) == 450,
          "window presentation defaults remain independent from canvas");
    check(std::strlen(defaults.effectiveSha256) == 64, "effective configuration has SHA256 independent from path");
    auto experimental = [](const wchar_t*, const wchar_t* name, wchar_t* text, DWORD count) noexcept -> DWORD {
        const wchar_t* value = wcscmp(name, L"NativeResize") == 0 ? L"1" : L"";
        wcscpy_s(text, count, value); return *value ? static_cast<DWORD>(wcslen(value)) : lakefix::kConfigMissing;
    };
    check(lakefix::ReadSessionConfig(experimental, legacy) && legacy.Get(lakefix::ConfigKey::ScaleRendering) == 0 &&
          legacy.Get(lakefix::ConfigKey::WindowHeight) == 768, "native resize override preserves legacy fallback without a live INI re-read");
    check(std::strcmp(defaults.effectiveSha256, legacy.effectiveSha256) != 0, "behavior override changes effective fingerprint");
    check(legacy.Get(lakefix::ConfigKey::SystemCursor) == 0 && legacy.Get(lakefix::ConfigKey::SmoothFonts) == 0 &&
          legacy.values[static_cast<unsigned>(lakefix::ConfigKey::ScaleRendering)].origin == lakefix::ConfigOrigin::MissingDefault &&
          legacy.values[static_cast<unsigned>(lakefix::ConfigKey::ScaleRendering)].resolution == lakefix::ConfigResolution::CompatibilityMode,
          "inactive scaled-only options and mode override retain the original missing/requested origin");
    const auto definition = lakefix::configDefinitions[static_cast<unsigned>(lakefix::ConfigKey::CanvasWidth)];
    for (const wchar_t* invalid : {L"-1", L"4097", L"abc", L"1088evil", L"9999999999999999999999", L""}) {
        const auto value = lakefix::ParseConfigValue(definition, invalid, false);
        check(value.applied == 1088 && value.origin == lakefix::ConfigOrigin::InvalidDefault, "invalid requested option is distinguished from missing default");
    }
    const auto custom = lakefix::ParseConfigValue(definition, L"2048", false);
    check(custom.applied == 2048 && custom.requested == 2048 && custom.origin == lakefix::ConfigOrigin::Requested,
          "supported custom dimension is not replaced by a universal default");
    auto future = [](const wchar_t*, const wchar_t*, wchar_t* text, DWORD count) noexcept -> DWORD {
        wcscpy_s(text, count, L"2"); return 1;
    };
    check(!lakefix::ReadSessionConfig(future, legacy), "future schema is rejected instead of silently misinterpreted");
    MEMORY_BASIC_INFORMATION layout{};
    layout.BaseAddress = reinterpret_cast<void*>(0x10001000);
    layout.AllocationBase = reinterpret_cast<void*>(0x10000000);
    layout.RegionSize = 4096;
    layout.State = MEM_COMMIT; layout.Type = MEM_IMAGE; layout.Protect = PAGE_READWRITE;
    check(lakefix::LayoutMemoryAdmitted(layout, 0x10000000, 8192, 4096), "layout policy admits only the bounded writable image region");
    layout.Protect |= PAGE_GUARD;
    check(!lakefix::LayoutMemoryAdmitted(layout, 0x10000000, 8192, 4096), "guard page is rejected before dereferencing dimensions");
    layout.Protect = PAGE_READWRITE; layout.Type = MEM_PRIVATE;
    check(!lakefix::LayoutMemoryAdmitted(layout, 0x10000000, 8192, 4096), "unrelated private allocation cannot admit build-specific RVAs");
    layout.Type = MEM_IMAGE;
    check(!lakefix::LayoutMemoryAdmitted(layout, 0x10000000, 4096, 4096), "layout extent must remain within admitted image");
    check(!lakefix::LayoutValuesAdmitted(0, 612) && !lakefix::LayoutValuesAdmitted(1088, 32769), "invalid native layout values are rejected");
    check(FAILED(lakefix::ScaledLayoutAdmission(true, false)), "eligible frame without admitted layout fails explicitly instead of requesting native forwarding");
    check(lakefix::ScaledLayoutAdmission(true, true) == S_OK && FAILED(lakefix::ScaledLayoutAdmission(false, true)),
          "layout admission is scoped to the owning canvas and independent of unrelated past errors");
    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
