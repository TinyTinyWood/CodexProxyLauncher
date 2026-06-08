#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "resource.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ws2_32.lib")

namespace {

constexpr COLORREF kWindowColor = RGB(247, 248, 250);
constexpr COLORREF kTextColor = RGB(28, 32, 39);
constexpr COLORREF kBorderColor = RGB(222, 226, 232);
constexpr COLORREF kInputBorderColor = RGB(128, 139, 153);
constexpr COLORREF kAccentColor = RGB(20, 108, 221);
constexpr COLORREF kButtonNeutralColor = RGB(232, 237, 244);
constexpr int kWindowWidth = 528;
constexpr int kWindowHeight = 416;
constexpr int kMinWindowWidth = 488;
constexpr int kMinWindowHeight = 376;
constexpr UINT kDefaultDpi = 96;

constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_ACTION_DONE = WM_APP + 2;

constexpr int IDC_PROXY = 1001;
constexpr int IDC_LOG = 1002;
constexpr int IDC_START = 1003;
constexpr int IDC_TEST = 1004;

struct OperationResult {
    bool success = false;
    std::wstring message;
    bool showMessageBox = false;
    std::wstring dialogTitle = L"提示";
    std::wstring dialogMessage;
    UINT dialogIcon = MB_ICONINFORMATION;
    bool wslProxyUnreachable = false;
};

struct LauncherSettings {
    std::wstring proxyAddress = L"http://127.0.0.1:10808";
    std::wstring chromiumProxy;
    std::wstring noProxy = L"localhost,127.0.0.1,::1";
    std::wstring codexExePath;
    int startupWaitSeconds = 20;
    bool temporarilySetUserProxyEnvironment = false;
};

struct WslBackendDetection {
    bool shouldApplyProxy = false;
    std::wstring message;
};

struct ParsedUrl {
    std::wstring original;
    std::wstring scheme;
    std::wstring userInfo;
    std::wstring host;
    std::wstring pathAndQuery;
    int port = 0;
    bool valid = false;
};

struct ProcessOutput {
    bool started = false;
    bool timedOut = false;
    DWORD exitCode = 0;
    std::wstring output;
    std::wstring error;
    std::wstring errorMessage;
};

struct CaseInsensitiveLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    }
};

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return L"";
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return L"";
    }
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), needed);
    return result;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return "";
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return "";
    }
    std::string result(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), needed, nullptr, nullptr);
    return result;
}

std::wstring Trim(std::wstring value) {
    auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) { return iswspace(ch) != 0; });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) { return iswspace(ch) != 0; }).base();
    if (first >= last) {
        return L"";
    }
    return std::wstring(first, last);
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

bool StartsWithI(std::wstring_view value, std::wstring_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    return _wcsnicmp(value.data(), prefix.data(), prefix.size()) == 0;
}

bool EndsWithI(std::wstring_view value, std::wstring_view suffix) {
    if (value.size() < suffix.size()) {
        return false;
    }
    return _wcsnicmp(value.data() + value.size() - suffix.size(), suffix.data(), suffix.size()) == 0;
}

bool ContainsI(std::wstring_view value, std::wstring_view needle) {
    auto lowerValue = ToLower(std::wstring(value));
    auto lowerNeedle = ToLower(std::wstring(needle));
    return lowerValue.find(lowerNeedle) != std::wstring::npos;
}

std::wstring NormalizeKey(std::wstring key) {
    std::wstring normalized;
    for (wchar_t ch : key) {
        if (ch != L'-' && !iswspace(ch)) {
            normalized.push_back(static_cast<wchar_t>(towlower(ch)));
        }
    }
    return normalized;
}

std::vector<std::wstring> Split(std::wstring_view value, wchar_t delimiter) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(delimiter, start);
        if (end == std::wstring_view::npos) {
            end = value.size();
        }
        auto item = Trim(std::wstring(value.substr(start, end - start)));
        if (!item.empty()) {
            parts.push_back(item);
        }
        if (end == value.size()) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

std::wstring GetModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length == path.size()) {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    path.resize(length);
    return path;
}

std::wstring GetDirectoryName(std::wstring path) {
    auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

std::wstring CombinePath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) {
        return right;
    }
    if (left.back() == L'\\' || left.back() == L'/') {
        return left + right;
    }
    return left + L"\\" + right;
}

std::wstring GetBaseDirectory() {
    return GetDirectoryName(GetModulePath());
}

bool FileExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ExpandEnvironment(const std::wstring& value) {
    DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (needed == 0) {
        return value;
    }
    std::wstring expanded(needed, L'\0');
    ExpandEnvironmentStringsW(value.c_str(), expanded.data(), needed);
    if (!expanded.empty() && expanded.back() == L'\0') {
        expanded.pop_back();
    }
    return expanded;
}

std::wstring FullPath(const std::wstring& path) {
    DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return path;
    }
    std::wstring result(needed, L'\0');
    GetFullPathNameW(path.c_str(), needed, result.data(), nullptr);
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}

std::wstring FormatWindowsError(DWORD error) {
    if (error == ERROR_SUCCESS) {
        return L"未知错误。";
    }
    LPWSTR buffer = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    std::wstring message = length > 0 && buffer ? Trim(buffer) : L"Windows 错误码 " + std::to_wstring(error);
    if (buffer) {
        LocalFree(buffer);
    }
    return message;
}

UINT GetSystemDpi() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetDpiForSystemFn = UINT(WINAPI*)();
        auto getDpiForSystem = reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(user32, "GetDpiForSystem"));
        if (getDpiForSystem) {
            return getDpiForSystem();
        }
    }
    HDC dc = GetDC(nullptr);
    UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY)) : kDefaultDpi;
    if (dc) {
        ReleaseDC(nullptr, dc);
    }
    return dpi == 0 ? kDefaultDpi : dpi;
}

UINT GetWindowDpi(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
        if (getDpiForWindow) {
            UINT dpi = getDpiForWindow(hwnd);
            if (dpi != 0) {
                return dpi;
            }
        }
    }
    return GetSystemDpi();
}

int ScaleForDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), static_cast<int>(kDefaultDpi));
}

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
        auto setProcessDpiAwarenessContext =
            reinterpret_cast<SetProcessDpiAwarenessContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setProcessDpiAwarenessContext &&
            setProcessDpiAwarenessContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) {
            return;
        }
        if (setProcessDpiAwarenessContext &&
            setProcessDpiAwarenessContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-2)))) {
            return;
        }
    }
    SetProcessDPIAware();
}

