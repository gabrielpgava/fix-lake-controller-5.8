#pragma once
#include "setup_text.h"
#include <cstdint>
#include <limits>
#include <string>

namespace lakefix::setup {
enum class Mode { Install, Remove, Check, Upgrade, Rollback, RemovalWorker };
struct SetupOptions {
    Mode mode = Mode::Install;
    Language language = Language::English;
    std::uint64_t parentPid = 0, parentCreation = 0;
    std::wstring removalToken;
};
inline bool ParsePositive(const wchar_t* text, std::uint64_t& number) noexcept {
    if (!text || !*text) return false;
    number = 0;
    for (const wchar_t* current = text; *current; ++current) {
        if (*current < L'0' || *current > L'9' ||
            number > (std::numeric_limits<std::uint64_t>::max() - (*current - L'0')) / 10) return false;
        number = number * 10 + static_cast<unsigned>(*current - L'0');
    }
    return number != 0;
}
// No I/O or side effects. Language never changes the requested operation or
// relaxes the removal worker's identity/token checks in setup.cpp.
inline bool ParseSetupOptions(int count, const wchar_t* const* arguments, Language fallback, SetupOptions& output) {
    if (count < 1 || !arguments || !arguments[0]) return false;
    SetupOptions result;
    result.language = fallback;
    bool modeSeen = false, languageSeen = false;
    for (int i = 1; i < count; ++i) {
        const auto* value = arguments[i];
        if (!value) return false;
        if (wcscmp(value, L"--lang") == 0) {
            if (languageSeen || ++i >= count || !ParseLanguage(arguments[i], result.language)) return false;
            languageSeen = true;
            continue;
        }
        if (modeSeen) return false;
        modeSeen = true;
        if (wcscmp(value, L"--uninstall") == 0) result.mode = Mode::Remove;
        else if (wcscmp(value, L"--check") == 0) result.mode = Mode::Check;
        else if (wcscmp(value, L"--upgrade") == 0) result.mode = Mode::Upgrade;
        else if (wcscmp(value, L"--rollback") == 0) result.mode = Mode::Rollback;
        else if (wcscmp(value, L"--remove-after-parent") == 0) {
            if (count - i <= 3 || !ParsePositive(arguments[i + 1], result.parentPid) ||
                !ParsePositive(arguments[i + 2], result.parentCreation) || !arguments[i + 3]) return false;
            result.mode = Mode::RemovalWorker;
            result.removalToken = arguments[i + 3];
            i += 3;
        } else return false;
    }
    output = result;
    return true;
}
} // namespace lakefix::setup
