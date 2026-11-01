#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "setup_engine.h"
#include "installer_payload.h"
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>

namespace {
namespace setup = lakefix::setup;
constexpr wchar_t kTitle[] = L"Lake Controller - Instalar modo janela";
constexpr int kLocate = 1003;

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

std::wstring BrowseApplication(HWND owner) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    BROWSEINFOW options{};
    options.hwndOwner = owner;
    options.lpszTitle = L"Selecione a pasta do Lake Controller 5.8 Build 007 original";
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

bool ParseUnsigned(const wchar_t* text, unsigned long long& number) {
    if (!text || !*text) return false;
    number = 0;
    for (const wchar_t* current = text; *current; ++current) {
        if (*current < L'0' || *current > L'9' ||
            number > (std::numeric_limits<unsigned long long>::max() - (*current - L'0')) / 10) return false;
        number = number * 10 + static_cast<unsigned>(*current - L'0');
    }
    return number != 0;
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

bool StartRemovalWorker(const setup::Locations& locations) {
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
        std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(creation) + L" " + guid;
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
                  const setup::Locations& locations) {
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
    MessageBoxW(nullptr, result.message.c_str(), kTitle, MB_OK | (result.ok ? MB_ICONINFORMATION : MB_ICONERROR));
    // This owned helper is left in the user's temporary directory for normal
    // OS cleanup. Never invoke a shell or a recursive delete to remove itself.
    return result.ok ? 0 : static_cast<int>(result.error ? result.error : ERROR_INSTALL_FAILURE);
}

enum class Mode { Install, Remove, Check };
struct Dialog {
    Mode mode = Mode::Install;
    setup::Locations locations;
    std::wstring app;
    HWND status = nullptr;
    HWND action = nullptr;
    HWND cancel = nullptr;
    HWND locate = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    bool done = false;
    int exitCode = ERROR_CANCELLED;
};

void UpdateDescription(Dialog& state) {
    std::wstring text;
    if (state.mode == Mode::Remove) {
        text = L"Remover o modo janela deste usuario?\r\n\r\nO Lake original e seus presets nao serao removidos.\r\n"
               L"Configuracao INI editada e arquivos desconhecidos serao preservados.\r\n\r\n" + state.locations.installDirectory;
    } else if (state.app.empty()) {
        text = L"Lake Controller 5.8 Build 007 compativel nao encontrado.\r\n\r\n"
               L"Instale o programa original ou clique em Localizar Lake.\r\n"
               L"Nenhum arquivo do fabricante sera substituido.";
    } else {
        text = L"Lake compativel encontrado:\r\n" + state.app +
            L"\r\n\r\nInstalacao para este usuario, com atalhos na area de trabalho e no menu Iniciar.\r\n"
            L"Sem comandos, configuracao manual ou alteracao dos arquivos originais.\r\n\r\n"
            L"Versao 0.6 experimental: teste primeiro OFFLINE e fora de producao.";
    }
    if (state.mode == Mode::Check) text += L"\r\n\r\nVerificacao somente-leitura: nada sera instalado.";
    SetWindowTextW(state.status, text.c_str());
    EnableWindow(state.action, state.mode != Mode::Install || !state.app.empty());
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
        state->status = control(L"STATIC", L"", SS_LEFT, 20, 18, 520, 192, 0);
        if (state->mode == Mode::Install)
            state->locate = control(L"BUTTON", L"Localizar Lake...", WS_TABSTOP | BS_PUSHBUTTON, 20, 225, 145, 32, kLocate);
        state->action = control(L"BUTTON", state->mode == Mode::Remove ? L"Remover" :
            state->mode == Mode::Check ? L"Fechar" : L"Instalar", WS_TABSTOP | BS_DEFPUSHBUTTON, 310, 225, 110, 32, IDOK);
        state->cancel = control(L"BUTTON", L"Cancelar", WS_TABSTOP | BS_PUSHBUTTON, 430, 225, 110, 32, IDCANCEL);
        UpdateDescription(*state);
        SetFocus(state->action);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDCANCEL) { DestroyWindow(window); return 0; }
        if (LOWORD(wparam) == kLocate && state->mode == Mode::Install && !state->done) {
            const auto selected = BrowseApplication(window);
            if (!selected.empty()) {
                const auto checked = setup::ValidateApplication(selected, setup::LakeBuild());
                if (checked.ok) state->app = selected;
                else MessageBoxW(window, checked.message.c_str(), kTitle, MB_OK | MB_ICONERROR);
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
            EnableWindow(state->action, FALSE);
            EnableWindow(state->cancel, FALSE);
            if (state->locate) EnableWindow(state->locate, FALSE);
            SetWindowTextW(state->status, L"Aguarde... Os arquivos originais do Lake nao serao alterados.");
            UpdateWindow(window);
            setup::Result result;
            if (state->mode == Mode::Remove && SamePath(ModulePath(), Join(state->locations.installDirectory, setup::kManagerName))) {
                if (StartRemovalWorker(state->locations)) { state->exitCode = 0; DestroyWindow(window); return 0; }
                result.message = L"Nao foi possivel preparar a remocao. Nenhum arquivo foi removido.";
                result.error = GetLastError();
            } else if (state->mode == Mode::Remove) {
                result = setup::Uninstall(state->locations, setup::kPayloadFiles, ARRAYSIZE(setup::kPayloadFiles));
            } else {
                result = setup::Install(state->locations, state->app, setup::kPayloadFiles,
                                        ARRAYSIZE(setup::kPayloadFiles), ModulePath(), setup::LakeBuild());
            }
            state->done = result.ok;
            state->exitCode = result.ok ? 0 : static_cast<int>(result.error ? result.error : ERROR_INSTALL_FAILURE);
            SetWindowTextW(state->status, result.message.c_str());
            SetWindowTextW(state->action, result.ok ? L"Concluir" : state->mode == Mode::Remove ? L"Remover" : L"Instalar");
            SetWindowTextW(state->cancel, L"Fechar");
            EnableWindow(state->action, TRUE);
            EnableWindow(state->cancel, TRUE);
            if (state->locate && !result.ok) EnableWindow(state->locate, TRUE);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window); return 0;
    case WM_DPICHANGED: {
        state->dpi = HIWORD(wparam);
        const HFONT old = state->font;
        state->font = CreateFontW(-MulDiv(10, static_cast<int>(state->dpi), 72), 0, 0, 0, FW_NORMAL,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        const auto position = [&](HWND child, int x, int y, int width, int height) {
            if (!child) return;
            SetWindowPos(child, nullptr, scale(x), scale(y), scale(width), scale(height), SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        };
        position(state->status, 20, 18, 520, 192);
        position(state->locate, 20, 225, 145, 32);
        position(state->action, 310, 225, 110, 32);
        position(state->cancel, 430, 225, 110, 32);
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
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    InitializeDpi();
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return ERROR_INVALID_PARAMETER;
    Dialog state;
    state.locations = setup::DefaultLocations();
    if (state.locations.installDirectory.empty()) { LocalFree(arguments); return ERROR_PATH_NOT_FOUND; }
    if (count == 5 && wcscmp(arguments[1], L"--remove-after-parent") == 0) {
        unsigned long long pid = 0, creation = 0;
        const bool valid = ParseUnsigned(arguments[2], pid) && ParseUnsigned(arguments[3], creation);
        const std::wstring token = arguments[4];
        LocalFree(arguments);
        return valid ? FinishRemoval(pid, creation, token.c_str(), state.locations) : ERROR_INVALID_PARAMETER;
    }
    if (count == 2 && wcscmp(arguments[1], L"--uninstall") == 0) state.mode = Mode::Remove;
    else if (count == 2 && wcscmp(arguments[1], L"--check") == 0) state.mode = Mode::Check;
    else if (count != 1) { LocalFree(arguments); return ERROR_INVALID_PARAMETER; }
    LocalFree(arguments);
    if (state.mode != Mode::Remove) state.app = FindApplication();
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
    RECT bounds{0, 0, MulDiv(560, static_cast<int>(dpi), 96), MulDiv(280, static_cast<int>(dpi), 96)};
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustFrame(bounds, style, dpi);
    HWND window = CreateWindowExW(0, type.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT,
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