std::optional<std::string> ReadAllBytes(const std::wstring& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::optional<std::wstring> ReadUtf8File(const std::wstring& path) {
    auto bytes = ReadAllBytes(path);
    if (!bytes) {
        return std::nullopt;
    }
    std::string data = *bytes;
    if (data.size() >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }
    return Utf8ToWide(data);
}

bool WriteUtf8File(const std::wstring& path, const std::wstring& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    auto utf8 = WideToUtf8(value);
    output.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return true;
}

std::wstring CurrentTimeForLog(bool includeDate) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[64]{};
    if (includeDate) {
        swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
                   time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    } else {
        swprintf_s(buffer, L"%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
    }
    return buffer;
}

std::wstring ConfigPath() {
    return CombinePath(GetBaseDirectory(), L"config.txt");
}

std::wstring LogPath() {
    return CombinePath(GetBaseDirectory(), L"log.txt");
}

void AppendFileLog(const std::wstring& message) {
    std::ofstream output(LogPath(), std::ios::binary | std::ios::app);
    if (!output) {
        return;
    }
    auto line = L"[" + CurrentTimeForLog(true) + L"] " + message + L"\r\n";
    auto utf8 = WideToUtf8(line);
    output.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

OperationResult Ok(std::wstring message) {
    return OperationResult{true, std::move(message)};
}

OperationResult Fail(std::wstring message) {
    OperationResult result;
    result.success = false;
    result.message = L"错误：" + std::move(message);
    return result;
}

OperationResult ProxyUnreachable(std::wstring message) {
    auto result = Fail(std::move(message));
    result.wslProxyUnreachable = true;
    return result;
}

OperationResult Prompt(std::wstring message) {
    OperationResult result;
    result.success = false;
    result.message = message;
    result.showMessageBox = true;
    result.dialogTitle = L"Codex 已在运行";
    result.dialogMessage = std::move(message);
    return result;
}

OperationResult WithWslProxyWarning(OperationResult result, const std::wstring& warning) {
    if (Trim(warning).empty()) {
        return result;
    }
    result.showMessageBox = true;
    result.dialogTitle = L"WSL 代理端口不通";
    result.dialogMessage = result.dialogMessage.empty()
        ? warning
        : warning + L"\r\n\r\n" + result.dialogMessage;
    result.dialogIcon = MB_ICONWARNING;
    return result;
}

std::wstring WslProxyUnreachableWarning() {
    return L"Codex被设置为使用WSL后端。\r\n"
           L"当前 WSL 无法连接到 Windows 代理端口，Codex 启动后可能无法联网或请求长时间无响应。\r\n"
           L"请确认代理地址和端口正确，并在代理软件中允许来自 WSL/局域网的连接。";
}

void SaveSettings(const LauncherSettings& settings) {
    std::wostringstream text;
    text << L"# Codex Proxy Launcher configuration\r\n"
         << L"# The GUI only edits proxy_address. Change the other values here, then restart the launcher.\r\n\r\n"
         << L"proxy_address=" << settings.proxyAddress << L"\r\n"
         << L"chromium_proxy=" << settings.chromiumProxy << L"\r\n"
         << L"no_proxy=" << settings.noProxy << L"\r\n"
         << L"codex_exe_path=" << settings.codexExePath << L"\r\n"
         << L"startup_wait_seconds=" << settings.startupWaitSeconds << L"\r\n"
         << L"temporarily_set_user_proxy_environment=" << (settings.temporarilySetUserProxyEnvironment ? L"true" : L"false") << L"\r\n";
    WriteUtf8File(ConfigPath(), text.str());
}

void ApplySetting(LauncherSettings& settings, std::wstring key, std::wstring value) {
    key = NormalizeKey(std::move(key));
    value = Trim(std::move(value));
    if (key == L"proxyaddress" || key == L"proxy_address" || key == L"proxy" || key == L"http_proxy" || key == L"all_proxy") {
        settings.proxyAddress = value;
    } else if (key == L"chromiumproxy" || key == L"chromium_proxy") {
        settings.chromiumProxy = value;
    } else if (key == L"noproxy" || key == L"no_proxy") {
        settings.noProxy = value;
    } else if (key == L"codexexepath" || key == L"codex_exe_path") {
        settings.codexExePath = value;
    } else if (key == L"startupwaitseconds" || key == L"startup_wait_seconds") {
        try {
            int seconds = std::stoi(value);
            settings.startupWaitSeconds = std::max(3, std::min(90, seconds));
        } catch (...) {
        }
    } else if (key == L"temporarilysetuserproxyenvironment" || key == L"temporarily_set_user_proxy_environment") {
        settings.temporarilySetUserProxyEnvironment = ToLower(value) == L"true";
    }
}

LauncherSettings LoadSettings() {
    LauncherSettings settings;
    auto config = ReadUtf8File(ConfigPath());
    if (!config) {
        SaveSettings(settings);
        return settings;
    }

    std::wistringstream lines(*config);
    std::wstring raw;
    while (std::getline(lines, raw)) {
        auto line = Trim(raw);
        if (line.empty() || line[0] == L'#') {
            continue;
        }
        auto separator = line.find(L'=');
        if (separator == std::wstring::npos || separator == 0) {
            continue;
        }
        ApplySetting(settings, line.substr(0, separator), line.substr(separator + 1));
    }
    return settings;
}

ParsedUrl ParseUrl(std::wstring input) {
    ParsedUrl parsed;
    parsed.original = Trim(std::move(input));
    auto schemePos = parsed.original.find(L"://");
    if (schemePos == std::wstring::npos || schemePos == 0) {
        return parsed;
    }
    parsed.scheme = parsed.original.substr(0, schemePos);
    auto rest = parsed.original.substr(schemePos + 3);
    auto pathPos = rest.find_first_of(L"/?#");
    auto authority = pathPos == std::wstring::npos ? rest : rest.substr(0, pathPos);
    parsed.pathAndQuery = pathPos == std::wstring::npos ? L"" : rest.substr(pathPos);

    auto at = authority.rfind(L'@');
    if (at != std::wstring::npos) {
        parsed.userInfo = authority.substr(0, at);
        authority = authority.substr(at + 1);
    }

    std::wstring portText;
    if (!authority.empty() && authority.front() == L'[') {
        auto close = authority.find(L']');
        if (close == std::wstring::npos) {
            return parsed;
        }
        parsed.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == L':') {
            portText = authority.substr(close + 2);
        }
    } else {
        auto colon = authority.rfind(L':');
        if (colon == std::wstring::npos) {
            parsed.host = authority;
        } else {
            parsed.host = authority.substr(0, colon);
            portText = authority.substr(colon + 1);
        }
    }

    if (parsed.host.empty() || portText.empty()) {
        return parsed;
    }
    try {
        parsed.port = std::stoi(portText);
    } catch (...) {
        return parsed;
    }
    parsed.valid = parsed.port > 0 && parsed.port <= 65535;
    return parsed;
}

bool TcpConnect(const std::wstring& host, int port, int timeoutMs) {
    static bool wsaStarted = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!wsaStarted) {
        return false;
    }

    ADDRINFOW hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    std::wstring portText = std::to_wstring(port);
    ADDRINFOW* result = nullptr;
    if (GetAddrInfoW(host.c_str(), portText.c_str(), &hints, &result) != 0) {
        return false;
    }

    bool connected = false;
    for (auto* item = result; item != nullptr && !connected; item = item->ai_next) {
        SOCKET sock = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (sock == INVALID_SOCKET) {
            continue;
        }
        u_long nonBlocking = 1;
        ioctlsocket(sock, FIONBIO, &nonBlocking);
        int rc = connect(sock, item->ai_addr, static_cast<int>(item->ai_addrlen));
        if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writeSet{};
            FD_ZERO(&writeSet);
            FD_SET(sock, &writeSet);
            timeval timeout{};
            timeout.tv_sec = timeoutMs / 1000;
            timeout.tv_usec = (timeoutMs % 1000) * 1000;
            rc = select(0, nullptr, &writeSet, nullptr, &timeout);
            if (rc > 0) {
                int error = 0;
                int len = sizeof(error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len);
                connected = error == 0;
            }
        } else {
            connected = rc == 0;
        }
        closesocket(sock);
    }

    FreeAddrInfoW(result);
    return connected;
}

std::wstring QuoteArgument(const std::wstring& value) {
    if (!value.empty() && value.find_first_of(L" \t\"") == std::wstring::npos) {
        return value;
    }
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            backslashes++;
        } else if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(ch);
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(ch);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring BuildCommandLine(const std::wstring& exe, const std::vector<std::wstring>& args) {
    std::wstring command = QuoteArgument(exe);
    for (const auto& arg : args) {
        command += L" ";
        command += QuoteArgument(arg);
    }
    return command;
}

std::wstring BuildArgumentString(const std::vector<std::wstring>& args) {
    std::wstring command;
    for (const auto& arg : args) {
        if (!command.empty()) {
            command += L" ";
        }
        command += QuoteArgument(arg);
    }
    return command;
}

std::wstring ReadPipeUtf8(HANDLE pipe) {
    std::string bytes;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        bytes.append(buffer, buffer + read);
    }
    return Utf8ToWide(bytes);
}

