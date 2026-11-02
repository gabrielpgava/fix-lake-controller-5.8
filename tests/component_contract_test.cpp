#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../src/component_contract.h"
#include <cstdio>
#include <cstring>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    int checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* name) { ++checks; if (!ok) ++failures; printf("%s: %s\n", ok ? "PASS" : "FAIL", name); };
    // Only our own production DLL. No entry point or proprietary image runs.
    const HMODULE module = LoadLibraryW(argv[1]);
    check(module != nullptr, "own x86 production DLL loads without heavy DllMain initialization");
    if (!module) return 1;
    using Entry = DWORD (WINAPI*)(void*);
    const auto query = reinterpret_cast<Entry>(reinterpret_cast<void*>(GetProcAddress(module, lakefix::kQueryContractExport)));
    const auto initialize = reinterpret_cast<Entry>(reinterpret_cast<void*>(GetProcAddress(module, lakefix::kInitializeExport)));
    check(query && initialize, "ABI exports use undecorated SDK-compatible x86 WINAPI entry points");
    check(!GetProcAddress(module, "LakeWindowedFlushDiagnostics"), "production DLL does not expose fixture-only control exports");
    if (query) {
        lakefix::ComponentContract contract{};
        contract.size = sizeof(contract);
        check(query(&contract) == ERROR_SUCCESS && lakefix::ComponentContractMatches(contract), "compiled launcher-side build identity matches the actual production DLL");
        contract.size--;
        check(query(&contract) == ERROR_REVISION_MISMATCH, "wrong wire size is rejected before writing the contract");
        check(query(nullptr) == ERROR_REVISION_MISMATCH, "invalid output pointer is rejected without an exception");
    }
    if (initialize) check(initialize(nullptr) == ERROR_REVISION_MISMATCH, "missing startup identity cannot install production hooks or admit RVAs");
    // Initialization was refused BEFORE hooks/pinning. This test never unloads
    // an active initialized DLL or removes hooks from a session.
    FreeLibrary(module);
    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
