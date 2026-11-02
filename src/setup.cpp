#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "setup_engine.h"
#include "installer_payload.h"
#include "product_version.h"
#include "setup_options.h"
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>
#include <utility>

namespace {
namespace setup = lakefix::setup;
constexpr int kLocate = 1003;
constexpr int kLanguage = 1004;
using Mode = setup::Mode;
using Language = setup::Language;
using TextId = setup::TextId;
using setup::Text;

template<class Procedure> Procedure User32Procedure(const char* name) {
    const FARPROC address = GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
    Procedure result = nullptr;
    static_assert(sizeof(result) == sizeof(address), "Windows procedure pointer representation");
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

UINT SystemDpi() {
    using Query = UINT(WINAPI*)();
    const auto query = User32Procedure<Query>("GetDpiForSystem");
    if (query) return query();
    HDC dc = GetDC(nullptr);
    const int value = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return value > 0 ? static_cast<UINT>(value) : 96;
}

UINT WindowDpi(HWND window) {
    using Query = UINT(WINAPI*)(HWND);
    const auto query = User32Procedure<Query>("GetDpiForWindow");
    return query ? query(window) : SystemDpi();
}

void InitializeDpi() {
    using SetContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set = User32Procedure<SetContext>("SetProcessDpiAwarenessContext");
    if (!set || !set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware();
}

void AdjustFrame(RECT& rectangle, DWORD style, UINT dpi) {
    using Adjust = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    const auto adjust = User32Procedure<Adjust>("AdjustWindowRectExForDpi");
    if (adjust) adjust(&rectangle, style, FALSE, 0, dpi);
    else AdjustWindowRectEx(&rectangle, style, FALSE, 0);
}

std::wstring ModulePath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return size && size < buffer.size() ? std::wstring(buffer.data(), size) : std::wstring();
}

std::wstring Join(const std::wstring& directory, const wchar_t* name) {
    return directory + L"\\" + name;
}

bool SamePath(const std::wstring& first, const std::wstring& second) {
    return !first.empty() && !second.empty() && _wcsicmp(first.c_str(), second.c_str()) == 0;
}

std::wstring FindApplication() {
    for (const KNOWNFOLDERID* id : {&FOLDERID_ProgramFilesX86, &FOLDERID_ProgramFiles}) {
        PWSTR folder = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, nullptr, &folder))) {
            const std::wstring candidate = std::wstring(folder) + L"\\Lake\\Lake Controller v5.8";
            CoTaskMemFree(folder);
            if (setup::ValidateApplication(candidate, setup::LakeBuild()).ok) return candidate;
        }
    }
    return {};
}

std::wstring BrowseApplication(HWND owner, Language language) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    BROWSEINFOW options{};
    options.hwndOwner = owner;
    options.lpszTitle = Text(TextId::BrowseTitle, language);
    options.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
    PIDLIST_ABSOLUTE selected = SHBrowseForFolderW(&options);
    wchar_t path[MAX_PATH]{};
    const bool found = selected && SHGetPathFromIDListW(selected, path);
    CoTaskMemFree(selected);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return found ? path : L"";
}

unsigned long long CreationTime(HANDLE process) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return 0;
    return (static_cast<unsigned long long>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}

struct RemovalEvents {
    HANDLE ready = nullptr;
    HANDLE proceed = nullptr;
    ~RemovalEvents() {
        if (ready) CloseHandle(ready);
        if (proceed) CloseHandle(proceed);
    }
};

std::wstring RemovalEvent(const wchar_t* kind, const wchar_t* token) {
    return std::wstring(L"Local\\LakeWindowedFix.Remove") + kind + L"." + token;
}

bool ValidRemovalToken(const wchar_t* token) {
    if (!token || wcslen(token) != 38 || token[0] != L'{' || token[37] != L'}') return false;
    GUID parsed{};
    wchar_t canonical[40]{};
    return SUCCEEDED(CLSIDFromString(token, &parsed)) &&
        StringFromGUID2(parsed, canonical, ARRAYSIZE(canonical)) && _wcsicmp(token, canonical) == 0;
}