std::vector<wchar_t> BuildEnvironmentBlock(const std::map<std::wstring, std::wstring, CaseInsensitiveLess>& overrides) {
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> values;
    LPWCH raw = GetEnvironmentStringsW();
    if (raw) {
        for (LPWCH item = raw; *item; item += wcslen(item) + 1) {
            std::wstring entry = item;
            auto separator = entry.find(L'=');
            if (separator != std::wstring::npos && separator > 0) {
                values[entry.substr(0, separator)] = entry.substr(separator + 1);
            }
        }
        FreeEnvironmentStringsW(raw);
    }
    for (const auto& item : overrides) {
        values[item.first] = item.second;
    }

    std::vector<wchar_t> block;
    for (const auto& item : values) {
        std::wstring entry = item.first + L"=" + item.second;
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

ProcessOutput RunProcessCapture(
    const std::wstring& exe,
    const std::vector<std::wstring>& args,
    DWORD timeoutMs,
    const std::map<std::wstring, std::wstring, CaseInsensitiveLess>* environment = nullptr) {
    ProcessOutput result;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    HANDLE errRead = nullptr;
    HANDLE errWrite = nullptr;

    if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&errRead, &errWrite, &sa, 0)) {
        result.errorMessage = L"无法创建进程管道。";
        return result;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    auto command = BuildCommandLine(exe, args);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    auto envBlock = environment ? BuildEnvironmentBlock(*environment) : std::vector<wchar_t>{};

    BOOL ok = CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | (environment ? CREATE_UNICODE_ENVIRONMENT : 0),
        environment ? envBlock.data() : nullptr,
        nullptr,
        &si,
        &pi);

    CloseHandle(outWrite);
    CloseHandle(errWrite);

    if (!ok) {
        result.errorMessage = L"启动进程失败。";
        CloseHandle(outRead);
        CloseHandle(errRead);
        return result;
    }

    result.started = true;
    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        result.timedOut = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    GetExitCodeProcess(pi.hProcess, &result.exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    result.output = ReadPipeUtf8(outRead);
    result.error = ReadPipeUtf8(errRead);
    CloseHandle(outRead);
    CloseHandle(errRead);
    return result;
}

std::optional<std::wstring> GetUserEnvironmentVariable(const std::wstring& name) {
    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Environment", name.c_str(), RRF_RT_REG_SZ, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    status = RegGetValueW(HKEY_CURRENT_USER, L"Environment", name.c_str(), RRF_RT_REG_SZ, &type, value.data(), &bytes);
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

void BroadcastEnvironmentChanged() {
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"),
                        SMTO_ABORTIFHUNG, 2000, &ignored);
}

bool SetUserEnvironmentVariable(const std::wstring& name, const std::optional<std::wstring>& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS status = ERROR_SUCCESS;
    if (value) {
        status = RegSetValueExW(key, name.c_str(), 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(value->c_str()),
                                static_cast<DWORD>((value->size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, name.c_str());
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    BroadcastEnvironmentChanged();
    return status == ERROR_SUCCESS;
}

std::map<std::wstring, std::wstring, CaseInsensitiveLess> BuildProxyEnvironment(const LauncherSettings& settings, const std::wstring& proxy) {
    auto noProxy = Trim(settings.noProxy);
    return {
        {L"HTTP_PROXY", proxy},
        {L"http_proxy", proxy},
        {L"HTTPS_PROXY", proxy},
        {L"https_proxy", proxy},
        {L"ALL_PROXY", proxy},
        {L"all_proxy", proxy},
        {L"WS_PROXY", proxy},
        {L"ws_proxy", proxy},
        {L"WSS_PROXY", proxy},
        {L"wss_proxy", proxy},
        {L"NO_PROXY", noProxy},
        {L"no_proxy", noProxy},
        {L"NPM_CONFIG_PROXY", proxy},
        {L"npm_config_proxy", proxy},
        {L"NPM_CONFIG_HTTPS_PROXY", proxy},
        {L"npm_config_https_proxy", proxy},
        {L"GLOBAL_AGENT_HTTP_PROXY", proxy},
        {L"global_agent_http_proxy", proxy}
    };
}

bool StartProcessWithEnvironment(
    const std::wstring& exe,
    const std::vector<std::wstring>& args,
    const std::map<std::wstring, std::wstring, CaseInsensitiveLess>& environment,
    DWORD& startedPid,
    std::wstring& errorMessage) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    auto command = BuildCommandLine(exe, args);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    auto envBlock = BuildEnvironmentBlock(environment);
    auto workingDirectory = GetDirectoryName(exe);
    BOOL ok = CreateProcessW(
        exe.c_str(),
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_UNICODE_ENVIRONMENT,
        envBlock.data(),
        workingDirectory.c_str(),
        &si,
        &pi);
    if (!ok) {
        errorMessage = FormatWindowsError(GetLastError());
        return false;
    }
    startedPid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool ActivatePackagedAppWithShell(
    const std::wstring& appUserModelId,
    const std::wstring& arguments,
    DWORD& startedPid,
    std::wstring& errorMessage) {
    std::wstring target = L"shell:AppsFolder\\" + appUserModelId;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = target.c_str();
    info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        errorMessage = FormatWindowsError(GetLastError());
        return false;
    }
    if (info.hProcess) {
        startedPid = GetProcessId(info.hProcess);
        CloseHandle(info.hProcess);
    }
    return true;
}

void AddWslProxyEnvironment(std::map<std::wstring, std::wstring, CaseInsensitiveLess>& proxyEnv) {
    const std::vector<std::wstring> variableNames = {
        L"HTTP_PROXY", L"http_proxy", L"HTTPS_PROXY", L"https_proxy", L"ALL_PROXY", L"all_proxy",
        L"NO_PROXY", L"no_proxy", L"WS_PROXY", L"ws_proxy", L"WSS_PROXY", L"wss_proxy",
        L"NPM_CONFIG_PROXY", L"npm_config_proxy", L"NPM_CONFIG_HTTPS_PROXY", L"npm_config_https_proxy",
        L"GLOBAL_AGENT_HTTP_PROXY", L"global_agent_http_proxy"
    };

    auto existing = GetUserEnvironmentVariable(L"WSLENV");
    if (!existing) {
        DWORD needed = GetEnvironmentVariableW(L"WSLENV", nullptr, 0);
        if (needed > 0) {
            std::wstring value(needed, L'\0');
            GetEnvironmentVariableW(L"WSLENV", value.data(), needed);
            while (!value.empty() && value.back() == L'\0') {
                value.pop_back();
            }
            existing = value;
        }
    }

    auto entries = Split(existing.value_or(L""), L':');
    for (const auto& name : variableNames) {
        bool found = false;
        for (const auto& entry : entries) {
            if (entry == name || StartsWithI(entry, name + L"/")) {
                found = true;
                break;
            }
        }
        if (!found) {
            entries.push_back(name + L"/u");
        }
    }

    std::wstring wslenv;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) {
            wslenv += L":";
        }
        wslenv += entries[i];
    }
    proxyEnv[L"WSLENV"] = wslenv;
}

bool ConfigEnablesWslBackend(const std::wstring& configPath) {
    auto content = ReadUtf8File(configPath);
    if (!content) {
        return false;
    }
    std::wistringstream lines(*content);
    std::wstring raw;
    while (std::getline(lines, raw)) {
        auto commentStart = raw.find(L'#');
        auto line = Trim(commentStart == std::wstring::npos ? raw : raw.substr(0, commentStart));
        if (line.empty()) {
            continue;
        }
        auto separator = line.find(L'=');
        if (separator == std::wstring::npos || separator == 0) {
            continue;
        }
        auto key = Trim(line.substr(0, separator));
        if (key != L"runCodexInWindowsSubsystemForLinux") {
            continue;
        }
        auto value = Trim(line.substr(separator + 1));
        if (!value.empty() && (value.front() == L'"' || value.front() == L'\'')) {
            value.erase(value.begin());
        }
        if (!value.empty() && (value.back() == L'"' || value.back() == L'\'')) {
            value.pop_back();
        }
        return ToLower(value) == L"true";
    }
    return false;
}

WslBackendDetection DetectWslBackend(const std::optional<std::wstring>& configPath = std::nullopt) {
    std::wstring path;
    if (configPath && !Trim(*configPath).empty()) {
        path = *configPath;
    } else {
        DWORD needed = GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
        std::wstring userProfile;
        if (needed > 0) {
            userProfile.resize(needed, L'\0');
            GetEnvironmentVariableW(L"USERPROFILE", userProfile.data(), needed);
            while (!userProfile.empty() && userProfile.back() == L'\0') {
                userProfile.pop_back();
            }
        }
        path = CombinePath(CombinePath(userProfile, L".codex"), L"config.toml");
    }

    if (ConfigEnablesWslBackend(path)) {
        return {true, L"检测到 Codex 使用了 WSL 后端：runCodexInWindowsSubsystemForLinux = true。"};
    }
    return {false, L"Codex 配置未开启 runCodexInWindowsSubsystemForLinux，跳过 WSL 代理注入。"};
}

std::wstring ShQuote(std::wstring value) {
    std::wstring result = L"'";
    for (wchar_t ch : value) {
        if (ch == L'\'') {
            result += L"'\"'\"'";
        } else {
            result.push_back(ch);
        }
    }
    result.push_back(L'\'');
    return result;
}

bool IsLoopbackHost(const std::wstring& host) {
    return _wcsicmp(host.c_str(), L"localhost") == 0 || host == L"::1" || StartsWithI(host, L"127.");
}

std::wstring GetPathSuffix(const ParsedUrl& uri) {
    if (Trim(uri.pathAndQuery).empty() || uri.pathAndQuery == L"/") {
        return L"";
    }
    if (!uri.pathAndQuery.empty() && uri.pathAndQuery.front() == L'/') {
        return uri.pathAndQuery;
    }
    return L"/" + uri.pathAndQuery;
}

std::wstring BuildWslScript(const ParsedUrl& proxyUri, const std::wstring& noProxy) {
    bool loopbackProxy = IsLoopbackHost(proxyUri.host);
    std::wstring proxyPrefix = proxyUri.scheme + L"://" + (proxyUri.userInfo.empty() ? L"" : proxyUri.userInfo + L"@");
    std::wstring proxySuffix = L":" + std::to_wstring(proxyUri.port) + GetPathSuffix(proxyUri);
    std::wstring proxyUrl = proxyUri.original;

    std::wostringstream script;
    script << L"set -eu\n";
    script << L"CODEX_PROXY_PORT=" << ShQuote(std::to_wstring(proxyUri.port)) << L"\n";
    script << L"CODEX_NO_PROXY=" << ShQuote(noProxy) << L"\n";
    script << LR"SH(codex_test_tcp() {
  command -v bash >/dev/null 2>&1 || return 2
  if command -v timeout >/dev/null 2>&1; then
    CODEX_TEST_HOST="$1" CODEX_TEST_PORT="$2" timeout 4 bash -lc ': >/dev/tcp/$CODEX_TEST_HOST/$CODEX_TEST_PORT' >/dev/null 2>&1
  else
    CODEX_TEST_HOST="$1" CODEX_TEST_PORT="$2" bash -lc ': >/dev/tcp/$CODEX_TEST_HOST/$CODEX_TEST_PORT' >/dev/null 2>&1
  fi
}
codex_escape_double() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\$/\\$/g; s/`/\\`/g'
}
codex_write_export() {
  CODEX_EXPORT_VALUE="$(codex_escape_double "$2")"
  printf 'export %s="%s"\n' "$1" "$CODEX_EXPORT_VALUE"
}
)SH";

    if (loopbackProxy) {
        script << L"CODEX_PROXY_PREFIX=" << ShQuote(proxyPrefix) << L"\n";
        script << L"CODEX_PROXY_SUFFIX=" << ShQuote(proxySuffix) << L"\n";
        script << LR"SH(CODEX_WINDOWS_HOST="$(awk '/^nameserver[ \t]+/ {print $2; exit}' /etc/resolv.conf 2>/dev/null || true)"
if [ -z "$CODEX_WINDOWS_HOST" ]; then
  CODEX_WINDOWS_HOST="$(ip route 2>/dev/null | awk '/^default / {print $3; exit}' || true)"
fi
CODEX_PROXY_HOST=""
for CODEX_CANDIDATE_HOST in 127.0.0.1 localhost "$CODEX_WINDOWS_HOST"; do
  if [ -n "$CODEX_CANDIDATE_HOST" ] && codex_test_tcp "$CODEX_CANDIDATE_HOST" "$CODEX_PROXY_PORT"; then
    CODEX_PROXY_HOST="$CODEX_CANDIDATE_HOST"
    break
  fi
done
if [ -z "$CODEX_PROXY_HOST" ]; then
  CODEX_PROXY_HOST="$CODEX_WINDOWS_HOST"
  CODEX_PROXY_REACHABLE=0
else
  CODEX_PROXY_REACHABLE=1
fi
CODEX_PROXY_URL="${CODEX_PROXY_PREFIX}${CODEX_PROXY_HOST}${CODEX_PROXY_SUFFIX}"
)SH";
    } else {
        script << L"CODEX_PROXY_HOST=" << ShQuote(proxyUri.host) << L"\n";
        script << L"CODEX_PROXY_URL=" << ShQuote(proxyUrl) << L"\n";
        script << LR"SH(if codex_test_tcp "$CODEX_PROXY_HOST" "$CODEX_PROXY_PORT"; then
  CODEX_PROXY_REACHABLE=1
else
  CODEX_PROXY_REACHABLE=0
fi
)SH";
    }

    script << LR"SH(mkdir -p "$HOME/.codex"
