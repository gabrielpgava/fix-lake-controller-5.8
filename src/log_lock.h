#pragma once

#include <windows.h>

namespace lakefix {
inline bool AcquireDiagnosticLock(SRWLOCK& lock, bool bestEffort) noexcept {
    if (bestEffort) return TryAcquireSRWLockExclusive(&lock) != FALSE;
    AcquireSRWLockExclusive(&lock);
    return true;
}
} // namespace lakefix