bool StartRemovalWorker(const setup::Locations& locations, Language language) {
    const std::wstring self = ModulePath();
    if (!SamePath(self, Join(locations.installDirectory, setup::kManagerName))) return false;
    wchar_t temporary[MAX_PATH]{};
    GUID identifier{};
    wchar_t guid[40]{};
    const DWORD size = GetTempPathW(ARRAYSIZE(temporary), temporary);
    if (!size || size >= ARRAYSIZE(temporary) || FAILED(CoCreateGuid(&identifier)) ||
        !StringFromGUID2(identifier, guid, ARRAYSIZE(guid))) return false;
    const std::wstring directory = std::wstring(temporary) + L"LakeWindowedFix-Uninstall-" + guid;
    if (!CreateDirectoryW(directory.c_str(), nullptr)) return false;
    const std::wstring worker = Join(directory, setup::kManagerName);
    if (!CopyFileW(self.c_str(), worker.c_str(), TRUE)) { RemoveDirectoryW(directory.c_str()); return false; }
    const std::wstring digest = setup::FileSha256(self);
    if (digest.empty() || setup::FileSha256(worker) != digest) {
        DeleteFileW(worker.c_str()); RemoveDirectoryW(directory.c_str()); return false;
    }
    const auto creation = CreationTime(GetCurrentProcess());
    if (!creation) { DeleteFileW(worker.c_str()); RemoveDirectoryW(directory.c_str()); return false; }
    RemovalEvents events;
    SetLastError(ERROR_SUCCESS);
    events.ready = CreateEventW(nullptr, TRUE, FALSE, RemovalEvent(L"Ready", guid).c_str());
    const DWORD readyError = GetLastError();
    SetLastError(ERROR_SUCCESS);
    events.proceed = CreateEventW(nullptr, TRUE, FALSE, RemovalEvent(L"Proceed", guid).c_str());
    const DWORD proceedError = GetLastError();
    if (!events.ready || !events.proceed || readyError == ERROR_ALREADY_EXISTS || proceedError == ERROR_ALREADY_EXISTS) {
        DeleteFileW(worker.c_str()); RemoveDirectoryW(directory.c_str()); return false;
    }
    std::wstring command = L"\"" + worker + L"\" --remove-after-parent " +
        std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(creation) + L" " + guid +
        L" --lang " + setup::LanguageArgument(language);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(worker.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                                       nullptr, directory.c_str(), &startup, &process);
    if (!created) { DeleteFileW(worker.c_str()); RemoveDirectoryW(directory.c_str()); return false; }
    CloseHandle(process.hThread);
    // Keep the parent alive until the child has opened/verified its handle.
    // A second acknowledgement authorizes removal; a failed/expired handshake
    // never turns a later user-initiated window close into an uninstall.
    HANDLE waiting[] = {events.ready, process.hProcess};
    const DWORD ready = WaitForMultipleObjects(ARRAYSIZE(waiting), waiting, FALSE, 30000);
    const bool approved = ready == WAIT_OBJECT_0 && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT &&
        SetEvent(events.proceed);
    CloseHandle(process.hProcess);
    return approved;
}

int FinishRemoval(unsigned long long pid, unsigned long long creation, const wchar_t* token,
                  const setup::Locations& locations, Language language) {
    if (pid > MAXDWORD || pid == GetCurrentProcessId() || !ValidRemovalToken(token)) return ERROR_INVALID_PARAMETER;
    HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!parent) return ERROR_INVALID_PARAMETER;
    std::vector<wchar_t> image(32768);
    DWORD size = static_cast<DWORD>(image.size());
    const std::wstring manager = Join(locations.installDirectory, setup::kManagerName);
    const bool verified = QueryFullProcessImageNameW(parent, 0, image.data(), &size) &&
        SamePath(std::wstring(image.data(), size), manager) && CreationTime(parent) == creation &&
        !setup::FileSha256(manager).empty() && setup::FileSha256(ModulePath()) == setup::FileSha256(manager);
    RemovalEvents events;
    if (verified) {
        events.ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, RemovalEvent(L"Ready", token).c_str());
        events.proceed = OpenEventW(SYNCHRONIZE, FALSE, RemovalEvent(L"Proceed", token).c_str());
    }
    const bool approved = verified && events.ready && events.proceed && SetEvent(events.ready) &&
        WaitForSingleObject(events.proceed, 60000) == WAIT_OBJECT_0;
    const DWORD waited = approved ? WaitForSingleObject(parent, 60000) : WAIT_FAILED;
    CloseHandle(parent);
    if (!approved || waited != WAIT_OBJECT_0) return ERROR_INVALID_PARAMETER;
    const setup::Result result = setup::Uninstall(locations, setup::kPayloadFiles, ARRAYSIZE(setup::kPayloadFiles));
    MessageBoxW(nullptr, result.Message(language).c_str(), Text(TextId::Title, language), MB_OK | (result.ok ? MB_ICONINFORMATION : MB_ICONERROR));
    // This owned helper is left in the user's temporary directory for normal
    // OS cleanup. Never invoke a shell or a recursive delete to remove itself.
    return result.ok ? 0 : static_cast<int>(result.error ? result.error : ERROR_INSTALL_FAILURE);
}