CODEX_PROXY_FILE="$HOME/.codex/proxy-env.sh"
{
  printf '%s\n' '# Generated by Codex Proxy Launcher. Edit config.txt in Windows instead.'
  for CODEX_PROXY_NAME in HTTP_PROXY http_proxy HTTPS_PROXY https_proxy ALL_PROXY all_proxy WS_PROXY ws_proxy WSS_PROXY wss_proxy NPM_CONFIG_PROXY npm_config_proxy NPM_CONFIG_HTTPS_PROXY npm_config_https_proxy GLOBAL_AGENT_HTTP_PROXY global_agent_http_proxy; do
    codex_write_export "$CODEX_PROXY_NAME" "$CODEX_PROXY_URL"
  done
  codex_write_export NO_PROXY "$CODEX_NO_PROXY"
  codex_write_export no_proxy "$CODEX_NO_PROXY"
} > "$CODEX_PROXY_FILE"
chmod 600 "$CODEX_PROXY_FILE"
CODEX_MARKER='# codex-proxy-gui proxy'
CODEX_SOURCE_LINE='[ -f "$HOME/.codex/proxy-env.sh" ] && . "$HOME/.codex/proxy-env.sh"'
for CODEX_RC in "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc"; do
  touch "$CODEX_RC"
  if ! grep -Fq "$CODEX_MARKER" "$CODEX_RC"; then
    printf '\n%s\n%s\n' "$CODEX_MARKER" "$CODEX_SOURCE_LINE" >> "$CODEX_RC"
  fi
done
. "$CODEX_PROXY_FILE"
printf 'WSL_PROXY=%s\n' "$HTTP_PROXY"
if [ "$CODEX_PROXY_REACHABLE" != "1" ]; then
  printf 'CODEX_PROXY_UNREACHABLE %s %s\n' "$CODEX_PROXY_HOST" "$CODEX_PROXY_PORT" >&2
  exit 13
fi
)SH";
    return script.str();
}

bool TryParseUnreachableProxy(const std::wstring& error, const std::wstring& output, std::wstring& host, std::wstring& port) {
    std::wistringstream lines(error + L"\n" + output);
    std::wstring line;
    const std::wstring marker = L"CODEX_PROXY_UNREACHABLE ";
    while (std::getline(lines, line)) {
        auto trimmed = Trim(line);
        if (!StartsWithI(trimmed, marker)) {
            continue;
        }
        std::wistringstream parts(trimmed.substr(marker.size()));
        if (parts >> host >> port) {
            return true;
        }
    }
    return false;
}

OperationResult ConfigureWslProxy(const ParsedUrl& proxyUri, const std::wstring& noProxy) {
    auto process = RunProcessCapture(L"wsl.exe", {L"-e", L"sh", L"-lc", BuildWslScript(proxyUri, noProxy)}, 15000);
    if (!process.started) {
        return Fail(L"WSL 内代理设置失败：" + process.errorMessage);
    }
    if (process.timedOut) {
        return Fail(L"WSL 内代理设置超时。");
    }
    auto output = Trim(process.output);
    auto error = Trim(process.error);
    if (process.exitCode != 0) {
        std::wstring host;
        std::wstring port;
        if (process.exitCode == 13 && TryParseUnreachableProxy(error, output, host, port)) {
            return ProxyUnreachable(L"WSL 无法连接代理 " + host + L":" + port + L"。请确认 Windows 代理允许 WSL 访问，必要时开启代理软件的 Allow LAN/局域网访问。");
        }
        return Fail(error.empty() ? output : error);
    }
    return Ok(output.empty() ? L"WSL 代理环境已更新。" : output);
}

OperationResult VerifyWslProxy(std::map<std::wstring, std::wstring, CaseInsensitiveLess> environment, const std::wstring& expectedProxy, const std::wstring& expectedNoProxy) {
    auto process = RunProcessCapture(
        L"wsl.exe",
        {L"-e", L"sh", L"-c", L"printf '%s\n%s\n%s\n%s\n%s\n' \"$HTTP_PROXY\" \"$HTTPS_PROXY\" \"$ALL_PROXY\" \"$NO_PROXY\" \"$WSLENV\""},
        10000,
        &environment);
    if (!process.started) {
        return Fail(L"WSL 代理验证失败：" + process.errorMessage);
    }
    if (process.timedOut) {
        return Fail(L"WSL 代理验证超时。");
    }
    if (process.exitCode != 0) {
        return Fail(L"WSL 代理验证失败：" + Trim(process.error));
    }
    std::vector<std::wstring> lines;
    std::wistringstream reader(process.output);
    std::wstring line;
    while (std::getline(reader, line)) {
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    if (lines.size() < 5) {
        return Fail(L"WSL 代理验证输出不完整。");
    }
    if (lines[0] != expectedProxy || lines[1] != expectedProxy || lines[2] != expectedProxy ||
        lines[3] != expectedNoProxy || lines[4].find(L"HTTP_PROXY/u") == std::wstring::npos ||
        lines[4].find(L"NO_PROXY/u") == std::wstring::npos) {
        return Fail(L"WSL 未收到完整代理环境变量。");
    }
    return Ok(L"WSL 可以收到启动器注入的代理环境变量。");
}

bool IsWindowsAppsCodexPath(const std::wstring& path) {
    return ContainsI(path, L"\\WindowsApps\\OpenAI.Codex_") && EndsWithI(path, L"\\app\\Codex.exe");
}

int GetVersionScore(const std::wstring& path) {
    auto appDir = GetDirectoryName(path);
    auto packageDir = GetDirectoryName(appDir);
    auto folder = packageDir.substr(packageDir.find_last_of(L"\\/") + 1);
    int major = 0;
    int minor = 0;
    int build = 0;
    std::wstring digits;
    for (wchar_t ch : folder) {
        if (iswdigit(ch) || ch == L'.') {
            digits.push_back(ch);
        }
    }
    swscanf_s(digits.c_str(), L"%d.%d.%d", &major, &minor, &build);
    return major * 1000000 + minor * 10000 + build;
}

std::vector<std::wstring> EnumerateCodexCandidates() {
    auto getFolder = [](int csidl) {
        wchar_t path[MAX_PATH]{};
        SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, path);
        return std::wstring(path);
    };

    auto local = getFolder(CSIDL_LOCAL_APPDATA);
    auto programFiles = getFolder(CSIDL_PROGRAM_FILES);
    auto programFilesX86 = getFolder(CSIDL_PROGRAM_FILESX86);
    std::vector<std::wstring> candidates = {
        CombinePath(local, L"Programs\\Codex\\Codex.exe"),
        CombinePath(local, L"Programs\\OpenAI Codex\\Codex.exe"),
        CombinePath(local, L"Programs\\OpenAI\\Codex\\Codex.exe"),
        CombinePath(programFiles, L"Codex\\Codex.exe"),
        CombinePath(programFiles, L"OpenAI Codex\\Codex.exe"),
        CombinePath(programFilesX86, L"Codex\\Codex.exe"),
        CombinePath(local, L"Microsoft\\WindowsApps\\Codex.exe")
    };

    auto windowsApps = CombinePath(programFiles, L"WindowsApps");
    if (DirectoryExists(windowsApps)) {
        WIN32_FIND_DATAW data{};
        auto pattern = CombinePath(windowsApps, L"OpenAI.Codex_*");
        HANDLE find = FindFirstFileW(pattern.c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    candidates.push_back(CombinePath(CombinePath(windowsApps, data.cFileName), L"app\\Codex.exe"));
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
    }
    return candidates;
}

bool TryResolveCodex(const std::wstring& configuredPath, std::wstring& exe, std::wstring& message) {
    exe.clear();
    auto configured = Trim(configuredPath);
    if (!configured.empty()) {
        if (configured.front() == L'"' && configured.back() == L'"') {
            configured = configured.substr(1, configured.size() - 2);
        }
        auto expanded = ExpandEnvironment(configured);
        if (FileExists(expanded)) {
            exe = FullPath(expanded);
            message = L"已使用 config.txt 中设置的 Codex.exe。";
            return true;
        }
        message = L"config.txt 中设置的 Codex.exe 不存在：" + expanded;
        return false;
    }

    std::vector<std::wstring> candidates;
    std::set<std::wstring, CaseInsensitiveLess> seen;
    for (const auto& item : EnumerateCodexCandidates()) {
        if (FileExists(item) && seen.insert(item).second) {
            candidates.push_back(item);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        bool leftApp = IsWindowsAppsCodexPath(left);
        bool rightApp = IsWindowsAppsCodexPath(right);
        if (leftApp != rightApp) {
            return leftApp > rightApp;
        }
        return GetVersionScore(left) > GetVersionScore(right);
    });

    if (candidates.empty()) {
        message = L"未能自动找到 Codex.exe。可在 config.txt 中设置 codex_exe_path。";
        return false;
    }
    exe = candidates[0];
    message = L"已找到 Codex.exe：" + exe;
    return true;
}

std::vector<DWORD> FindCodexProcesses(const std::wstring& exe) {
    std::vector<DWORD> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) {
                continue;
            }
            std::wstring path(32768, L'\0');
            DWORD size = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
                path.resize(size);
                if (_wcsicmp(path.c_str(), exe.c_str()) == 0 || IsWindowsAppsCodexPath(path)) {
                    processes.push_back(entry.th32ProcessID);
                }
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return processes;
}

std::wstring FirstNonEmpty(const std::wstring& first, const std::wstring& second) {
    auto trimmed = Trim(first);
    return trimmed.empty() ? Trim(second) : trimmed;
}

std::wstring BuildChromiumBypassList(const std::wstring& noProxy) {
    std::vector<std::wstring> items = {L"<local>"};
    auto parts = Split(noProxy, L',');
    for (const auto& part : parts) {
        bool exists = false;
        for (const auto& item : items) {
            if (_wcsicmp(item.c_str(), part.c_str()) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            items.push_back(part);
        }
    }
    std::wstring result;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            result += L";";
        }
        result += items[i];
    }
    return result;
}

std::wstring GetXmlAttribute(const std::wstring& xml, const std::wstring& element, const std::wstring& attribute) {
    try {
        std::wregex elementRegex(std::wstring(L"<\\s*") + element + L"\\b[^>]*>", std::regex_constants::icase);
        std::wsmatch elementMatch;
        if (!std::regex_search(xml, elementMatch, elementRegex)) {
            return L"";
        }
        std::wstring tag = elementMatch.str();
        std::wregex attrRegex(attribute + L"\\s*=\\s*\"([^\"]*)\"", std::regex_constants::icase);
        std::wsmatch attrMatch;
        if (std::regex_search(tag, attrMatch, attrRegex) && attrMatch.size() > 1) {
            return attrMatch[1].str();
        }
    } catch (...) {
    }
    return L"";
}

std::wstring ResolveAppUserModelId(const std::wstring& exe) {
    auto appDir = GetDirectoryName(exe);
    auto packageDir = GetDirectoryName(appDir);
    auto manifestPath = CombinePath(packageDir, L"AppxManifest.xml");
    auto manifest = ReadUtf8File(manifestPath);
    if (!manifest) {
        throw std::runtime_error("manifest missing");
    }
    auto identityName = GetXmlAttribute(*manifest, L"Identity", L"Name");
    auto appId = GetXmlAttribute(*manifest, L"Application", L"Id");
    auto folder = packageDir.substr(packageDir.find_last_of(L"\\/") + 1);
    auto marker = folder.rfind(L"__");
    if (identityName.empty() || appId.empty() || marker == std::wstring::npos) {
        throw std::runtime_error("manifest parse failed");
    }
    auto publisherId = folder.substr(marker + 2);
    return identityName + L"_" + publisherId + L"!" + appId;
}

class LauncherService {
public:
    using LogFn = std::function<void(const std::wstring&)>;

    LauncherService(LauncherSettings settings, LogFn log) : settings_(std::move(settings)), log_(std::move(log)) {}

    OperationResult TestProxy() {
        auto proxy = Trim(settings_.proxyAddress);
        if (proxy.empty()) {
            return Fail(L"请先填写代理地址。");
        }
        auto uri = ParseUrl(proxy);
        if (!uri.valid) {
            return Fail(L"代理地址格式不正确：" + proxy);
        }
        log_(L"正在测试代理端口：" + uri.host + L":" + std::to_wstring(uri.port) + L"...");
        if (!TcpConnect(uri.host, uri.port, 2000)) {
            return Fail(L"代理端口无法连接。");
        }
        return Ok(L"代理端口可以连接。");
    }

    OperationResult TestWslProxyPropagation() {
        auto proxy = Trim(settings_.proxyAddress);
        if (proxy.empty()) {
            return Fail(L"请先填写代理地址。");
        }
        auto proxyEnv = BuildProxyEnvironment(settings_, proxy);
        AddWslProxyEnvironment(proxyEnv);
        return VerifyWslProxy(proxyEnv, proxy, Trim(settings_.noProxy));
    }

    OperationResult TestWslProxySetup() {
        auto proxy = Trim(settings_.proxyAddress);
        if (proxy.empty()) {
            return Fail(L"请先填写代理地址。");
        }
        auto uri = ParseUrl(proxy);
        if (!uri.valid) {
            return Fail(L"代理地址格式不正确：" + proxy);
        }
        return ConfigureWslProxy(uri, Trim(settings_.noProxy));
    }

    OperationResult TestWslProxyWarning() {
        auto setup = TestWslProxySetup();
        if (setup.success || !setup.wslProxyUnreachable) {
            return setup;
        }
        return WithWslProxyWarning(Ok(L"测试 WSL 代理端口警告。"), WslProxyUnreachableWarning());
    }