struct Dialog {
    Mode mode = Mode::Install;
    Language language = Language::English;
    setup::Locations locations;
    std::wstring app;
    HWND languageLabel = nullptr;
    HWND languageSelect = nullptr;
    HWND status = nullptr;
    HWND notice = nullptr;
    HWND action = nullptr;
    HWND cancel = nullptr;
    HWND locate = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    bool done = false;
    bool busy = false;
    bool hasResult = false;
    bool verificationOnly = false;
    setup::Result result;
    int exitCode = ERROR_CANCELLED;
};

void UpdateDescription(Dialog& state) {
    std::wstring text;
    if (state.hasResult) {
        text = state.result.Message(state.language);
        if (!state.result.ok && state.result.error)
            text += std::wstring(Text(TextId::WindowsError, state.language)) + std::to_wstring(state.result.error);
    } else if (state.mode == Mode::Remove) {
        text = Text(TextId::RemoveDescription, state.language) + state.locations.installDirectory;
    } else if (state.mode == Mode::Rollback) {
        text = Text(TextId::RollbackDescription, state.language) + state.locations.installDirectory;
    } else if (state.mode == Mode::Upgrade) {
        text = Text(TextId::UpgradePrefix, state.language) + std::wstring(setup::kVersion) +
            Text(TextId::UpgradeDescription, state.language) + state.locations.installDirectory;
    } else if (state.app.empty()) {
        text = Text(TextId::NoApplication, state.language);
    } else {
        text = Text(TextId::ApplicationFound, state.language) + state.app +
            Text(TextId::InstallDescription, state.language) + setup::kVersion + Text(TextId::Experimental, state.language);
    }
    if (state.mode == Mode::Check && !state.hasResult) text += Text(TextId::ReadOnly, state.language);
    SetWindowTextW(state.status, text.c_str());
    EnableWindow(state.action, state.mode != Mode::Install || !state.app.empty());
}

void UpdateLanguage(HWND window, Dialog& state) {
    SetWindowTextW(window, Text(TextId::Title, state.language));
    SetWindowTextW(state.languageLabel, Text(TextId::LanguageLabel, state.language));
    SetWindowTextW(state.notice, Text(TextId::RightsNotice, state.language));
    if (state.locate) SetWindowTextW(state.locate, Text(TextId::Locate, state.language));
    const auto action = state.done ? TextId::Finish : state.mode == Mode::Remove ? TextId::Remove :
        state.mode == Mode::Check ? TextId::Close : state.mode == Mode::Upgrade ? TextId::Upgrade :
        state.mode == Mode::Rollback ? TextId::Rollback : TextId::Install;
    SetWindowTextW(state.action, Text(action, state.language));
    SetWindowTextW(state.cancel, Text(state.hasResult ? TextId::Close : TextId::Cancel, state.language));
    UpdateDescription(state);
}