    OperationResult StartCodex() {
        std::optional<WslBackendDetection> wslDetection;
        std::wstring wslProxyWarning;
        try {
            wslDetection = DetectWslBackend();
            if (wslDetection->shouldApplyProxy) {
                log_(wslDetection->message);
            }
        } catch (...) {
        }

        std::wstring exe;
        std::wstring resolveMessage;
        if (!TryResolveCodex(settings_.codexExePath, exe, resolveMessage)) {
            return Fail(resolveMessage);
        }

        auto proxy = Trim(settings_.proxyAddress);
        if (proxy.empty()) {
            return Fail(L"请填写代理地址。");
        }
        auto proxyUri = ParseUrl(proxy);
        if (!proxyUri.valid) {
            return Fail(L"代理地址格式不正确：" + proxy);
        }

        auto chromiumProxy = FirstNonEmpty(settings_.chromiumProxy, proxy);
        std::vector<std::wstring> arguments = {
            L"--proxy-server=" + chromiumProxy,
            L"--proxy-bypass-list=" + BuildChromiumBypassList(settings_.noProxy)
        };

        auto proxyEnv = BuildProxyEnvironment(settings_, proxy);
        if (wslDetection && wslDetection->shouldApplyProxy) {
            try {
                AddWslProxyEnvironment(proxyEnv);
                log_(L"已为 Codex 的 WSL 后端加入代理环境变量。");
            } catch (const std::exception& ex) {
                log_(L"WSL 代理注入失败，将继续启动 Codex：" + Utf8ToWide(ex.what()));
            }

            try {
                auto wslSetup = ConfigureWslProxy(proxyUri, Trim(settings_.noProxy));
                if (wslSetup.success) {
                    log_(L"已在 WSL 内写入代理配置：" + wslSetup.message);
                } else {
                    log_(L"WSL 内代理设置失败，将继续启动 Codex：" + wslSetup.message);
                    if (wslSetup.wslProxyUnreachable) {
                        wslProxyWarning = WslProxyUnreachableWarning();
                        log_(wslProxyWarning);
                    }
                }
            } catch (const std::exception& ex) {
                log_(L"WSL 内代理设置失败，将继续启动 Codex：" + Utf8ToWide(ex.what()));
            }
        }

        if (!FindCodexProcesses(exe).empty()) {
            return WithWslProxyWarning(Prompt(L"Codex 已经在运行，请先关闭 Codex 后再启动。"), wslProxyWarning);
        }

        log_(L"正在启动 Codex...");
        AppendFileLog(L"启动：exe=" + exe);

        DWORD startedPid = 0;
        std::vector<std::pair<std::wstring, std::optional<std::wstring>>> previousUserEnv;
        std::vector<std::wstring> launchDiagnostics;
        auto flushLaunchDiagnostics = [&] {
            for (const auto& line : launchDiagnostics) {
                log_(line);
            }
            launchDiagnostics.clear();
        };

        if (IsWindowsAppsCodexPath(exe)) {
            if (settings_.temporarilySetUserProxyEnvironment) {
                log_(L"正在为启动过程临时写入代理环境变量...");
                for (const auto& item : proxyEnv) {
                    previousUserEnv.push_back({item.first, GetUserEnvironmentVariable(item.first)});
                    SetUserEnvironmentVariable(item.first, item.second);
                }
            }

            bool launched = false;
            std::wstring appUserModelId;
            auto argumentString = BuildArgumentString(arguments);
            try {
                appUserModelId = ResolveAppUserModelId(exe);
                launchDiagnostics.push_back(L"使用 Windows 应用入口启动：" + appUserModelId);

                IApplicationActivationManager* activator = nullptr;
                HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_IApplicationActivationManager, reinterpret_cast<void**>(&activator));
                if (FAILED(hr) || !activator) {
                    launchDiagnostics.push_back(L"创建 Windows 应用启动器失败，改用备用启动方式。HRESULT：0x" + HResultHex(hr));
                } else {
                    hr = activator->ActivateApplication(appUserModelId.c_str(), argumentString.c_str(), AO_NOERRORUI, &startedPid);
                    activator->Release();
                    if (SUCCEEDED(hr)) {
                        launched = true;
                    } else {
                        launchDiagnostics.push_back(L"通过 Windows 应用入口启动失败，改用备用启动方式。HRESULT：0x" + HResultHex(hr));
                    }
                }
            } catch (...) {
                launchDiagnostics.push_back(L"无法从应用清单解析 Codex 启动入口，改用备用启动方式。");
            }

            if (!launched) {
                std::wstring errorMessage;
                launchDiagnostics.push_back(L"正在尝试直接启动 Codex.exe...");
                launched = StartProcessWithEnvironment(exe, arguments, proxyEnv, startedPid, errorMessage);
                if (!launched) {
                    launchDiagnostics.push_back(L"直接启动 Codex.exe 失败：" + errorMessage);
                }
            }

            if (!launched && !appUserModelId.empty()) {
                std::wstring errorMessage;
                launchDiagnostics.push_back(L"正在尝试通过 Shell 应用入口启动 Codex...");
                launched = ActivatePackagedAppWithShell(appUserModelId, argumentString, startedPid, errorMessage);
                if (!launched) {
                    launchDiagnostics.push_back(L"通过 Shell 应用入口启动失败：" + errorMessage);
                }
            }

            if (!launched) {
                RestoreUserEnvironment(previousUserEnv);
                flushLaunchDiagnostics();
                return WithWslProxyWarning(Fail(L"启动 Codex 失败，所有 WindowsApps 启动方式都不可用。"), wslProxyWarning);
            }
        } else {
            std::wstring errorMessage;
            if (!StartProcessWithEnvironment(exe, arguments, proxyEnv, startedPid, errorMessage)) {
                return WithWslProxyWarning(Fail(L"启动 Codex 失败：" + errorMessage), wslProxyWarning);
            }
        }

        RestoreUserEnvironment(previousUserEnv);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(settings_.startupWaitSeconds);
        while (std::chrono::steady_clock::now() < deadline) {
            auto processes = FindCodexProcesses(exe);
            if (!processes.empty()) {
                return WithWslProxyWarning(Ok(L"Codex 已通过代理启动。PID：" + std::to_wstring(processes[0])), wslProxyWarning);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        flushLaunchDiagnostics();
        return WithWslProxyWarning(Fail(L"已尝试启动 Codex，但在 " + std::to_wstring(settings_.startupWaitSeconds) + L" 秒内未找到正在运行的匹配进程。日志：" + LogPath()), wslProxyWarning);
    }

private:
    static constexpr CLSID CLSID_ApplicationActivationManager{0x45BA127D, 0x10A8, 0x46EA, {0x8A, 0xB7, 0x56, 0xEA, 0x90, 0x79, 0x43, 0xC3}};
    static constexpr IID IID_IApplicationActivationManager{0x2E941141, 0x7F97, 0x4756, {0xBA, 0x1D, 0x9D, 0xEC, 0xDE, 0x89, 0x4A, 0x3D}};

    enum ACTIVATEOPTIONS {
        AO_NONE = 0,
        AO_DESIGNMODE = 0x1,
        AO_NOERRORUI = 0x2,
        AO_NOSPLASHSCREEN = 0x4
    };

    struct IApplicationActivationManager : public IUnknown {
        virtual HRESULT STDMETHODCALLTYPE ActivateApplication(LPCWSTR appUserModelId, LPCWSTR arguments, ACTIVATEOPTIONS options, DWORD* processId) = 0;
        virtual HRESULT STDMETHODCALLTYPE ActivateForFile(LPCWSTR appUserModelId, IUnknown* itemArray, LPCWSTR verb, DWORD* processId) = 0;
        virtual HRESULT STDMETHODCALLTYPE ActivateForProtocol(LPCWSTR appUserModelId, IUnknown* itemArray, DWORD* processId) = 0;
    };

    static std::wstring HResultHex(HRESULT hr) {
        wchar_t buffer[16]{};
        swprintf_s(buffer, L"%08X", static_cast<unsigned int>(hr));
        return buffer;
    }

    void RestoreUserEnvironment(const std::vector<std::pair<std::wstring, std::optional<std::wstring>>>& values) {
        if (values.empty()) {
            return;
        }
        for (const auto& item : values) {
            SetUserEnvironmentVariable(item.first, item.second);
        }
        log_(L"已恢复当前用户代理环境变量。");
    }

    LauncherSettings settings_;
    LogFn log_;
};

HFONT MakeFont(double points, int weight, const wchar_t* face, UINT dpi) {
    int height = -MulDiv(static_cast<int>(points * 10 + 0.5), static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), 720);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kTextColor);
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        bool rounded = GetWindowLongPtrW(hwnd, GWLP_USERDATA) == 1;
        HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
        HBRUSH parent = CreateSolidBrush(kWindowColor);
        FillRect(dc, &rc, parent);
        DeleteObject(parent);
        if (rounded) {
            HPEN pen = CreatePen(PS_SOLID, 1, kBorderColor);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, bg);
            RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        } else {
            FillRect(dc, &rc, bg);
            HPEN pen = CreatePen(PS_SOLID, 1, kInputBorderColor);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
        DeleteObject(bg);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

class MainWindow {
public:
    explicit MainWindow(HINSTANCE instance) : instance_(instance) {}

    bool Create() {
        RegisterPanelClass();
        WNDCLASSW wc{};
        wc.lpfnWndProc = &MainWindow::WndProc;
        wc.hInstance = instance_;
        wc.lpszClassName = L"CodexProxyLauncherWin32Window";
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);

        dpi_ = GetSystemDpi();
        int windowWidth = ScaleForDpi(kWindowWidth, dpi_);
        int windowHeight = ScaleForDpi(kWindowHeight, dpi_);
        hwnd_ = CreateWindowExW(
            0,
            wc.lpszClassName,
            L"Codex 代理启动器",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            windowWidth,
            windowHeight,
            nullptr,
            nullptr,
            instance_,
            this);
        return hwnd_ != nullptr;
    }

    int Run() {
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

    HWND Hwnd() const {
        return hwnd_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<MainWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        return self->HandleMessage(message, wParam, lParam);
    }

    static void RegisterPanelClass() {
        static bool registered = false;
        if (registered) {
            return;
        }
        WNDCLASSW wc{};
        wc.lpfnWndProc = PanelProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"CodexProxyPanel";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            OnCreate();
            return 0;
        case WM_ERASEBKGND:
            return OnEraseBackground(reinterpret_cast<HDC>(wParam));
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = Scale(kMinWindowWidth);
            info->ptMinTrackSize.y = Scale(kMinWindowHeight);
            return 0;
        }
        case WM_DPICHANGED:
            OnDpiChanged(HIWORD(wParam), reinterpret_cast<RECT*>(lParam));
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            return OnCtlColor(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));
        case WM_DRAWITEM:
            DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_COMMAND:
            OnCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_APP_LOG: {
            std::unique_ptr<std::wstring> messageText(reinterpret_cast<std::wstring*>(lParam));
            AppendLog(*messageText);
            return 0;
        }
        case WM_APP_ACTION_DONE: {
            std::unique_ptr<OperationResult> result(reinterpret_cast<OperationResult*>(lParam));
            AppendLog(result->message);
            SetBusy(false);
            if (result->showMessageBox) {
                MessageBoxW(hwnd_, result->dialogMessage.c_str(), result->dialogTitle.c_str(), MB_OK | result->dialogIcon);
            }
            return 0;
        }
        case WM_DESTROY:
            DeleteObject(font_);
            DeleteObject(fontSemiBold_);
            DeleteObject(fontButton_);
            DeleteObject(fontMono_);
            DeleteObject(windowBrush_);
            DeleteObject(whiteBrush_);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    int Scale(int value) const {
        return ScaleForDpi(value, dpi_);
    }

    int FontTextHeight(HFONT font) const {
        HDC dc = GetDC(hwnd_);
        if (!dc) {
            return Scale(17);
        }
        HGDIOBJ oldFont = SelectObject(dc, font);
        TEXTMETRICW metrics{};
        int height = GetTextMetricsW(dc, &metrics) ? metrics.tmHeight : Scale(17);
        SelectObject(dc, oldFont);
        ReleaseDC(hwnd_, dc);
        return height;
    }

    void DeleteFont(HFONT& font) {
        if (font) {
            DeleteObject(font);
            font = nullptr;
        }
    }

    void RecreateFonts() {
        DeleteFont(font_);
        DeleteFont(fontSemiBold_);
        DeleteFont(fontButton_);
        DeleteFont(fontMono_);

        font_ = MakeFont(10.5, FW_NORMAL, L"Microsoft YaHei UI", dpi_);
        fontSemiBold_ = MakeFont(11.5, FW_NORMAL, L"Microsoft YaHei UI", dpi_);
        fontButton_ = MakeFont(10.5, FW_NORMAL, L"Microsoft YaHei UI", dpi_);
        fontMono_ = MakeFont(10, FW_NORMAL, L"Microsoft YaHei UI", dpi_);

        if (label_) {
            SendMessageW(label_, WM_SETFONT, reinterpret_cast<WPARAM>(fontSemiBold_), TRUE);
        }
        if (proxyEdit_) {
            SendMessageW(proxyEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
        if (logEdit_) {
            SendMessageW(logEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(fontMono_), TRUE);
        }
        if (startButton_) {
            InvalidateRect(startButton_, nullptr, TRUE);
        }
        if (testButton_) {
            InvalidateRect(testButton_, nullptr, TRUE);
        }
    }

    LRESULT OnEraseBackground(HDC dc) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if (windowBrush_) {
            FillRect(dc, &rc, windowBrush_);
        } else {
            HBRUSH brush = CreateSolidBrush(kWindowColor);
            FillRect(dc, &rc, brush);
            DeleteObject(brush);
        }
        return 1;
    }

    void OnDpiChanged(UINT dpi, RECT* suggestedRect) {
        dpi_ = dpi == 0 ? kDefaultDpi : dpi;
        RecreateFonts();
        if (suggestedRect) {
            SetWindowPos(hwnd_, nullptr,
                         suggestedRect->left,
                         suggestedRect->top,
                         suggestedRect->right - suggestedRect->left,
                         suggestedRect->bottom - suggestedRect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        Layout();
    }

    void OnCreate() {
        settings_ = LoadSettings();
        dpi_ = GetWindowDpi(hwnd_);
        RecreateFonts();
        windowBrush_ = CreateSolidBrush(kWindowColor);
        whiteBrush_ = CreateSolidBrush(RGB(255, 255, 255));

        label_ = CreateWindowExW(0, L"STATIC", L"代理地址", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                0, 0, 1, 1, hwnd_, nullptr, instance_, nullptr);
        SendMessageW(label_, WM_SETFONT, reinterpret_cast<WPARAM>(fontSemiBold_), TRUE);

        inputHost_ = CreateWindowExW(0, L"CodexProxyPanel", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                    0, 0, 1, 1, hwnd_, nullptr, instance_, nullptr);
        proxyEdit_ = CreateWindowExW(0, L"EDIT", settings_.proxyAddress.c_str(),
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    0, 0, 1, 1, inputHost_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROXY)), instance_, nullptr);
        SendMessageW(proxyEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

        startButton_ = CreateWindowExW(0, L"BUTTON", L"启动 Codex",
                                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                      0, 0, 122, 38, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_START)), instance_, nullptr);
        testButton_ = CreateWindowExW(0, L"BUTTON", L"测试代理",
                                     WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                     0, 0, 122, 38, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEST)), instance_, nullptr);

        logHost_ = CreateWindowExW(0, L"CodexProxyPanel", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                  0, 0, 1, 1, hwnd_, nullptr, instance_, nullptr);
        SetWindowLongPtrW(logHost_, GWLP_USERDATA, 1);
        logEdit_ = CreateWindowExW(0, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                                  0, 0, 1, 1, logHost_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG)), instance_, nullptr);
        SendMessageW(logEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(fontMono_), TRUE);

        AppendLog(L"已加载配置：" + ConfigPath());
        SetFocus(startButton_);
        Layout();
    }

    LRESULT OnCtlColor(HDC dc, HWND child) {
        SetTextColor(dc, kTextColor);
        if (child == label_) {
            SetBkColor(dc, kWindowColor);
            return reinterpret_cast<LRESULT>(windowBrush_);
        }
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(whiteBrush_);
    }

    void DrawButton(DRAWITEMSTRUCT* item) {
        bool primary = item->CtlID == IDC_START;
        bool disabled = (item->itemState & ODS_DISABLED) != 0;
        COLORREF bg = primary ? kAccentColor : kButtonNeutralColor;
        COLORREF fg = primary ? RGB(255, 255, 255) : kTextColor;
        if (disabled) {
            bg = RGB(210, 214, 220);
            fg = RGB(120, 126, 136);
        }
        HBRUSH brush = CreateSolidBrush(bg);
        FillRect(item->hDC, &item->rcItem, brush);
        DeleteObject(brush);
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, fg);
        HGDIOBJ oldFont = SelectObject(item->hDC, fontButton_);
        wchar_t text[64]{};
        GetWindowTextW(item->hwndItem, text, 64);
        RECT textRect = item->rcItem;
        InflateRect(&textRect, -Scale(2), -Scale(1));
        DrawTextW(item->hDC, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(item->hDC, oldFont);
        if (item->itemState & ODS_FOCUS) {
            RECT focus = item->rcItem;
            InflateRect(&focus, -3, -3);
            DrawFocusRect(item->hDC, &focus);
        }
    }

    void OnCommand(WORD id, WORD code) {
        if (id == IDC_PROXY && code == EN_CHANGE && !loading_) {
            SaveSettingsFromControls();
        } else if (id == IDC_START && code == BN_CLICKED) {
            RunAction(L"start");
        } else if (id == IDC_TEST && code == BN_CLICKED) {
            RunAction(L"test");
        }
    }

    void SaveSettingsFromControls() {
        int length = GetWindowTextLengthW(proxyEdit_);
        std::wstring value(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(proxyEdit_, value.data(), length + 1);
        value.resize(static_cast<size_t>(length));
        settings_.proxyAddress = Trim(std::move(value));
        SaveSettings(settings_);
    }

    void RunAction(std::wstring action) {
        SetBusy(true);
        SaveSettingsFromControls();
        auto settings = settings_;
        HWND hwnd = hwnd_;
        std::thread([settings, action = std::move(action), hwnd] {
            HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            LauncherService service(settings, [hwnd](const std::wstring& line) {
                PostMessageW(hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(new std::wstring(line)));
            });
            OperationResult result;
            try {
                if (action == L"start") {
                    result = service.StartCodex();
                } else if (action == L"test") {
                    result = service.TestProxy();
                } else {
                    result = Fail(L"未知操作。");
                }
            } catch (const std::exception& ex) {
                result = Fail(Utf8ToWide(ex.what()));
            }
            PostMessageW(hwnd, WM_APP_ACTION_DONE, 0, reinterpret_cast<LPARAM>(new OperationResult(std::move(result))));
            if (SUCCEEDED(comInit)) {
                CoUninitialize();
            }
        }).detach();
    }

    void SetBusy(bool busy) {
        EnableWindow(startButton_, !busy);
        EnableWindow(testButton_, !busy);
        SetCursor(LoadCursorW(nullptr, busy ? IDC_WAIT : IDC_ARROW));
    }

    void AppendLog(const std::wstring& message) {
        AppendFileLog(message);
        std::wstring line = L"[" + CurrentTimeForLog(false) + L"] " + message + L"\r\n";
        int length = GetWindowTextLengthW(logEdit_);
        SendMessageW(logEdit_, EM_SETSEL, length, length);
        SendMessageW(logEdit_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    }

    void Layout() {
        if (!label_ || !inputHost_ || !proxyEdit_ || !startButton_ || !testButton_ || !logHost_ || !logEdit_) {
            return;
        }
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;
        int left = Scale(34);
        int top = Scale(20);
        int rightPad = Scale(34);
        int bottomPad = Scale(18);
        int contentWidth = std::max(1, width - left - rightPad);

        auto move = [](HWND hwnd, int x, int y, int cx, int cy) {
            SetWindowPos(hwnd, nullptr, x, y, std::max(1, cx), std::max(1, cy),
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
        };

        int labelTop = top + Scale(4);
        int inputTop = top + Scale(40);
        int inputHeight = Scale(34);
        move(label_, left, labelTop, contentWidth, Scale(28));
        move(inputHost_, left, inputTop, contentWidth - 1, inputHeight);
        int editHeight = std::min(inputHeight - Scale(4), FontTextHeight(font_) + Scale(6));
        int editY = std::max(Scale(1), (inputHeight - editHeight) / 2 + Scale(5));
        editY = std::min(editY, std::max(Scale(1), inputHeight - editHeight - Scale(1)));
        move(proxyEdit_, Scale(10), editY, contentWidth - Scale(22), editHeight);

        int buttonWidth = Scale(112);
        int buttonHeight = Scale(36);
        int buttonY = top + Scale(92);
        int startX = left + contentWidth - buttonWidth;
        int testX = startX - Scale(8) - buttonWidth;
        move(startButton_, startX, buttonY, buttonWidth, buttonHeight);
        move(testButton_, testX, buttonY, buttonWidth, buttonHeight);

        int logY = top + Scale(138);
        int logHeight = std::max(1, height - logY - bottomPad);
        move(logHost_, left, logY, contentWidth, logHeight);
        move(logEdit_, Scale(12), Scale(12), contentWidth - Scale(24), logHeight - Scale(24));

        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND label_ = nullptr;
    HWND inputHost_ = nullptr;
    HWND proxyEdit_ = nullptr;
    HWND startButton_ = nullptr;
    HWND testButton_ = nullptr;
    HWND logHost_ = nullptr;
    HWND logEdit_ = nullptr;
    HFONT font_ = nullptr;
    HFONT fontSemiBold_ = nullptr;
    HFONT fontButton_ = nullptr;
    HFONT fontMono_ = nullptr;
    HBRUSH windowBrush_ = nullptr;
    HBRUSH whiteBrush_ = nullptr;
    LauncherSettings settings_;
    UINT dpi_ = kDefaultDpi;
    bool loading_ = false;
};

bool HasArg(const std::vector<std::wstring>& args, const std::wstring& name) {
    return std::any_of(args.begin(), args.end(), [&](const auto& arg) { return _wcsicmp(arg.c_str(), name.c_str()) == 0; });
}

bool TryGetArgValue(const std::vector<std::wstring>& args, const std::wstring& name, std::wstring& value) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (_wcsicmp(args[i].c_str(), name.c_str()) == 0) {
            value = args[i + 1];
            return true;
        }
    }
    return false;
}

int GetEncoderClsid(const WCHAR* format, CLSID* clsid) {
    UINT count = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&count, &size);
    if (size == 0) {
        return -1;
    }
    std::vector<BYTE> buffer(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(count, size, encoders);
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, format) == 0) {
            *clsid = encoders[i].Clsid;
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SaveBitmapPng(HBITMAP bitmap, const std::wstring& path) {
    CLSID pngClsid{};
    if (GetEncoderClsid(L"image/png", &pngClsid) < 0) {
        return false;
    }
    Gdiplus::Bitmap image(bitmap, nullptr);
    return image.Save(path.c_str(), &pngClsid, nullptr) == Gdiplus::Ok;
}

int SaveMainSnapshot(HINSTANCE instance, const std::wstring& path) {
    MainWindow window(instance);
    if (!window.Create()) {
        return 1;
    }
    HWND hwnd = window.Hwnd();
    UINT dpi = GetWindowDpi(hwnd);
    SetWindowPos(hwnd, nullptr, -20000, -20000, ScaleForDpi(kWindowWidth, dpi), ScaleForDpi(kWindowHeight, dpi), SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    Sleep(200);

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    HDC windowDc = GetWindowDC(hwnd);
    HDC memDc = CreateCompatibleDC(windowDc);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDc, width, height);
    HGDIOBJ old = SelectObject(memDc, bitmap);
    BOOL printed = PrintWindow(hwnd, memDc, PW_RENDERFULLCONTENT);
    if (!printed) {
        BitBlt(memDc, 0, 0, width, height, windowDc, 0, 0, SRCCOPY);
    }
    SelectObject(memDc, old);
    DeleteDC(memDc);
    ReleaseDC(hwnd, windowDc);
    bool saved = SaveBitmapPng(bitmap, path);
    DeleteObject(bitmap);
    DestroyWindow(hwnd);
    return saved ? 0 : 1;
}

std::vector<std::wstring> GetArgs() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    LocalFree(argv);
    return args;
}