void LayoutDialog(HWND window, const Dialog& state) {
    RECT area{};
    if (!GetClientRect(window, &area)) return;
    const int width = MulDiv(area.right, 96, static_cast<int>(state.dpi));
    const int height = MulDiv(area.bottom, 96, static_cast<int>(state.dpi));
    const int buttons = (std::max)(140, height - 52);
    const int notice = (std::max)(90, buttons - 112);
    const auto position = [&](HWND child, int x, int y, int w, int h) {
        if (!child) return;
        const auto scale = [&](int value) { return MulDiv(value, static_cast<int>(state.dpi), 96); };
        SetWindowPos(child, nullptr, scale(x), scale(y), scale(w), scale(h), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    };
    position(state.languageLabel, 20, 14, 135, 24);
    position(state.languageSelect, 160, 9, (std::max)(100, (std::min)(240, width - 180)), 200);
    position(state.status, 20, 48, (std::max)(100, width - 40), (std::max)(20, notice - 58));
    position(state.notice, 20, notice, (std::max)(100, width - 40), 100);
    position(state.locate, 20, buttons, 145, 32);
    position(state.action, (std::max)(20, width - 310), buttons, 170, 32);
    position(state.cancel, (std::max)(20, width - 130), buttons, 110, 32);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<Dialog*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    const auto scale = [state](int value) { return MulDiv(value, static_cast<int>(state->dpi), 96); };
    switch (message) {
    case WM_CREATE: {
        state->dpi = WindowDpi(window);
        state->font = CreateFontW(-MulDiv(10, static_cast<int>(state->dpi), 72), 0, 0, 0, FW_NORMAL,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        const HINSTANCE instance = reinterpret_cast<CREATESTRUCTW*>(lparam)->hInstance;
        const auto control = [&](const wchar_t* kind, const wchar_t* label, DWORD style,
                                 int x, int y, int width, int height, int id) {
            HWND child = CreateWindowExW(0, kind, label, WS_CHILD | WS_VISIBLE | style,
                scale(x), scale(y), scale(width), scale(height), window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
            return child;
        };
        state->languageLabel = control(L"STATIC", L"", SS_LEFT, 20, 14, 135, 24, 0);
        state->languageSelect = control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 160, 9, 240, 200, kLanguage);
        SendMessageW(state->languageSelect, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Português (Brasil)"));
        SendMessageW(state->languageSelect, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
        SendMessageW(state->languageSelect, CB_SETCURSEL, state->language == Language::Portuguese ? 0 : 1, 0);
        state->status = control(L"EDIT", L"", ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 20, 48, 660, 218, 0);
        state->notice = control(L"STATIC", L"", SS_LEFT, 20, 274, 660, 100, 0);
        if (state->mode == Mode::Install)
            state->locate = control(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 20, 398, 145, 32, kLocate);
        state->action = control(L"BUTTON", L"", WS_TABSTOP | BS_DEFPUSHBUTTON, 390, 398, 170, 32, IDOK);
        state->cancel = control(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 570, 398, 110, 32, IDCANCEL);
        if (!state->languageLabel || !state->languageSelect || !state->status || !state->notice ||
            !state->action || !state->cancel || (state->mode == Mode::Install && !state->locate)) return -1;
        UpdateLanguage(window, *state);
        LayoutDialog(window, *state);
        if (!state->verificationOnly) SetFocus(state->action);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDCANCEL && !state->busy) { DestroyWindow(window); return 0; }
        if (LOWORD(wparam) == kLanguage && HIWORD(wparam) == CBN_SELCHANGE && !state->busy) {
            const auto selected = SendMessageW(state->languageSelect, CB_GETCURSEL, 0, 0);
            if (selected == 0 || selected == 1) {
                state->language = selected == 0 ? Language::Portuguese : Language::English;
                UpdateLanguage(window, *state);
            }
            return 0;
        }
        if (state->busy || state->verificationOnly) return 0;
        if (LOWORD(wparam) == kLocate && state->mode == Mode::Install && !state->done) {
            const auto selected = BrowseApplication(window, state->language);
            if (!selected.empty()) {
                const auto checked = setup::ValidateApplication(selected, setup::LakeBuild());
                if (checked.ok) state->app = selected;
                else MessageBoxW(window, checked.Message(state->language).c_str(), Text(TextId::Title, state->language), MB_OK | MB_ICONERROR);
                state->hasResult = false;
                UpdateDescription(*state);
            }
            return 0;
        }
        if (LOWORD(wparam) == IDOK) {
            if (state->done || state->mode == Mode::Check) {
                if (state->mode == Mode::Check) state->exitCode = state->app.empty() ? ERROR_NOT_FOUND : 0;
                DestroyWindow(window); return 0;
            }
            if (state->mode == Mode::Install && state->app.empty()) return 0;
            state->busy = true;
            EnableWindow(state->action, FALSE);
            EnableWindow(state->cancel, FALSE);
            if (state->locate) EnableWindow(state->locate, FALSE);
            EnableWindow(state->languageSelect, FALSE);
            SetWindowTextW(state->status, Text(TextId::Busy, state->language));
            UpdateWindow(window);
            setup::Result result;
            if (state->mode == Mode::Remove && SamePath(ModulePath(), Join(state->locations.installDirectory, setup::kManagerName))) {
                if (StartRemovalWorker(state->locations, state->language)) { state->exitCode = 0; DestroyWindow(window); return 0; }
                result.message = Text(TextId::RemovalWorkerFailed, Language::Portuguese);
                result.messageEnglish = Text(TextId::RemovalWorkerFailed, Language::English);
                result.error = GetLastError();
            } else if (state->mode == Mode::Remove) {
                result = setup::Uninstall(state->locations, setup::kPayloadFiles, ARRAYSIZE(setup::kPayloadFiles));
            } else if (state->mode == Mode::Rollback) {
                result = setup::Rollback(state->locations);
            } else if (state->mode == Mode::Upgrade) {
                result = setup::Upgrade(state->locations, state->app, setup::kPayloadFiles,
                                        ARRAYSIZE(setup::kPayloadFiles), ModulePath(), setup::LakeBuild());
            } else {
                result = setup::Install(state->locations, state->app, setup::kPayloadFiles,
                                        ARRAYSIZE(setup::kPayloadFiles), ModulePath(), setup::LakeBuild());
            }
            state->done = result.ok;
            state->exitCode = result.ok ? 0 : static_cast<int>(result.error ? result.error : ERROR_INSTALL_FAILURE);
            state->result = std::move(result);
            state->hasResult = true;
            state->busy = false;
            UpdateLanguage(window, *state);
            EnableWindow(state->action, TRUE);
            EnableWindow(state->cancel, TRUE);
            EnableWindow(state->languageSelect, TRUE);
            if (state->locate && !state->done) EnableWindow(state->locate, TRUE);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (state->busy) return 0;
        DestroyWindow(window); return 0;
    case WM_SIZE:
        LayoutDialog(window, *state); return 0;
    case WM_DPICHANGED: {
        state->dpi = HIWORD(wparam);
        const HFONT old = state->font;
        state->font = CreateFontW(-MulDiv(10, static_cast<int>(state->dpi), 72), 0, 0, 0, FW_NORMAL,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        LayoutDialog(window, *state);
        if (old) DeleteObject(old);
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        if (state->font) DeleteObject(state->font);
        PostQuitMessage(state->exitCode); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int VerifyLocalization(HINSTANCE instance) {
    // Only hidden OWN windows and in-memory fixtures. Never use DefaultLocations,
    // application discovery, filesystem/registry transactions or Lake execution.
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok) { ++checks; if (!ok) ++failures; };
    check(setup::LanguageFromId(MAKELANGID(LANG_PORTUGUESE, SUBLANG_PORTUGUESE_BRAZILIAN)) == Language::Portuguese);
    check(setup::LanguageFromId(MAKELANGID(LANG_PORTUGUESE, SUBLANG_PORTUGUESE)) == Language::Portuguese);
    check(setup::LanguageFromId(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)) == Language::English);
    check(setup::LanguageFromId(MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN)) == Language::English);
    for (std::size_t i = 1; i < setup::kSetupTextCount; ++i) {
        check(Text(static_cast<TextId>(i), Language::Portuguese)[0] != 0);
        check(Text(static_cast<TextId>(i), Language::English)[0] != 0);
    }
    check(std::wstring(Text(TextId::RightsNotice, Language::English)).find(L"NOT included") != std::wstring::npos);
    check(std::wstring(Text(TextId::RightsNotice, Language::Portuguese)).find(L"NÃO estão incluídos") != std::wstring::npos);
    const auto payload = setup::VerifyPayload(setup::kPayloadFiles, ARRAYSIZE(setup::kPayloadFiles));
    check(payload.ok && payload.Message(Language::Portuguese) == Text(TextId::PayloadVerified, Language::Portuguese) &&
          payload.Message(Language::English) == Text(TextId::PayloadVerified, Language::English));
    const auto rejected = setup::VerifyPayload(nullptr, 0);
    check(!rejected.ok && rejected.error == ERROR_INVALID_DATA &&
          rejected.Message(Language::English) == Text(TextId::InvalidEmbeddedPayload, Language::English));
    setup::SetupOptions options;
    const wchar_t* defaults[] = {L"setup.exe"};
    check(setup::ParseSetupOptions(1, defaults, Language::Portuguese, options) && options.mode == Mode::Install && options.language == Language::Portuguese);
    const wchar_t* languageOnly[] = {L"setup.exe", L"--lang", L"en"};
    check(setup::ParseSetupOptions(3, languageOnly, Language::Portuguese, options) && options.mode == Mode::Install && options.language == Language::English);
    const wchar_t* english[] = {L"setup.exe", L"--upgrade", L"--lang", L"en"};
    check(setup::ParseSetupOptions(4, english, Language::Portuguese, options) && options.mode == Mode::Upgrade && options.language == Language::English);
    const wchar_t* portuguese[] = {L"setup.exe", L"--lang", L"pt-BR", L"--uninstall"};
    check(setup::ParseSetupOptions(4, portuguese, Language::English, options) && options.mode == Mode::Remove && options.language == Language::Portuguese);
    const wchar_t* worker[] = {L"setup.exe", L"--remove-after-parent", L"123", L"456", L"{11111111-1111-4111-8111-111111111111}", L"--lang", L"en"};
    check(setup::ParseSetupOptions(7, worker, Language::Portuguese, options) && options.mode == Mode::RemovalWorker &&
          options.parentPid == 123 && options.parentCreation == 456 && options.language == Language::English && ValidRemovalToken(options.removalToken.c_str()));
    const wchar_t* invalid[][5] = {
        {L"setup.exe", L"--lang", nullptr}, {L"setup.exe", L"--lang", L"fr"},
        {L"setup.exe", L"--lang", L"en", L"--lang", L"pt"},
        {L"setup.exe", L"--upgrade", L"--uninstall"}, {L"setup.exe", L"--unknown"},
        {L"setup.exe", L"--remove-after-parent", L"0", L"12", L"token"},
        {L"setup.exe", L"--remove-after-parent", L"18446744073709551616", L"12", L"token"}
    };
    const int lengths[] = {2, 3, 5, 3, 2, 5, 5};
    for (std::size_t i = 0; i < ARRAYSIZE(invalid); ++i)
        check(!setup::ParseSetupOptions(lengths[i], invalid[i], Language::English, options));
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type); type.hInstance = instance; type.lpfnWndProc = WindowProcedure;
    type.lpszClassName = L"LakeWindowedFix.Setup.LocalizationFixture";
    const bool registered = RegisterClassExW(&type) != 0;
    check(registered);
    const auto windowText = [](HWND window) {
        const int size = GetWindowTextLengthW(window);
        std::vector<wchar_t> value(static_cast<std::size_t>(size) + 1);
        GetWindowTextW(window, value.data(), static_cast<int>(value.size()));
        return std::wstring(value.data());
    };
    if (registered) {
        for (Mode mode : {Mode::Install, Mode::Remove, Mode::Check, Mode::Upgrade, Mode::Rollback}) {
            Dialog state;
            state.verificationOnly = true;
            state.mode = mode;
            state.language = Language::Portuguese;
            state.locations.installDirectory = L"C:\\OwnLocalizationFixture";
            state.app = L"C:\\OwnApplicationFixture";
            HWND window = CreateWindowExW(WS_EX_NOACTIVATE, type.lpszClassName, L"", WS_OVERLAPPED,
                0, 0, 700, 450, nullptr, nullptr, instance, &state);
            check(window != nullptr);
            if (!window) continue;
            for (Language language : {Language::Portuguese, Language::English, Language::Portuguese}) {
                const auto selected = language == Language::Portuguese ? 0 : 1;
                SendMessageW(state.languageSelect, CB_SETCURSEL, selected, 0);
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(kLanguage, CBN_SELCHANGE), reinterpret_cast<LPARAM>(state.languageSelect));
                check(state.language == language && !IsWindowVisible(window));
                check(windowText(window) == Text(TextId::Title, language));
                check(windowText(state.notice) == Text(TextId::RightsNotice, language));
                const auto action = mode == Mode::Install ? TextId::Install : mode == Mode::Remove ? TextId::Remove :
                    mode == Mode::Check ? TextId::Close : mode == Mode::Upgrade ? TextId::Upgrade : TextId::Rollback;
                check(windowText(state.action) == Text(action, language));
                check(windowText(state.status).find(state.app) != std::wstring::npos || mode == Mode::Remove || mode == Mode::Upgrade || mode == Mode::Rollback);
                state.hasResult = true;
                state.result.ok = false;
                state.result.error = ERROR_CRC;
                state.result.message = Text(TextId::ModifiedRepairRefused, Language::Portuguese);
                state.result.messageEnglish = Text(TextId::ModifiedRepairRefused, Language::English);
                UpdateLanguage(window, state);
                check(windowText(state.status).find(state.result.Message(language)) == 0);
                check(windowText(state.status).find(L"23") != std::wstring::npos);
                state.hasResult = false;
                UpdateLanguage(window, state);
            }
            state.hasResult = true;
            state.done = true;
            state.result = payload;
            for (Language language : {Language::English, Language::Portuguese}) {
                state.language = language;
                UpdateLanguage(window, state);
                check(windowText(state.action) == Text(TextId::Finish, language));
                check(windowText(state.status) == payload.Message(language));
            }
            DestroyWindow(window);
        }
        UnregisterClassW(type.lpszClassName, instance);
    }
    char output[256]{};
    const int size = snprintf(output, sizeof(output), "Installer localization: %u checks, %u failures; hidden own windows only; no installation or Lake launch.\r\n", checks, failures);
    DWORD written = 0;
    if (size > 0) WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, static_cast<DWORD>(size), &written, nullptr);
    return failures ? ERROR_INVALID_DATA : 0;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return ERROR_INVALID_PARAMETER;
    if (count == 2 && wcscmp(arguments[1], L"--verify-localization") == 0) {
        LocalFree(arguments);
        return VerifyLocalization(instance);
    }
    // Build gate: no DPI/COM/known folders/process inventory/GUI/installation.
    if (count == 2 && wcscmp(arguments[1], L"--verify-payload") == 0) {
        LocalFree(arguments);
        const setup::Result result = setup::VerifyPayload(setup::kPayloadFiles, ARRAYSIZE(setup::kPayloadFiles));
        // Retain this setup link's exact identity in runtime data, independently
        // of the embedded runtime payload's different BuildId. No GUI or install.
        constexpr char buildId[] = LAKE_FIX_BUILD_ID;
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buildId, sizeof(buildId) - 1, &written, nullptr);
        return result.ok ? 0 : static_cast<int>(result.error ? result.error : ERROR_INVALID_DATA);
    }
    setup::SetupOptions options;
    const bool parsed = setup::ParseSetupOptions(count, arguments, setup::LanguageFromId(GetUserDefaultUILanguage()), options);
    LocalFree(arguments);
    if (!parsed) return ERROR_INVALID_PARAMETER;
    InitializeDpi();
    Dialog state;
    state.mode = options.mode;
    state.language = options.language;
    state.locations = setup::DefaultLocations();
    if (state.locations.installDirectory.empty()) return ERROR_PATH_NOT_FOUND;
    if (options.mode == Mode::RemovalWorker) {
        return FinishRemoval(options.parentPid, options.parentCreation, options.removalToken.c_str(), state.locations, state.language);
    }
    if (state.mode == Mode::Install || state.mode == Mode::Check) state.app = FindApplication();
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = WindowProcedure;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    type.lpszClassName = L"LakeWindowedFix.Setup.v1";
    if (!RegisterClassExW(&type)) return static_cast<int>(GetLastError());
    const UINT dpi = SystemDpi();
    RECT bounds{0, 0, MulDiv(700, static_cast<int>(dpi), 96), MulDiv(450, static_cast<int>(dpi), 96)};
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustFrame(bounds, style, dpi);
    HWND window = CreateWindowExW(0, type.lpszClassName, Text(TextId::Title, state.language), style, CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance, &state);
    if (!window) return static_cast<int>(GetLastError());
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    int received;
    while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    return received < 0 ? static_cast<int>(GetLastError()) : static_cast<int>(message.wParam);
}