int RunCommandLine(HINSTANCE instance, const std::vector<std::wstring>& args) {
    auto settings = LoadSettings();
    if (HasArg(args, L"--self-test")) {
        std::wstring exe;
        std::wstring message;
        return TryResolveCodex(settings.codexExePath, exe, message) ? 0 : 2;
    }
    if (HasArg(args, L"--self-test-wsl-proxy")) {
        LauncherService service(settings, [](const std::wstring&) {});
        return service.TestWslProxyPropagation().success ? 0 : 4;
    }
    if (HasArg(args, L"--self-test-wsl-setup")) {
        LauncherService service(settings, [](const std::wstring&) {});
        auto result = service.TestWslProxySetup();
        fwprintf(stderr, L"%s\n", result.message.c_str());
        return result.success ? 0 : 6;
    }
    if (HasArg(args, L"--self-test-wsl-warning")) {
        LauncherService service(settings, [](const std::wstring&) {});
        auto result = service.TestWslProxyWarning();
        fwprintf(stderr, L"%d|%s|%s\n", result.showMessageBox ? 1 : 0, result.dialogTitle.c_str(), result.dialogMessage.c_str());
        return result.showMessageBox ? 0 : 7;
    }
    if (HasArg(args, L"--self-test-wsl-detection")) {
        std::wstring configPath;
        bool hasPath = TryGetArgValue(args, L"--codex-config", configPath);
        return DetectWslBackend(hasPath ? std::optional<std::wstring>(configPath) : std::nullopt).shouldApplyProxy ? 0 : 5;
    }
    if (HasArg(args, L"--start")) {
        LauncherService service(settings, [](const std::wstring&) {});
        return service.StartCodex().success ? 0 : 3;
    }
    if (HasArg(args, L"--snapshot-main")) {
        std::wstring path;
        if (!TryGetArgValue(args, L"--snapshot-main", path)) {
            return 1;
        }
        return SaveMainSnapshot(instance, path);
    }
    return -1;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    auto args = GetArgs();
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    int cli = RunCommandLine(instance, args);
    if (cli >= 0) {
        if (gdiplusToken != 0) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
        }
        CoUninitialize();
        return cli;
    }

    MainWindow window(instance);
    if (!window.Create()) {
        CoUninitialize();
        return 1;
    }
    int exitCode = window.Run();
    if (gdiplusToken != 0) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }
    CoUninitialize();
    return exitCode;
}
