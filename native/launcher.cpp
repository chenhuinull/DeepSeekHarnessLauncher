#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <algorithm>
#include <memory>
#include <optional>

using namespace Gdiplus;
namespace fs = std::filesystem;

static constexpr UINT WM_LOG = WM_APP + 1;
static constexpr UINT WM_WORK_DONE = WM_APP + 2;
static constexpr UINT WM_SERVER_STARTED = WM_APP + 3;
static constexpr UINT WM_SERVER_READY = WM_APP + 4;
static constexpr UINT WM_SERVER_EXIT = WM_APP + 5;
static constexpr UINT WM_SERVER_ADOPTED = WM_APP + 6;
static constexpr UINT WM_DOWNLOAD = WM_APP + 7;
static constexpr UINT WM_SERVER_UNHEALTHY = WM_APP + 8;
static constexpr UINT WM_SERVER_HEALTHY = WM_APP + 9;
static constexpr int H_COLLAPSED = 48, H_EXPANDED = 228;
// The whole button row is derived from these numbers, so the window width follows the
// button width instead of being a separate constant.
static constexpr int buttonWidth = 60, buttonHeight = 28, buttonTop = 10, buttonGap = 4;
// The status control is a lamp only, so it stays as narrow as a square indicator.
static constexpr int indicatorWidth = 28;
static constexpr int firstButtonX = 42;
static constexpr int statusX = firstButtonX;
static constexpr int startX = statusX + indicatorWidth + buttonGap;
static constexpr int logX = startX + buttonWidth + buttonGap;
static constexpr int updateX = logX + buttonWidth + buttonGap;
static constexpr int topmostX = updateX + buttonWidth + buttonGap;
static constexpr int lastButtonRight = topmostX + buttonWidth;
static constexpr int titleClusterGap = 8, titleButtonWidth = 25, rightMargin = 14;
static constexpr int titleClusterX = lastButtonRight + titleClusterGap;
static constexpr int W = titleClusterX + 2 * titleButtonWidth + rightMargin;
// Folded down to the whale icon and the status lamp; the rest of the chip drags.
static constexpr int W_MINI = statusX + indicatorWidth + rightMargin;
static constexpr USHORT serverPort = 3080;
static constexpr int maxLogLines = 50'000;
static constexpr int trimChunk = 500;
static constexpr UINT_PTR logHoverTimer = 1;
static constexpr UINT logHoverIntervalMs = 120;
static constexpr DWORD readyProbeIntervalMs = 1'000;
// After the service is up, keep asking the port whether it still answers: a process can
// stay alive while the web server inside it is gone.
static constexpr DWORD healthProbeIntervalMs = 5'000;
static constexpr int healthFailureThreshold = 3;
static constexpr float controlCornerRadius = 4.0f;   // buttons and the log box share this
static constexpr DWORD updateCheckTimeoutMs = 20'000;
// Versions this build is known to work with. Downloads stay reproducible and a
// newer dsh is only installed when the user asks for it.
static constexpr wchar_t pinnedNodeVersion[] = L"24.16.0";
static constexpr wchar_t pinnedDshVersion[] = L"0.1.5-rc.2";
enum class State { Running, Stopped, Missing, Update, Unresponsive };
enum class Work { Start, Check, InstallUpdate, Rollback };
enum class Button { None, Log, Status, Start, Update, Topmost, Minimize, Close };
struct Result { Work work; bool ok; std::wstring message; std::wstring version; std::wstring info; bool installed; bool update; };
struct ServerIdentity { DWORD pid = 0; unsigned long long created = 0; bool owned = true; };
struct UiRect { int x, y, w, h; bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; } };

static HWND windowHandle, logEdit;
static HBRUSH logBackground = nullptr;
static HICON appIcon;
static std::atomic_bool closing{false};
static std::mutex processMutex;
static HANDLE workJob = nullptr, serverJob = nullptr;
static ServerIdentity serverIdentity;
static fs::path runtimeDir;
static constexpr wchar_t serverJobName[] = L"Local\\DeepSeekHarnessLauncher.Server";
static bool expanded = false, topmost = true, busy = false, serverRunning = false;
static bool mini = false;
static bool systemCorners = false;
static bool stopping = false, updateAvailable = false, serverExternal = false;
static std::atomic_bool serverReady{false};
static State status = State::Missing;
static Button hoverButton = Button::None;
static std::wstring statusTip = L"未安装固件", updateLabel = L"检查更新", rollbackVersion;
static int logLineCount = 0;
static bool logHovered = false, logScrollBarShown = false;
static HWND tooltip;
static TOOLINFOW tipInfo{};
static float scaleFactor = 1.0f;

static const UiRect logRect{logX, buttonTop, buttonWidth, buttonHeight};
static const UiRect statusRect{statusX, buttonTop, indicatorWidth, buttonHeight};
static const UiRect startRect{startX, buttonTop, buttonWidth, buttonHeight};
static const UiRect updateRect{updateX, buttonTop, buttonWidth, buttonHeight};
static const UiRect topRect{topmostX, buttonTop, buttonWidth, buttonHeight};
static const UiRect minRect{titleClusterX, buttonTop, titleButtonWidth, buttonHeight};
static const UiRect closeRect{titleClusterX + titleButtonWidth, buttonTop, titleButtonWidth, buttonHeight};
// The whale icon doubles as the fold/unfold handle, so it is excluded from dragging.
static const UiRect iconRect{10, 8, 26, 32};

static std::wstring Utf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    UINT codepage = CP_UTF8;
    if (!n) { codepage = CP_ACP; n = MultiByteToWideChar(codepage, 0, s.data(), (int)s.size(), nullptr, 0); }
    std::wstring out(n, L'\0');
    MultiByteToWideChar(codepage, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

static std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

struct PortService { DWORD pid = 0; std::wstring image; bool harness = false; };

static DWORD PortOwnerPid(USHORT port) {
    DWORD size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
        return 0;
    std::vector<unsigned char> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return 0;
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if ((DWORD)ntohs((u_short)row.dwLocalPort) != port) continue;
        if (row.dwLocalAddr != htonl(INADDR_LOOPBACK) && row.dwLocalAddr != 0) continue;
        return row.dwOwningPid;
    }
    return 0;
}

// The DSH web server answers an unauthenticated request with "dsh web authentication required".
static bool ProbeHarness(USHORT port) {
    HINTERNET session = WinHttpOpen(L"DeepSeekHarnessLauncher/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 600, 600, 1200, 1200);
    bool result = false;
    if (HINTERNET connect = WinHttpConnect(session, L"127.0.0.1", port, 0)) {
        if (HINTERNET request = WinHttpOpenRequest(connect, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, 0)) {
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                std::string body;
                DWORD available = 0;
                while (body.size() < 8192 && WinHttpQueryDataAvailable(request, &available) && available) {
                    std::string chunk(available, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) break;
                    body.append(chunk.data(), read);
                }
                std::string lower;
                lower.reserve(body.size());
                for (char c : body) lower += (char)tolower((unsigned char)c);
                result = lower.find("dsh web") != std::string::npos ||
                    lower.find("deepseek harness") != std::string::npos;
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return result;
}

static std::optional<PortService> InspectPort(USHORT port) {
    DWORD pid = PortOwnerPid(port);
    if (!pid) return std::nullopt;
    PortService service;
    service.pid = pid;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 4];
        DWORD length = (DWORD)std::size(path);
        if (QueryFullProcessImageNameW(process, 0, path, &length)) service.image.assign(path, length);
        CloseHandle(process);
    }
    service.harness = ProbeHarness(port);
    return service;
}

static std::wstring WinError(DWORD code = GetLastError()) {
    wchar_t* ptr = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, (LPWSTR)&ptr, 0, nullptr);
    std::wstring result = ptr ? ptr : L"未知错误";
    if (ptr) LocalFree(ptr);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}

static fs::path LocalAppData() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) return fs::temp_directory_path();
    fs::path path(raw); CoTaskMemFree(raw); return path;
}

static fs::path EntryPoint() { return runtimeDir / L"node_modules" / L"@deepseek-ai" / L"dsh" / L"lib" / L"bin.js"; }
static fs::path PackageJson() { return runtimeDir / L"node_modules" / L"@deepseek-ai" / L"dsh" / L"package.json"; }
static fs::path ServerPidFile() { return runtimeDir.parent_path() / L"server.pid"; }
static fs::path ServerLogFile() { return runtimeDir.parent_path() / L"server.log"; }
static fs::path LocalNodeRoot() { return runtimeDir.parent_path() / L"tools" / L"node"; }
static bool Installed() { return fs::exists(EntryPoint()); }

static std::wstring Version() {
    std::ifstream file(PackageJson(), std::ios::binary);
    if (!file) return {};
    std::string json((std::istreambuf_iterator<char>(file)), {});
    size_t key = json.find("\"version\"");
    if (key == std::string::npos) return {};
    size_t colon = json.find(':', key + 9), first = json.find('"', colon);
    if (colon == std::string::npos || first == std::string::npos) return {};
    size_t last = json.find('"', first + 1);
    return last == std::string::npos ? L"" : Utf8(json.substr(first + 1, last - first - 1));
}

static std::wstring FindOnPath(const wchar_t* name) {
    DWORD len = SearchPathW(nullptr, name, nullptr, 0, nullptr, nullptr);
    if (!len) return {};
    std::wstring path(len, L'\0');
    DWORD used = SearchPathW(nullptr, name, nullptr, len, path.data(), nullptr);
    if (!used || used >= len) return {};
    path.resize(used);
    return path;
}

struct NodeTools { std::wstring node, npm; };
static std::optional<NodeTools> CompleteNode(const fs::path& root) {
    fs::path node = root / L"node.exe";
    fs::path npm = root / L"node_modules" / L"npm" / L"bin" / L"npm-cli.js";
    if (fs::is_regular_file(node) && fs::is_regular_file(npm)) return NodeTools{node.wstring(), npm.wstring()};
    return std::nullopt;
}

static void PrependLocalNodeToPath() {
    std::wstring root = LocalNodeRoot().wstring();
    DWORD length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring path(length ? length : 1, L'\0');
    if (length) { DWORD used = GetEnvironmentVariableW(L"PATH", path.data(), length); path.resize(used); }
    if (path.rfind(root + L";", 0) != 0) SetEnvironmentVariableW(L"PATH", (root + L";" + path).c_str());
}

static std::wstring Quote(const std::wstring& s) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\'); out += L'\"';
    return out;
}

static void PostLog(const std::wstring& line) {
    if (closing) return;
    auto* copy = new std::wstring(line);
    if (!PostMessageW(windowHandle, WM_LOG, 0, (LPARAM)copy)) delete copy;
}

static void SetJob(HANDLE& slot, HANDLE job) { std::lock_guard lock(processMutex); slot = job; }
static void KillJob(HANDLE& slot) { std::lock_guard lock(processMutex); if (slot) TerminateJobObject(slot, 1); }

static DWORD RunNode(const std::wstring& node, const std::vector<std::wstring>& args,
    std::wstring* output = nullptr, DWORD timeoutMs = INFINITE) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) throw std::runtime_error("pipe");
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullInput; si.hStdOutput = writePipe; si.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = Quote(node);
    for (const auto& arg : args) { cmd += L" "; cmd += Quote(arg); }
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end()); mutableCmd.push_back(0);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
    fs::path home = fs::path(LocalAppData()).parent_path();
    BOOL started = CreateProcessW(node.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, home.c_str(), &si, &pi);
    DWORD launchError = GetLastError();
    CloseHandle(writePipe); if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    if (!started) { CloseHandle(readPipe); CloseHandle(job); throw std::runtime_error("launch: " + std::to_string(launchError)); }
    bool assignedToJob = !!AssignProcessToJobObject(job, pi.hProcess);
    SetJob(workJob, job);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    ULONGLONG startedAt = GetTickCount64();
    bool timedOut = false;
    std::string pending;
    char chunk[4096]; DWORD got = 0;
    auto emit = [&](const std::string& raw) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return;
        std::wstring wide = Utf8(line);
        if (output) { *output += wide; *output += L'\n'; }
        PostLog(wide);
    };
    while (true) {
        if (timeoutMs != INFINITE && GetTickCount64() - startedAt >= timeoutMs) {
            timedOut = true;
            if (!assignedToJob || !TerminateJobObject(job, ERROR_TIMEOUT))
                TerminateProcess(pi.hProcess, ERROR_TIMEOUT);
            break;
        }
        DWORD available = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            DWORD toRead = std::min<DWORD>(available, sizeof(chunk));
            if (ReadFile(readPipe, chunk, toRead, &got, nullptr) && got) {
                pending.append(chunk, got);
                size_t split;
                while ((split = pending.find('\n')) != std::string::npos) {
                    emit(pending.substr(0, split)); pending.erase(0, split + 1);
                }
                if (pending.size() > 65536) { emit(pending); pending.clear(); }
                continue;
            }
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
        Sleep(50);
    }
    if (!pending.empty()) emit(pending);
    CloseHandle(readPipe);
    DWORD wait = WaitForSingleObject(pi.hProcess, timedOut ? 5000 : INFINITE);
    if (wait == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, ERROR_TIMEOUT); WaitForSingleObject(pi.hProcess, 5000); }
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    { std::lock_guard lock(processMutex); workJob = nullptr; }
    CloseHandle(job);
    if (timedOut) throw std::runtime_error("update timeout");
    return code;
}

struct HttpSession {
    HINTERNET session = nullptr, connect = nullptr, request = nullptr;
    unsigned long long length = 0;
};

static void CloseHttp(HttpSession& http) {
    if (http.request) WinHttpCloseHandle(http.request);
    if (http.connect) WinHttpCloseHandle(http.connect);
    if (http.session) WinHttpCloseHandle(http.session);
    http = {};
}

// Open a GET request. An empty proxy connects directly; otherwise the proxy is used
// explicitly, so a local proxy keeps working even when the system proxy switch is off.
static bool BeginHttpGet(const std::wstring& url, const std::wstring& proxy, HttpSession& http, std::wstring& error) {
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &parts)) { error = L"URL 无法解析"; return false; }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    http.session = WinHttpOpen(L"DeepSeekHarnessLauncher/1.0",
        proxy.empty() ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    if (!http.session) { error = L"无法初始化 WinHTTP"; return false; }
    WinHttpSetTimeouts(http.session, 10'000, 10'000, 20'000, 30'000);
    http.connect = WinHttpConnect(http.session, host.c_str(), parts.nPort, 0);
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    http.request = http.connect
        ? WinHttpOpenRequest(http.connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, flags)
        : nullptr;
    if (!http.request) { error = L"无法建立 HTTP 请求"; CloseHttp(http); return false; }
    if (!WinHttpSendRequest(http.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(http.request, nullptr)) {
        error = L"请求发送失败"; CloseHttp(http); return false;
    }
    DWORD statusCode = 0, size = sizeof(statusCode);
    WinHttpQueryHeaders(http.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200) { error = L"HTTP " + std::to_wstring(statusCode); CloseHttp(http); return false; }
    DWORD contentLength = 0; size = sizeof(contentLength);
    if (WinHttpQueryHeaders(http.request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &size, WINHTTP_NO_HEADER_INDEX))
        http.length = contentLength;
    return true;
}

template <typename Sink>
static bool ReadHttpBody(HttpSession& http, Sink&& sink) {
    char buffer[65536]; DWORD available = 0;
    while (WinHttpQueryDataAvailable(http.request, &available) && available) {
        DWORD want = available > sizeof(buffer) ? (DWORD)sizeof(buffer) : available, got = 0;
        if (!WinHttpReadData(http.request, buffer, want, &got) || !got) return false;
        if (!sink(buffer, got)) return false;
    }
    return true;
}

static std::wstring FetchText(const std::wstring& url, const std::wstring& proxy, std::wstring& error) {
    HttpSession http;
    if (!BeginHttpGet(url, proxy, http, error)) return {};
    std::string body;
    bool ok = ReadHttpBody(http, [&](const char* data, DWORD size) {
        body.append(data, size);
        return body.size() <= 1'000'000;
    });
    CloseHttp(http);
    if (!ok) { error = L"读取响应失败"; return {}; }
    return Utf8(body);
}

static bool DownloadFile(const std::wstring& url, const std::wstring& proxy, const fs::path& destination,
    const std::function<void(unsigned long long, unsigned long long)>& progress, std::wstring& error) {
    HttpSession http;
    if (!BeginHttpGet(url, proxy, http, error)) return false;
    std::ofstream file(destination, std::ios::binary | std::ios::trunc);
    if (!file) { CloseHttp(http); error = L"无法写入下载文件"; return false; }
    unsigned long long done = 0;
    bool ok = ReadHttpBody(http, [&](const char* data, DWORD size) {
        file.write(data, size);
        done += size;
        if (!file) return false;
        progress(done, http.length);
        return true;
    });
    file.flush();
    bool good = file.good();
    CloseHttp(http);
    if (!ok || !good) { error = L"下载中断"; return false; }
    return true;
}

static std::wstring Megabytes(unsigned long long bytes) {
    wchar_t text[32];
    wsprintfW(text, L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
    return text;
}

static std::wstring Lowercase(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return text;
}

// Proxy candidates: an explicit environment variable first, then the system proxy when
// it is switched on, then a direct connection. Stage 1 retries the address saved in the
// system settings even if it is currently switched off, which is what makes a stopped
// local proxy (for example 127.0.0.1:7897) still usable as a last resort.
static std::wstring EnvironmentProxy() {
    for (const wchar_t* name : {L"HTTPS_PROXY", L"https_proxy", L"HTTP_PROXY", L"http_proxy", L"ALL_PROXY", L"all_proxy"}) {
        DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
        if (!length) continue;
        std::wstring value(length, L'\0');
        DWORD used = GetEnvironmentVariableW(name, value.data(), length);
        value.resize(used);
        if (value.rfind(L"http://", 0) == 0) value.erase(0, 7);
        else if (value.rfind(L"https://", 0) == 0) value.erase(0, 8);
        while (!value.empty() && value.back() == L'/') value.pop_back();
        if (!value.empty()) return value;
    }
    return {};
}

static std::wstring SystemProxy(bool& enabled) {
    enabled = false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
        0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return {};
    wchar_t text[512]{}; DWORD size = sizeof(text), type = 0;
    std::wstring server;
    if (RegQueryValueExW(key, L"ProxyServer", nullptr, &type, (LPBYTE)text, &size) == ERROR_SUCCESS && type == REG_SZ)
        server = text;
    DWORD flag = 0; size = sizeof(flag);
    if (RegQueryValueExW(key, L"ProxyEnable", nullptr, &type, (LPBYTE)&flag, &size) == ERROR_SUCCESS && type == REG_DWORD)
        enabled = flag != 0;
    RegCloseKey(key);
    for (const wchar_t* scheme : {L"https=", L"http="}) {
        size_t start = server.find(scheme);
        if (start == std::wstring::npos) continue;
        size_t end = server.find(L';', start);
        server = server.substr(start + 6, end == std::wstring::npos ? std::wstring::npos : end - start - 6);
        break;
    }
    while (!server.empty() && (server.back() == L';' || server.back() == L' ')) server.pop_back();
    return server;
}

static std::vector<std::wstring> ProxyCandidates(int stage) {
    std::vector<std::wstring> proxies;
    std::wstring fromEnvironment = EnvironmentProxy();
    bool enabled = false;
    std::wstring recorded = SystemProxy(enabled);
    if (stage == 0) {
        if (!fromEnvironment.empty()) proxies.push_back(fromEnvironment);
        if (enabled && !recorded.empty()) proxies.push_back(recorded);
        proxies.push_back(L"");
    } else if (!enabled && !recorded.empty() && recorded != fromEnvironment) {
        proxies.push_back(recorded);
    }
    return proxies;
}

static std::vector<std::wstring> NodeSourceBases() {
    std::wstring tag = L"v" + std::wstring(pinnedNodeVersion);
    return {
        L"https://nodejs.org/dist/" + tag,
        L"https://mirrors.tuna.tsinghua.edu.cn/nodejs-release/" + tag,
        L"https://registry.npmmirror.com/-/binary/node/" + tag,
    };
}

static std::wstring ChecksumFor(const std::wstring& manifest, const std::wstring& fileName) {
    std::wstring wanted = Lowercase(fileName);
    size_t position = 0;
    while (position < manifest.size()) {
        size_t end = manifest.find(L'\n', position);
        std::wstring line = manifest.substr(position, end == std::wstring::npos ? std::wstring::npos : end - position);
        position = end == std::wstring::npos ? manifest.size() : end + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        std::wstring lower = Lowercase(line);
        if (lower.size() < wanted.size() ||
            lower.compare(lower.size() - wanted.size(), wanted.size(), wanted) != 0) continue;
        size_t split = lower.find_first_of(L" \t");
        if (split == std::wstring::npos || split < 64) continue;
        return lower.substr(0, 64);
    }
    return {};
}

struct NodeSource { std::wstring proxy; std::wstring base; };

static bool SelectNodeSource(const std::wstring& fileName, NodeSource& chosen, std::wstring& manifest) {
    std::vector<std::wstring> bases = NodeSourceBases();
    for (int stage = 0; stage < 2; stage++) {
        for (const auto& proxy : ProxyCandidates(stage)) {
            for (const auto& base : bases) {
                std::wstring error;
                std::wstring text = FetchText(base + L"/SHASUMS256.txt", proxy, error);
                if (text.empty() || ChecksumFor(text, fileName).empty()) {
                    PostLog(std::wstring(L"下载源不可用（") + (proxy.empty() ? L"直连" : L"代理 " + proxy) + L"）：" +
                        base + L" · " + (error.empty() ? L"缺少校验值" : error));
                    continue;
                }
                chosen = NodeSource{proxy, base};
                manifest = text;
                return true;
            }
        }
    }
    return false;
}

static bool Sha256File(const fs::path& path, std::string& hex) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return false;
    DWORD objectSize = 0, hashSize = 0, written = 0;
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objectSize, sizeof(objectSize), &written, 0);
    BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, (PUCHAR)&hashSize, sizeof(hashSize), &written, 0);
    std::vector<unsigned char> object(objectSize), digest(hashSize);
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = objectSize && hashSize &&
        BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0));
    std::ifstream file(path, std::ios::binary);
    if (!file) ok = false;
    std::vector<char> buffer(1 << 16);
    while (ok && file) {
        file.read(buffer.data(), (std::streamsize)buffer.size());
        std::streamsize got = file.gcount();
        if (got > 0 && !BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)buffer.data(), (ULONG)got, 0))) ok = false;
    }
    if (ok && !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), hashSize, 0))) ok = false;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return false;
    static const char digits[] = "0123456789abcdef";
    hex.clear();
    for (unsigned char byte : digest) { hex += digits[byte >> 4]; hex += digits[byte & 15]; }
    return true;
}

static void ExtractArchive(const fs::path& archive, const fs::path& destination) {
    std::wstring tar = FindOnPath(L"tar.exe");
    if (tar.empty()) throw std::runtime_error("tar missing");
    fs::create_directories(destination);
    DWORD code = RunNode(tar, {L"-xf", archive.wstring(), L"-C", destination.wstring()});
    if (code != 0) throw std::runtime_error("extract failed: " + std::to_string(code));
}

static std::wstring FileVersionString(const fs::path& file) {
    DWORD size = GetFileVersionInfoSizeW(file.c_str(), nullptr);
    if (!size) return {};
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr; UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", (LPVOID*)&info, &length) || !info) return {};
    wchar_t text[64];
    wsprintfW(text, L"%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
        HIWORD(info->dwFileVersionLS));
    return text;
}

static void InstallLocalNode() {
    std::wstring version = pinnedNodeVersion;
    std::wstring folder = L"node-v" + version + L"-win-x64";
    std::wstring fileName = folder + L".zip";
    fs::path toolsDir = LocalNodeRoot().parent_path();
    fs::create_directories(toolsDir);
    fs::path staging = toolsDir / (L".node-download-" + std::to_wstring(GetTickCount64()) + L"-" +
        std::to_wstring(GetCurrentProcessId()));
    fs::path archive = staging / L"node.zip";
    fs::path expanded = staging / L"expanded";
    try {
        fs::create_directories(staging);
        PostLog(L"本机缺少 Node.js 与 npm，开始下载 Node.js v" + version + L"（启动器内置的已知可用版本）。");
        NodeSource source; std::wstring manifest;
        if (!SelectNodeSource(fileName, source, manifest)) throw std::runtime_error("node download failed");
        PostLog(L"下载源：" + source.base + (source.proxy.empty() ? L"（直连）" : L"（代理 " + source.proxy + L"）"));
        std::wstring expected = ChecksumFor(manifest, fileName);
        if (!closing) PostMessageW(windowHandle, WM_DOWNLOAD, 0, 0);
        int lastTenth = -1, lastPosted = -1;
        std::wstring error;
        bool downloaded = DownloadFile(source.base + L"/" + fileName, source.proxy, archive,
            [&](unsigned long long done, unsigned long long total) {
                int percent = total ? (int)(done * 100 / total) : 0;
                if (percent / 10 != lastTenth) {
                    lastTenth = percent / 10;
                    PostLog(L"下载进度 " + std::to_wstring(percent) + L"% (" + Megabytes(done) + L" / " +
                        (total ? Megabytes(total) : std::wstring(L"未知大小")) + L")");
                }
                if (percent - lastPosted >= 5) {
                    lastPosted = percent;
                    if (!closing) PostMessageW(windowHandle, WM_DOWNLOAD, (WPARAM)percent, 0);
                }
            }, error);
        if (!downloaded) throw std::runtime_error("node download failed");
        PostLog(L"校验 SHA-256 …");
        std::string actual;
        if (!Sha256File(archive, actual) || Lowercase(Utf8(actual)) != expected)
            throw std::runtime_error("checksum mismatch");
        ExtractArchive(archive, expanded);
        fs::path extracted = expanded / folder;
        if (!CompleteNode(extracted)) throw std::runtime_error("archive incomplete");
        if (fs::exists(LocalNodeRoot())) fs::remove_all(LocalNodeRoot());
        fs::rename(extracted, LocalNodeRoot());
        PostLog(L"Node.js v" + version + L" 已就绪。");
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        throw;
    }
    std::error_code ignored;
    fs::remove_all(staging, ignored);
}

static std::wstring ExceptionMessage(const std::exception& e);

static std::optional<NodeTools> SystemNode() {
    std::wstring systemNode = FindOnPath(L"node.exe");
    if (systemNode.empty()) return std::nullopt;
    fs::path root = fs::path(systemNode).parent_path();
    if (auto system = CompleteNode(root)) {
        PostLog(L"使用系统 Node.js 和 npm。");
        return system;
    }
    std::wstring npmCmd = FindOnPath(L"npm.cmd");
    if (!npmCmd.empty()) {
        fs::path npm = fs::path(npmCmd).parent_path() / L"node_modules" / L"npm" / L"bin" / L"npm-cli.js";
        if (fs::is_regular_file(npm)) {
            PostLog(L"使用系统 Node.js 和 npm。");
            return NodeTools{systemNode, npm.wstring()};
        }
    }
    return std::nullopt;
}

static std::optional<NodeTools> FindNodeWithoutDownload() {
    if (auto local = CompleteNode(LocalNodeRoot())) {
        PrependLocalNodeToPath();
        PostLog(L"使用本地 Node.js 和 npm。");
        return local;
    }
    return SystemNode();
}

static NodeTools FindNode() {
    if (auto local = CompleteNode(LocalNodeRoot())) {
        std::wstring version = FileVersionString(LocalNodeRoot() / L"node.exe");
        if (version.empty() || version == pinnedNodeVersion) {
            PrependLocalNodeToPath();
            PostLog(version.empty() ? L"使用本地 Node.js 和 npm。" : L"使用本地 Node.js v" + version + L" 和 npm。");
            return *local;
        }
        // The launcher ships a pinned runtime. Refreshing the private copy keeps the pin
        // meaningful after an upgrade, but a network failure must not break a working copy.
        PostLog(L"本地 Node.js v" + version + L" 与启动器内置的 v" + pinnedNodeVersion + L" 不一致，正在更新运行时…");
        try {
            InstallLocalNode();
        } catch (const std::exception& e) {
            PostLog(L"更新 Node.js 失败：" + ExceptionMessage(e) + L"；继续使用本地 v" + version + L"。");
        }
        if (auto refreshed = CompleteNode(LocalNodeRoot())) {
            PrependLocalNodeToPath();
            return *refreshed;
        }
        if (auto system = SystemNode()) return *system;
        throw std::runtime_error("node download failed");
    }
    if (auto system = SystemNode()) return *system;
    InstallLocalNode();
    auto local = CompleteNode(LocalNodeRoot());
    if (!local) throw std::runtime_error("node download failed");
    PrependLocalNodeToPath();
    return *local;
}

static unsigned long long CreationTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return value.QuadPart;
}

static std::optional<ServerIdentity> ReadServerIdentity() {
    std::ifstream file(ServerPidFile());
    ServerIdentity id;
    if (!(file >> id.pid >> id.created) || !id.pid || !id.created) return std::nullopt;
    int owned = 1;  // Older records only stored pid and creation time: they were started by a launcher.
    file >> owned;
    id.owned = owned != 0;
    return id;
}

static void SaveServerIdentity(ServerIdentity id) {
    fs::create_directories(ServerPidFile().parent_path());
    std::ofstream file(ServerPidFile(), std::ios::trunc);
    file << id.pid << ' ' << id.created << ' ' << (id.owned ? 1 : 0) << '\n';
    file.flush();
    if (!file) throw std::runtime_error("pid file");
}

static void ClearServerIdentity(ServerIdentity id) {
    auto saved = ReadServerIdentity();
    if (saved && saved->pid == id.pid && saved->created == id.created) {
        std::error_code error;
        fs::remove(ServerPidFile(), error);
    }
}

static HANDLE OpenLiveServer(ServerIdentity id) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, id.pid);
    if (!process) return nullptr;
    if (CreationTime(process) != id.created || WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
        CloseHandle(process);
        return nullptr;
    }
    return process;
}

// Kills a process and everything below it. Walking the tree is used instead of relying on
// the job object alone, because a launcher that was itself started from inside the
// service's process tree is a member of that job: TerminateJobObject would then kill the
// launcher too, which looks exactly like a crash.
static bool TerminateProcessTree(DWORD pid) {
    if (!pid || pid == GetCurrentProcessId()) return false;
    std::vector<std::pair<DWORD, DWORD>> links;
    if (HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do { links.emplace_back(entry.th32ParentProcessID, entry.th32ProcessID); }
            while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    std::vector<DWORD> descendants, pending{pid};
    while (!pending.empty()) {
        DWORD parent = pending.back(); pending.pop_back();
        for (const auto& link : links) {
            DWORD child = link.second;
            if (link.first != parent || child == GetCurrentProcessId()) continue;
            if (std::find(descendants.begin(), descendants.end(), child) != descendants.end()) continue;
            if (std::find(pending.begin(), pending.end(), child) != pending.end()) continue;
            descendants.push_back(child);
            pending.push_back(child);
        }
    }
    for (auto it = descendants.rbegin(); it != descendants.rend(); ++it) {
        if (HANDLE child = OpenProcess(PROCESS_TERMINATE, FALSE, *it)) {
            TerminateProcess(child, 1);
            CloseHandle(child);
        }
    }
    bool killed = false;
    if (HANDLE root = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
        killed = !!TerminateProcess(root, 1);
        CloseHandle(root);
    }
    return killed;
}

static bool TerminateServer(ServerIdentity id) {
    bool useJob = false;
    {
        std::lock_guard lock(processMutex);
        BOOL selfInJob = FALSE;
        useJob = serverJob && !serverExternal &&
            IsProcessInJob(GetCurrentProcess(), serverJob, &selfInJob) && !selfInJob;
        if (useJob) return !!TerminateJobObject(serverJob, 1);
    }
    HANDLE live = OpenLiveServer(id);   // refuse to kill a recycled pid
    if (!live) return false;
    CloseHandle(live);
    return TerminateProcessTree(id.pid);
}

static void ReadServerLog(unsigned long long& offset, std::string& pending) {
    HANDLE file = CreateFileW(ServerLogFile().c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER position{}; position.QuadPart = offset;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) { CloseHandle(file); return; }
    char chunk[4096]; DWORD got = 0;
    unsigned int bytesThisPass = 0;
    while (bytesThisPass < 65536 && ReadFile(file, chunk, sizeof(chunk), &got, nullptr) && got) {
        offset += got; bytesThisPass += got;
        pending.append(chunk, got);
        size_t split;
        while ((split = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, split);
            pending.erase(0, split + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            PostLog(Utf8(line));
            if (line.find("dsh web: http") != std::string::npos && !closing && !serverReady)
                PostMessageW(windowHandle, WM_SERVER_READY, 0, 0);
        }
        if (pending.size() > 65536) { PostLog(Utf8(pending)); pending.clear(); }
    }
    CloseHandle(file);
}

static DWORD MonitorServer(HANDLE process, HANDLE job, ServerIdentity id, bool attached) {
    unsigned long long offset = 0;
    if (attached) {
        std::error_code error;
        auto size = fs::file_size(ServerLogFile(), error);
        if (!error && size > 16384) offset = size - 16384;
    }
    std::string pending;
    bool exited = false;
    ULONGLONG nextProbe = GetTickCount64() + readyProbeIntervalMs;
    ULONGLONG nextHealth = GetTickCount64() + healthProbeIntervalMs;
    int healthFailures = 0;
    bool unhealthy = false;
    while (!closing) {
        ReadServerLog(offset, pending);
        // The log line is the fast path; probing the port keeps "ready" working
        // even when dsh changes what it prints.
        if (!serverReady && GetTickCount64() >= nextProbe) {
            nextProbe = GetTickCount64() + readyProbeIntervalMs;
            if (ProbeHarness(serverPort)) {
                serverReady = true;
                if (!closing) PostMessageW(windowHandle, WM_SERVER_READY, 0, 0);
            }
        } else if (serverReady && GetTickCount64() >= nextHealth) {
            // The process handle says nothing about the web server inside it, so keep
            // asking the port and report when it stops answering.
            nextHealth = GetTickCount64() + healthProbeIntervalMs;
            if (ProbeHarness(serverPort)) {
                healthFailures = 0;
                if (unhealthy) {
                    unhealthy = false;
                    if (!closing) PostMessageW(windowHandle, WM_SERVER_HEALTHY, 0, 0);
                }
            } else if (++healthFailures >= healthFailureThreshold && !unhealthy) {
                unhealthy = true;
                bool listening = PortOwnerPid(serverPort) != 0;
                if (!closing) PostMessageW(windowHandle, WM_SERVER_UNHEALTHY, listening ? 1 : 0, 0);
            }
        }
        if (WaitForSingleObject(process, 250) == WAIT_OBJECT_0) { exited = true; break; }
    }
    if (exited) ReadServerLog(offset, pending);
    DWORD code = 0;
    if (exited) GetExitCodeProcess(process, &code);
    if (exited) ClearServerIdentity(id);
    {
        std::lock_guard lock(processMutex);
        if (serverJob == job) serverJob = nullptr;
        if (serverIdentity.pid == id.pid && serverIdentity.created == id.created && exited)
            serverIdentity = {};
    }
    CloseHandle(process);
    if (job) CloseHandle(job);
    return code;
}

static DWORD RunServer(const std::wstring& node) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE logFile = CreateFileW(ServerLogFile().c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logFile == INVALID_HANDLE_VALUE) throw std::runtime_error("server log");
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullInput; si.hStdOutput = logFile; si.hStdError = logFile;
    std::wstring cmd = Quote(node) + L" " + Quote(EntryPoint().wstring()) + L" web --no-open";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end()); mutableCmd.push_back(0);
    HANDLE job = CreateJobObjectW(nullptr, serverJobName);
    PROCESS_INFORMATION pi{};
    fs::path home = fs::path(LocalAppData()).parent_path();
    BOOL started = CreateProcessW(node.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, home.c_str(), &si, &pi);
    DWORD launchError = GetLastError();
    CloseHandle(logFile); if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    if (!started) {
        if (job) CloseHandle(job);
        throw std::runtime_error("launch: " + std::to_string(launchError));
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) { CloseHandle(job); job = nullptr; }
    ServerIdentity id{pi.dwProcessId, CreationTime(pi.hProcess), true};
    try { SaveServerIdentity(id); }
    catch (...) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (job) CloseHandle(job);
        throw;
    }
    {
        std::lock_guard lock(processMutex);
        serverJob = job;
        serverIdentity = id;
    }
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    serverExternal = false;
    if (!closing) PostMessageW(windowHandle, WM_SERVER_STARTED, 0, 0);
    return MonitorServer(pi.hProcess, job, id, false);
}

static bool AttachRunningServer() {
    auto saved = ReadServerIdentity();
    if (!saved) return false;
    HANDLE process = OpenLiveServer(*saved);
    if (!process) { ClearServerIdentity(*saved); return false; }
    HANDLE job = OpenJobObjectW(JOB_OBJECT_TERMINATE | JOB_OBJECT_QUERY, FALSE, serverJobName);
    {
        std::lock_guard lock(processMutex);
        serverJob = job;
        serverIdentity = *saved;
    }
    serverRunning = true;
    serverExternal = !saved->owned;
    serverReady = true;
    std::thread([process, job, id = *saved] {
        DWORD code = MonitorServer(process, job, id, true);
        if (!closing) PostMessageW(windowHandle, WM_SERVER_EXIT, code, 0);
    }).detach();
    return true;
}

// Adopt a DSH web server this launcher did not start (for example one started with
// npx in a terminal). It is identified by the listening socket on serverPort, so a
// missing or stale pid file no longer hides a live service.
static bool AdoptRunningService() {
    auto service = InspectPort(serverPort);
    if (!service || !service->harness) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
        FALSE, service->pid);
    if (!process) return false;
    ServerIdentity id{service->pid, CreationTime(process), false};
    if (!id.created) { CloseHandle(process); return false; }
    HANDLE job = OpenJobObjectW(JOB_OBJECT_TERMINATE | JOB_OBJECT_QUERY, FALSE, serverJobName);
    {
        std::lock_guard lock(processMutex);
        serverJob = job;
        serverIdentity = id;
    }
    try { SaveServerIdentity(id); } catch (...) {}
    serverRunning = true;
    serverReady = true;
    serverExternal = true;
    std::wstring image = service->image.empty() ? L"未知进程" : fs::path(service->image).filename().wstring();
    PostLog(L"检测到已在运行的 DeepSeek Harness Web 服务（PID " + std::to_wstring(id.pid) + L"，" + image +
        L"），已接管；点击“停止”可结束它。");
    std::thread([process, job, id] {
        DWORD code = MonitorServer(process, job, id, true);
        if (!closing) PostMessageW(windowHandle, WM_SERVER_EXIT, code, 0);
    }).detach();
    if (!closing) PostMessageW(windowHandle, WM_SERVER_ADOPTED, (WPARAM)id.pid, 0);
    return true;
}

static void InstallHarness(const NodeTools& tools, const std::wstring& version) {
    fs::create_directories(runtimeDir);
    std::wstring spec = L"@deepseek-ai/dsh@" + version;
    PostLog(L"安装 " + spec + L" …");
    DWORD code = RunNode(tools.node,
        {tools.npm, L"install", L"--prefix", runtimeDir.wstring(), L"--no-audit", L"--no-fund", spec});
    if (code != 0 || !Installed()) throw std::runtime_error("install failed: " + std::to_string(code));
    PostLog(L"安装完成，版本 " + Version() + L"。");
}

// The version that last reached "service ready" is remembered, so a broken update can
// be rolled back without guessing.
static fs::path KnownGoodFile() { return runtimeDir.parent_path() / L"known-good.txt"; }

static std::wstring KnownGoodVersion() {
    std::ifstream file(KnownGoodFile(), std::ios::binary);
    std::string text;
    std::getline(file, text);
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return Utf8(text);
}

static void SaveKnownGoodVersion(const std::wstring& version) {
    if (version.empty()) return;
    std::ofstream file(KnownGoodFile(), std::ios::trunc);
    file << Narrow(version) << '\n';
    file.flush();
}

static std::wstring ExceptionMessage(const std::exception& e) {
    std::string what = e.what();
    if (what == "node download failed") return L"下载 Node.js 失败。请检查网络或代理后重试，也可自行安装 Node.js。";
    if (what == "checksum mismatch") return L"Node.js 压缩包 SHA-256 校验失败，本次下载已删除。";
    if (what == "archive incomplete") return L"Node.js 压缩包内容不完整，本次下载已删除。";
    if (what == "tar missing") return L"系统缺少 tar.exe，无法解压 Node.js 压缩包（需要 Windows 10 1803 及以上）。";
    if (what == "pipe") return L"无法建立子进程输出管道。";
    if (what == "rollback missing") return L"没有可回退的版本记录。";
    if (what == "update timeout") return L"检查更新超过 " + std::to_wstring(updateCheckTimeoutMs / 1000) + L" 秒，已停止检查。";
    if (what.rfind("extract failed: ", 0) == 0) return L"解压 Node.js 压缩包失败（退出码 " + Utf8(what.substr(16)) + L"）。";
    if (what.rfind("install failed:", 0) == 0) return L"安装失败（退出码 " + Utf8(what.substr(16)) + L"）。请查看运行日志。";
    if (what.rfind("port busy: ", 0) == 0) return L"本地端口 " + std::to_wstring((int)serverPort) +
        L" 已被其它程序占用（" + Utf8(what.substr(11)) + L"），已取消启动；请先结束占用该端口的程序。";
    return L"操作失败：" + Utf8(what);
}

static void Worker(Work work) {
    // Never race a live web service: adopt it instead of starting a second one that
    // would only die with EADDRINUSE.
    if (work == Work::Start && AdoptRunningService()) return;
    auto result = std::make_unique<Result>(); result->work = work; result->ok = false;
    auto finish = [&]() -> bool {
        if (!closing && !PostMessageW(windowHandle, WM_WORK_DONE, 0, (LPARAM)result.get())) return false;
        result.release();
        return true;
    };
    try {
        if (work == Work::Check) {
            // A version check must never pull tens of megabytes of runtime.
            auto existing = FindNodeWithoutDownload();
            if (!existing) {
                result->installed = Installed();
                result->info = result->installed
                    ? L"未找到 Node.js 与 npm，暂时无法查询 npm 上的最新版本。"
                    : L"本机尚未安装 Node.js 与 DeepSeek Harness；点击“启动”会自动下载并安装。";
                finish();
                return;
            }
            PostLog(L"正在检查 npm 上的最新版本…");
            std::wstring output;
            DWORD code = RunNode(existing->node, {existing->npm, L"view", L"@deepseek-ai/dsh", L"version", L"--json"},
                &output, updateCheckTimeoutMs);
            if (code != 0) throw std::runtime_error("check failed");
            size_t first = output.find(L'\"'), last = output.find(L'\"', first + 1);
            if (first == std::wstring::npos || last == std::wstring::npos) throw std::runtime_error("version missing");
            result->version = output.substr(first + 1, last - first - 1);
            result->installed = Installed();
            result->update = result->installed && Version() != result->version;
            result->ok = true;
            finish();
            return;
        }

        NodeTools tools = FindNode();
        if (work == Work::Start) {
            if (!Installed()) InstallHarness(tools, pinnedDshVersion);
            if (closing) return;
            if (auto service = InspectPort(serverPort)) {
                if (!service->harness || !AdoptRunningService()) {
                    std::string name = service->image.empty()
                        ? "未知进程" : Narrow(fs::path(service->image).filename().wstring());
                    throw std::runtime_error("port busy: " + std::to_string(service->pid) + " " + name);
                }
                return;
            }
            result->installed = true;
            if (!finish()) return;
            PostLog(L"正在启动 DeepSeek Harness Web 服务…");
            try {
                DWORD code = RunServer(tools.node);
                if (!closing) PostMessageW(windowHandle, WM_SERVER_EXIT, code, 0);
            } catch (const std::exception& e) {
                PostLog(L"启动失败：" + ExceptionMessage(e));
                if (!closing) PostMessageW(windowHandle, WM_SERVER_EXIT, 1, 0);
            }
            return;
        }
        if (work == Work::InstallUpdate) {
            InstallHarness(tools, L"latest");
        } else {
            if (rollbackVersion.empty()) throw std::runtime_error("rollback missing");
            InstallHarness(tools, rollbackVersion);
        }
        result->ok = true; result->installed = true; result->version = Version();
    } catch (const std::exception& e) { result->message = ExceptionMessage(e); }
    finish();
}

static int LogLineHeight() {
    HDC dc = GetDC(logEdit);
    if (!dc) return 0;
    HFONT font = (HFONT)SendMessageW(logEdit, WM_GETFONT, 0, 0);
    HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW metrics{};
    bool ok = GetTextMetricsW(dc, &metrics);
    if (previous) SelectObject(dc, previous);
    ReleaseDC(logEdit, dc);
    return ok ? metrics.tmHeight + metrics.tmExternalLeading : 0;
}

static bool LogOverflows() {
    if (!logEdit) return false;
    RECT client{}; GetClientRect(logEdit, &client);
    int lineHeight = LogLineHeight();
    if (client.bottom <= 0 || lineHeight <= 0) return false;
    int visible = client.bottom / lineHeight;
    return (int)SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0) > visible + 1;
}

// EM_GETSEL's return value packs both positions into 16 bits, which is wrong once the
// log passes 64k characters; the pointer form reports the real 32-bit range.
static bool LogSelectionRange(LONG& start, LONG& end) {
    DWORD from = 0, to = 0;
    SendMessageW(logEdit, EM_GETSEL, (WPARAM)&from, (LPARAM)&to);
    start = (LONG)from; end = (LONG)to;
    return end > start;
}

// The bar stays out of the way until it is useful: content that overflows and either
// the pointer is over the box or something is selected.
static void UpdateLogScrollBar() {
    if (!logEdit) return;
    LONG start = 0, end = 0;
    bool selected = LogSelectionRange(start, end);
    bool want = (logHovered || selected) && LogOverflows();
    if (want == logScrollBarShown) return;   // ShowScrollBar repaints, so only on change
    logScrollBarShown = want;
    ShowScrollBar(logEdit, SB_VERT, want);
}

// Polled instead of tracked through WM_MOUSELEAVE: the scrollbar belongs to the edit, so
// once it appears the pointer sits on it, the control reports that the mouse left its
// client area and the bar would hide again — over and over, which is the flicker.
static void UpdateLogHover() {
    if (!logEdit) return;
    POINT cursor{};
    RECT box{};
    if (!GetCursorPos(&cursor) || !GetWindowRect(logEdit, &box)) return;
    bool over = PtInRect(&box, cursor) != 0;
    if (over == logHovered) return;
    logHovered = over;
    UpdateLogScrollBar();
}

static void ClearLog() {
    if (!logEdit) return;
    logLineCount = 0;
    SetWindowTextW(logEdit, L"");
    logScrollBarShown = false;
    ShowScrollBar(logEdit, SB_VERT, FALSE);
}

static void AppendLog(const std::wstring& line) {
    if (!logEdit) return;
    SYSTEMTIME now; GetLocalTime(&now);
    wchar_t stamp[32]; wsprintfW(stamp, L"[%02d:%02d:%02d] ", now.wHour, now.wMinute, now.wSecond);
    std::wstring entry = stamp; entry += line; entry += L"\r\n";
    // Append instead of rebuilding the whole box: with a 50k line cap a full
    // SetWindowText per line would be quadratic.
    int length = GetWindowTextLengthW(logEdit);
    SendMessageW(logEdit, EM_SETSEL, length, length);
    SendMessageW(logEdit, EM_REPLACESEL, FALSE, (LPARAM)entry.c_str());
    for (wchar_t c : entry) if (c == L'\n') ++logLineCount;
    if (logLineCount > maxLogLines) {
        // Drop a whole chunk rather than one line: deleting from the front of an edit
        // control moves the remaining text, so doing it per line is needlessly costly.
        int excess = logLineCount - maxLogLines;
        if (excess < trimChunk) excess = trimChunk;
        if (excess > logLineCount) excess = logLineCount;
        int cut = (int)SendMessageW(logEdit, EM_LINEINDEX, (WPARAM)excess, 0);
        if (cut > 0) {
            SendMessageW(logEdit, EM_SETSEL, 0, cut);
            SendMessageW(logEdit, EM_REPLACESEL, FALSE, (LPARAM)L"");
            logLineCount -= excess;
        }
    }
    // Park the caret at the start of the last line: it keeps the view pinned to the
    // bottom without dragging it sideways for long lines.
    int lastLine = (int)SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0) - 1;
    int lastStart = (int)SendMessageW(logEdit, EM_LINEINDEX, (WPARAM)lastLine, 0);
    if (lastStart >= 0) SendMessageW(logEdit, EM_SETSEL, lastStart, lastStart);
    SendMessageW(logEdit, EM_SCROLLCARET, 0, 0);
    UpdateLogScrollBar();
}

// dsh is started with --no-open, so the launcher decides: it reads the URL dsh printed
// and only opens a browser when no page is showing the UI already. The page keeps a
// persistent browser-session cookie, so an already open tab survives a service restart.
static std::wstring FindServerUrl() {
    std::ifstream file(ServerLogFile(), std::ios::binary);
    if (!file) return {};
    std::string text((std::istreambuf_iterator<char>(file)), {});
    std::wstring url;
    size_t position = 0;
    while ((position = text.find("dsh web: http", position)) != std::string::npos) {
        size_t start = position + 9;
        size_t end = text.find_first_of(" \t\r\n", start);
        url = Utf8(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        position = end == std::string::npos ? text.size() : end;
    }
    return url;
}

// The UI sets its title to "<session title> — DeepSeek Harness" (or plain "DeepSeek
// Harness" before a session exists), and browsers append their own suffixes when several
// tabs are open, so the product name has to be matched inside the window title. A
// background tab still hides itself from this check: a browser window only carries the
// title of its active tab.
static bool TitleLooksLikeHarnessPage(const std::wstring& title) {
    return title.find(L"DeepSeek Harness") != std::wstring::npos;
}

static bool HarnessPageOpen() {
    bool found = false;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        if (!IsWindowVisible(window)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId()) return TRUE;   // never our own window
        wchar_t title[512]{};
        if (GetWindowTextW(window, title, 512) <= 0) return TRUE;
        if (TitleLooksLikeHarnessPage(title)) { *(bool*)parameter = true; return FALSE; }
        return TRUE;
    }, (LPARAM)&found);
    return found;
}

static void OpenBrowserIfNoPage() {
    if (HarnessPageOpen()) {
        AppendLog(L"浏览器中已打开 DeepSeek Harness 页面，未重复打开新标签页。");
        return;
    }
    std::wstring url = FindServerUrl();
    if (url.empty()) return;
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    AppendLog(L"已在默认浏览器中打开 " + url);
}

// The log box offers only what makes sense for a read-only log: copy the selection
// and clear the box. The stock edit menu (cut/paste/undo/select all) is suppressed.
static void ShowLogMenu(HWND owner, LPARAM screenPosition) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, L"复制");
    AppendMenuW(menu, MF_STRING, 2, L"清除");
    LONG from = 0, to = 0;
    if (!LogSelectionRange(from, to)) EnableMenuItem(menu, 1, MF_BYCOMMAND | MF_GRAYED);
    POINT point{};
    if (screenPosition == (LPARAM)-1) GetCursorPos(&point);
    else { point.x = GET_X_LPARAM(screenPosition); point.y = GET_Y_LPARAM(screenPosition); }
    SetForegroundWindow(windowHandle);
    int command = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        point.x, point.y, 0, windowHandle, nullptr);
    DestroyMenu(menu);
    PostMessageW(windowHandle, WM_NULL, 0, 0);
    if (command == 1) SendMessageW(logEdit, WM_COPY, 0, 0);
    else if (command == 2) ClearLog();
}

static LRESULT CALLBACK LogEditProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (message) {
    case WM_CONTEXTMENU: ShowLogMenu(hwnd, lp); return 0;
    case WM_KEYDOWN:
        // The stock edit menu is gone, so select-all has to be provided here. The async
        // state covers key messages that were posted rather than generated by real input.
        if (wp == 'A' && ((GetKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_CONTROL) & 0x8000))) {
            SendMessageW(hwnd, EM_SETSEL, 0, (LPARAM)-1);
            UpdateLogScrollBar();
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        break;   // hover is polled by the timer, which also covers the scrollbar
    case WM_MOUSELEAVE:
        return 0;
    case WM_LBUTTONUP:
    case WM_KEYUP:
        UpdateLogScrollBar();
        break;
    }
    return DefSubclassProc(hwnd, message, wp, lp);
}

// Single place that defines the log control, so the behaviour under test is the same
// one the window creates.
static int Scaled(int n);

static void CreateLogEdit(HWND parent) {
    logEdit = CreateWindowExW(0,L"EDIT",L"",
        WS_CHILD|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_AUTOHSCROLL|ES_NOHIDESEL,
        0,0,0,0,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
    HFONT font = CreateFontW(-Scaled(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
    SendMessageW(logEdit,WM_SETFONT,(WPARAM)font,TRUE);
    logBackground = CreateSolidBrush(RGB(255,255,255));
    SetWindowSubclass(logEdit, LogEditProc, 1, 0);
    // A multiline edit defaults to a 30k character limit; the line cap is the only limit
    // this box should have, otherwise appends fail silently once it is hit.
    SendMessageW(logEdit, EM_SETLIMITTEXT, 0, 0);
    ShowScrollBar(logEdit, SB_VERT, FALSE);
}

// ---------------------------------------------------------------- tray icon

static constexpr UINT WM_TRAYICON = WM_APP + 20;
static constexpr UINT trayIconId = 1;
static NOTIFYICONDATAW trayIconData{};
static bool trayIconAdded = false;
static UINT taskbarCreatedMessage = 0;
static void OnClick(Button button);

static std::wstring StatusName() {
    switch (status) {
    case State::Running: return L"已启动";
    case State::Stopped: return L"未启动";
    case State::Update: return L"未启动，有更新";
    case State::Unresponsive: return L"已启动，服务无响应";
    default: return L"未安装固件";
    }
}

static void AddTrayIcon() {
    if (!windowHandle) return;
    trayIconData = {};
    trayIconData.cbSize = sizeof(trayIconData);
    trayIconData.hWnd = windowHandle;
    trayIconData.uID = trayIconId;
    trayIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    trayIconData.uCallbackMessage = WM_TRAYICON;
    trayIconData.hIcon = appIcon ? appIcon : LoadIconW(nullptr, IDI_APPLICATION);
    std::wstring tip = L"DeepSeek Harness 启动器 · " + StatusName();
    wcsncpy_s(trayIconData.szTip, tip.c_str(), _TRUNCATE);
    trayIconAdded = Shell_NotifyIconW(NIM_ADD, &trayIconData) != FALSE;
}

static void UpdateTrayTip() {
    if (!trayIconAdded) return;
    std::wstring tip = L"DeepSeek Harness 启动器 · " + StatusName();
    wcsncpy_s(trayIconData.szTip, tip.c_str(), _TRUNCATE);
    trayIconData.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &trayIconData);
}

static void RemoveTrayIcon() {
    if (!trayIconAdded) return;
    trayIconAdded = false;
    Shell_NotifyIconW(NIM_DELETE, &trayIconData);
}

static void ShowTrayBalloon(const std::wstring& title, const std::wstring& text) {
    if (!trayIconAdded) return;
    wcsncpy_s(trayIconData.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(trayIconData.szInfo, text.c_str(), _TRUNCATE);
    trayIconData.dwInfoFlags = NIIF_INFO;
    trayIconData.uFlags = NIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &trayIconData);
}

static void ShowLauncherWindow() {
    if (IsIconic(windowHandle)) ShowWindow(windowHandle, SW_RESTORE);
    ShowWindow(windowHandle, SW_SHOW);
    SetWindowPos(windowHandle, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(windowHandle);
}

static void HideLauncherWindow() { ShowWindow(windowHandle, SW_HIDE); }
static void ToggleLauncherWindow() {
    if (IsWindowVisible(windowHandle)) HideLauncherWindow();
    else ShowLauncherWindow();
}

static void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, IsWindowVisible(windowHandle) ? L"隐藏窗口" : L"显示窗口");
    AppendMenuW(menu, MF_STRING, 2, serverRunning ? L"停止服务" : L"启动服务");
    if (busy) EnableMenuItem(menu, 2, MF_BYCOMMAND | MF_GRAYED);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"退出启动器");
    POINT cursor{};
    GetCursorPos(&cursor);
    if (IsWindowVisible(windowHandle)) SetForegroundWindow(windowHandle);
    int command = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        cursor.x, cursor.y, 0, windowHandle, nullptr);
    DestroyMenu(menu);
    PostMessageW(windowHandle, WM_NULL, 0, 0);
    if (command == 1) ToggleLauncherWindow();
    else if (command == 2) OnClick(Button::Start);      // same path as the window button
    else if (command == 3) DestroyWindow(windowHandle);
}

static void SetStatus(State state, const std::wstring& detail) {
    status = state;
    std::wstring name = StatusName();
    statusTip = name + L" · " + detail;
    if (tooltip) { tipInfo.lpszText = statusTip.data(); SendMessageW(tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tipInfo); }
    UpdateTrayTip();
    InvalidateRect(windowHandle, nullptr, FALSE);
}

static int Scaled(int n) { return (int)(n * scaleFactor + .5f); }
static void Layout() {
    if (!windowHandle) return;
    int width = Scaled(mini ? W_MINI : W);
    int height = Scaled(mini ? H_COLLAPSED : (expanded ? H_EXPANDED : H_COLLAPSED));
    SetWindowPos(windowHandle, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!systemCorners)
        SetWindowRgn(windowHandle, CreateRoundRectRgn(0, 0, width, height, Scaled(12), Scaled(12)), TRUE);
    if (logEdit) {
        SetWindowPos(logEdit, nullptr, Scaled(20), Scaled(53), Scaled(W - 40), Scaled(157), SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(logEdit, (expanded && !mini) ? SW_SHOW : SW_HIDE);
        if (expanded && !mini) SetTimer(windowHandle, logHoverTimer, logHoverIntervalMs, nullptr);
        else {
            KillTimer(windowHandle, logHoverTimer);
            logHovered = false;
        }
        UpdateLogScrollBar();
    }
    InvalidateRect(windowHandle, nullptr, TRUE);
}

static void ExpandLog(bool value) { if (expanded == value) return; expanded = value; Layout(); }
static void ToggleMini() {
    mini = !mini;
    if (mini) ExpandLog(false);
    Layout();
    // the log is hidden while folded, so a stale hover state must not keep the bar alive
    if (mini) { logHovered = false; UpdateLogScrollBar(); }
    AppendLog(mini ? L"已折叠为图标模式，双击鲸鱼图标可展开。"
                   : L"已展开全部按钮。");
}
static Button Hit(int x, int y) {
    if (mini) return Button::None;   // folded: the whole chip drags, the icon toggles
    if (logRect.contains(x,y)) return Button::Log;
    if (statusRect.contains(x,y)) return Button::Status;
    if (startRect.contains(x,y)) return Button::Start;
    if (updateRect.contains(x,y)) return Button::Update;
    if (topRect.contains(x,y)) return Button::Topmost;
    if (minRect.contains(x,y)) return Button::Minimize;
    if (closeRect.contains(x,y)) return Button::Close;
    return Button::None;
}

static Color ColorForStatus() {
    switch (status) {
    case State::Running: return Color(34,197,94);
    case State::Stopped: return Color(234,179,8);
    case State::Update: return Color(168,85,247);
    // Amber, not the yellow of "not started": the process is up, the service is not.
    case State::Unresponsive: return Color(249,115,22);
    default: return Color(239,68,68);
    }
}

static void Rounded(GraphicsPath& p, float x, float y, float w, float h, float radius) {
    float d = radius * 2;
    p.AddArc(x, y, d, d, 180, 90); p.AddArc(x+w-d, y, d, d, 270, 90);
    p.AddArc(x+w-d, y+h-d, d, d, 0, 90); p.AddArc(x, y+h-d, d, d, 90, 90);
    p.CloseFigure();
}

static void DrawButton(Graphics& g, const UiRect& r, const std::wstring& label, Button button,
    bool primary = false, bool disabled = false) {
    Color bg = primary ? Color(37,99,235) : Color(255,255,255);
    Color border = primary ? Color(37,99,235) : Color(203,213,225);
    Color fg = primary ? Color(255,255,255) : Color(35,50,76);
    if (disabled) { bg = Color(248,250,252); fg = Color(160,170,185); border = Color(221,228,238); }
    else if (hoverButton == button) { bg = primary ? Color(29,78,216) : Color(239,246,255); }
    GraphicsPath path; Rounded(path,r.x+.5f, r.y+.5f, r.w-1.f, r.h-1.f, controlCornerRadius);
    SolidBrush fill(bg); Pen edge(border, 1); g.FillPath(&fill, &path); g.DrawPath(&edge, &path);
}

static void DrawCaption(HDC dc, const UiRect& r, const std::wstring& text, COLORREF color) {
    RECT bounds{Scaled(r.x), Scaled(r.y), Scaled(r.x + r.w), Scaled(r.y + r.h)};
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void Paint() {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(windowHandle, &ps);
    RECT client; GetClientRect(windowHandle, &client);
    HDC memory = CreateCompatibleDC(hdc); HBITMAP bitmap = CreateCompatibleBitmap(hdc, client.right, client.bottom);
    HGDIOBJ old = SelectObject(memory, bitmap);
    Graphics g(memory); g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.ScaleTransform(scaleFactor, scaleFactor);
    int width = mini ? W_MINI : W;
    int h = mini ? H_COLLAPSED : (expanded ? H_EXPANDED : H_COLLAPSED);
    SolidBrush white(Color::White); g.FillRectangle(&white, 0, 0, width, h);
    if (!systemCorners) {
        GraphicsPath frame; Rounded(frame,1.f,1.f,width-2.f,h-2.f,6);
        Pen border(Color(169,184,204),1.5f); g.DrawPath(&border,&frame);
    }
    DrawButton(g,statusRect,L"",Button::Status);
    SolidBrush dot(ColorForStatus());
    g.FillEllipse(&dot,statusRect.x + (indicatorWidth - 10) / 2,buttonTop + (buttonHeight - 10) / 2,10,10);
    if (!mini) {
    DrawButton(g,startRect,serverRunning ? L"停止" : L"启动",Button::Start,true,busy);
    DrawButton(g,logRect,L"日志",Button::Log);
    DrawButton(g,updateRect,updateLabel,Button::Update,false,busy || serverRunning);
    DrawButton(g,topRect,topmost ? L"关闭置顶" : L"开启置顶",Button::Topmost);
    GraphicsPath titlePath; Rounded(titlePath,titleClusterX + .5f,10.5f,49,27,controlCornerRadius); SolidBrush pale(Color(248,250,252));
    Pen light(Color(203,213,225),1); g.FillPath(&pale,&titlePath);
    if (hoverButton == Button::Minimize || hoverButton == Button::Close) {
        GraphicsState saved = g.Save(); g.SetClip(&titlePath);
        SolidBrush hl(Color(236,242,249));
        g.FillRectangle(&hl,hoverButton == Button::Minimize ? minRect.x + 1 : closeRect.x + 1,11,24,26);
        g.Restore(saved);
    }
    g.DrawPath(&light,&titlePath);
    Pen divider(Color(226,232,240),1); g.DrawLine(&divider,closeRect.x,16,closeRect.x,32);
    Pen dash(Color(71,85,105),1.8f); g.DrawLine(&dash,minRect.x + 8,25,minRect.x + 17,25);
    Pen cross(Color(185,28,28),1.8f);
    g.DrawLine(&cross,closeRect.x + 8,19,closeRect.x + 16,29);
    g.DrawLine(&cross,closeRect.x + 16,19,closeRect.x + 8,29);
    }   // end of the buttons-only block
    if (expanded && !mini) {
        GraphicsPath logBox; Rounded(logBox,14.5f,48.5f,(float)(W - 29),165,controlCornerRadius); Pen logBorder(Color(221,228,238),1);
        g.DrawPath(&logBorder,&logBox);
    }
    g.Flush();
    if (appIcon) DrawIconEx(memory, Scaled(12), Scaled(13), appIcon, Scaled(22), Scaled(22), 0, nullptr, DI_NORMAL);
    if (!mini) {
    HFONT font = CreateFontW(-Scaled(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    HGDIOBJ previousFont = SelectObject(memory,font);
    DrawCaption(memory,logRect,L"日志",RGB(35,50,76));
    DrawCaption(memory,startRect,serverRunning?L"停止":L"启动",busy?RGB(160,170,185):RGB(255,255,255));
    DrawCaption(memory,updateRect,updateLabel,busy||serverRunning?RGB(160,170,185):RGB(35,50,76));
    DrawCaption(memory,topRect,topmost?L"关闭置顶":L"开启置顶",RGB(35,50,76));
    SelectObject(memory,previousFont); DeleteObject(font);
    }
    BitBlt(hdc,0,0,client.right,client.bottom,memory,0,0,SRCCOPY);
    SelectObject(memory,old); DeleteObject(bitmap); DeleteDC(memory); EndPaint(windowHandle,&ps);
}

static void BeginWork(Work work) {
    if (busy || serverRunning) return;
    busy = true; InvalidateRect(windowHandle,nullptr,FALSE);
    std::thread([work]{ Worker(work); }).detach();
}

static void OnClick(Button button) {
    switch (button) {
    case Button::Log: ExpandLog(!expanded); break;
    case Button::Start:
        if (busy) break;
        if (serverRunning) {
            ServerIdentity id;
            { std::lock_guard lock(processMutex); id = serverIdentity; }
            if (serverExternal) {
                std::wstring question = L"当前 Web 服务不是由本启动器启动的（PID " + std::to_wstring(id.pid) +
                    L"）。\r\n结束它会中断正在进行的会话，确定要停止吗？";
                if (MessageBoxW(windowHandle, question.c_str(), L"停止 Web 服务",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST) != IDYES) break;
            }
            stopping = true; busy = true;
            if (TerminateServer(id)) SetStatus(State::Stopped,L"正在关闭 Web 服务…");
            else { busy = false; stopping = false; AppendLog(L"停止失败：无法结束 Web 服务进程。"); }
        } else { serverReady = false; BeginWork(Work::Start); }
        break;
    case Button::Update:
        if (busy || serverRunning) break;
        if (!rollbackVersion.empty()) BeginWork(Work::Rollback);
        else BeginWork(updateAvailable ? Work::InstallUpdate : Work::Check);
        break;
    case Button::Topmost:
        topmost = !topmost;
        SetWindowPos(windowHandle,topmost ? HWND_TOPMOST : HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
        AppendLog(topmost ? L"已开启窗口始终置顶。" : L"已关闭窗口始终置顶。");
        InvalidateRect(windowHandle,nullptr,FALSE); break;
    case Button::Minimize:
        AppendLog(L"已最小化到托盘，点击托盘图标可重新打开窗口。");
        HideLauncherWindow();
        break;
    case Button::Close: DestroyWindow(windowHandle); break;
    default: break;
    }
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_NCCALCSIZE: if (wp) return 0; break;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &point);
        int x = (int)(point.x / scaleFactor), y = (int)(point.y / scaleFactor);
        // The whale icon both moves the window and folds it. Handing it to the system as a
        // caption is what lets Windows tell "press and move" (drag) from "two quick
        // clicks" (double click) — doing the drag ourselves would swallow the second
        // click — and it delivers WM_NCLBUTTONDBLCLK for the fold.
        if (iconRect.contains(x, y)) return HTCAPTION;
        if (mini) return statusRect.contains(x, y) ? HTCLIENT : HTCAPTION;
        if (y < 50 && Hit(x, y) == Button::None) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCLBUTTONDBLCLK: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &point);
        int x = (int)(point.x / scaleFactor), y = (int)(point.y / scaleFactor);
        if (iconRect.contains(x, y)) ToggleMini();
        return 0;
    }
    case WM_NCRBUTTONUP:
        return 0;   // the strip stays free of the system menu
    case WM_GETMINMAXINFO: {
        // A window with a caption and a thick frame may not shrink below the system's
        // minimum tracking size (about 136 px wide), which would keep the folded chip
        // wider than the icon and the lamp. Declare our own minimum instead.
        MINMAXINFO* limits = (MINMAXINFO*)lp;
        limits->ptMinTrackSize.x = Scaled(W_MINI);
        limits->ptMinTrackSize.y = Scaled(H_COLLAPSED);
        return 0;
    }
    case WM_CREATE: {
        windowHandle = hwnd;
        DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUNDSMALL;
        systemCorners = SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners)));
        if (systemCorners) {
            COLORREF borderColor = RGB(169, 184, 204);
            DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
        }
        appIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1),IMAGE_ICON,0,0,LR_DEFAULTSIZE);
        SendMessageW(hwnd,WM_SETICON,ICON_SMALL,(LPARAM)appIcon);
        logEdit = nullptr;
        CreateLogEdit(hwnd);
        tooltip = CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP,
            CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        tipInfo.cbSize=sizeof(tipInfo); tipInfo.uFlags=TTF_SUBCLASS; tipInfo.hwnd=hwnd;
        tipInfo.uId=1;
        tipInfo.rect={Scaled(statusRect.x),Scaled(buttonTop),Scaled(statusRect.x + buttonWidth),Scaled(buttonTop + buttonHeight)};
        tipInfo.lpszText=statusTip.data(); SendMessageW(tooltip,TTM_ADDTOOLW,0,(LPARAM)&tipInfo);
        Layout();
        bool attached = AttachRunningServer() || AdoptRunningService();
        SetStatus(attached?State::Running:(Installed()?State::Stopped:State::Missing),
            attached?L"检测到正在运行的 DeepSeek Harness Web 服务。":
            (Installed()?L"点击“启动”运行 DeepSeek Harness。":L"点击“启动”自动安装 DeepSeek Harness。"));
        AppendLog(L"启动器已就绪。首次启动会按需下载工具并安装 DeepSeek Harness，可能需要几分钟。");
        if (attached) {
            AppendLog(L"检测到已运行的 DeepSeek Harness，可以点击“停止”结束服务。");
            OpenBrowserIfNoPage();
        }
        return 0;
    }
    case WM_PAINT: Paint(); return 0;
    case WM_ERASEBKGND: return 1;
    // A read-only edit asks through WM_CTLCOLORSTATIC; without this it is filled with
    // the system face colour instead of the white the rest of the window uses.
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        if ((HWND)lp != logEdit || !logBackground) break;
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(35, 50, 76));
        SetBkColor(dc, RGB(255, 255, 255));
        return (LRESULT)logBackground;
    }
    case WM_MOUSEMOVE: {
        int x=(int)(GET_X_LPARAM(lp)/scaleFactor), y=(int)(GET_Y_LPARAM(lp)/scaleFactor);
        Button hit=Hit(x,y); if(hit!=hoverButton){hoverButton=hit;InvalidateRect(hwnd,nullptr,FALSE);}
        TRACKMOUSEEVENT tme{sizeof(tme),TME_LEAVE,hwnd,0}; TrackMouseEvent(&tme); return 0;
    }
    case WM_MOUSELEAVE: hoverButton=Button::None; InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_LBUTTONDOWN: {
        int x=(int)(GET_X_LPARAM(lp)/scaleFactor), y=(int)(GET_Y_LPARAM(lp)/scaleFactor);
        OnClick(Hit(x,y));   // dragging is handled through WM_NCHITTEST/HTCAPTION
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int x=(int)(GET_X_LPARAM(lp)/scaleFactor), y=(int)(GET_Y_LPARAM(lp)/scaleFactor);
        if(iconRect.contains(x,y)) ToggleMini();
        return 0;
    }
    case WM_TIMER:
        if (wp == logHoverTimer) { UpdateLogHover(); return 0; }
        break;
    case WM_LOG: {
        std::unique_ptr<std::wstring> line((std::wstring*)lp);
        if(!closing) AppendLog(*line); return 0;
    }
    case WM_WORK_DONE: {
        std::unique_ptr<Result> r((Result*)lp);
        busy=r->work==Work::Start && r->message.empty();
        if(!r->message.empty()) {
            AppendLog((r->work==Work::Start?L"启动失败：":L"更新操作失败：")+r->message);
            SetStatus(Installed()?State::Stopped:State::Missing,r->message);
            if(r->work==Work::Start) ExpandLog(true);
        } else if(!r->info.empty()) {
            AppendLog(r->info);
            SetStatus(Installed()?State::Stopped:State::Missing,r->info);
        } else if(r->work==Work::Start) {
            updateAvailable=false; updateLabel=L"检查更新"; rollbackVersion.clear();
            SetStatus(State::Stopped,L"正在等待 Web 服务就绪…");
        } else if(r->work==Work::InstallUpdate) {
            updateAvailable=false; updateLabel=L"检查更新"; rollbackVersion.clear();
            AppendLog(L"更新完成，版本 "+r->version+L"。");
            SetStatus(State::Stopped,L"已安装版本 "+r->version+L"。");
        } else if(r->work==Work::Rollback) {
            updateAvailable=false; updateLabel=L"检查更新"; rollbackVersion.clear();
            AppendLog(L"已回退到版本 "+r->version+L"。");
            SetStatus(State::Stopped,L"已回退到版本 "+r->version+L"。");
        } else if(!r->installed) {
            AppendLog(L"最新版本 "+r->version+L"；本机尚未安装，点击“启动”即可安装。");
            SetStatus(State::Missing,L"npm 最新版本 "+r->version+L"。启动时会自动安装。");
        } else if(r->update) {
            updateAvailable=true; updateLabel=L"安装更新"; rollbackVersion.clear();
            AppendLog(L"发现新版本："+Version()+L" → "+r->version+L"。再次点击“安装更新”执行更新。");
            SetStatus(State::Update,L"当前 "+Version()+L"，npm 最新 "+r->version+L"。");
        } else {
            AppendLog(L"已是最新版本："+r->version+L"。");
            SetStatus(State::Stopped,L"当前版本 "+r->version+L"。");
        }
        InvalidateRect(hwnd,nullptr,FALSE); return 0;
    }
    case WM_DOWNLOAD:
        if (wp == 0) ExpandLog(true);
        SetStatus(status,L"正在下载 Node.js… "+std::to_wstring((int)wp)+L"%");
        return 0;
    case WM_SERVER_STARTED: serverRunning=true; serverExternal=false; busy=false; InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_SERVER_ADOPTED:
        serverRunning=true; serverReady=true; serverExternal=true; busy=false;
        SetStatus(State::Running,L"检测到已在运行的 Web 服务（PID "+std::to_wstring((DWORD)wp)+L"），已接管；点击“停止”可结束它。");
        InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_SERVER_READY:
        serverReady=true;
        if(!serverExternal) SaveKnownGoodVersion(Version());
        rollbackVersion.clear(); updateLabel=L"检查更新";
        SetStatus(State::Running,L"Web 服务已就绪；访问地址见运行日志。");
        OpenBrowserIfNoPage();
        ExpandLog(false); return 0;
    case WM_SERVER_UNHEALTHY:
        if (!serverRunning) return 0;
        SetStatus(State::Unresponsive, wp
            ? L"端口 "+std::to_wstring((int)serverPort)+L" 仍有监听，但连续 "+std::to_wstring(healthFailureThreshold)+L" 次探测都没有应答。"
            : L"端口 "+std::to_wstring((int)serverPort)+L" 已无监听；进程还在，但 Web 服务已经不在了。");
        AppendLog(wp ? L"Web 服务无响应（端口仍有监听，但没有应答），状态灯已变为橙色。"
                    : L"Web 服务已停止监听（端口无监听，进程仍在），状态灯已变为橙色。");
        return 0;
    case WM_SERVER_HEALTHY:
        if (!serverRunning || status != State::Unresponsive) return 0;
        SetStatus(State::Running, L"Web 服务已恢复响应。");
        AppendLog(L"Web 服务已恢复响应，状态灯已变回绿色。");
        return 0;
    case WM_SERVER_EXIT: {
        if (!serverRunning && !busy) return 0;
        bool failed=!stopping&&!serverReady;
        serverRunning=false; busy=false; serverReady=false; serverExternal=false;
        std::wstring known=KnownGoodVersion();
        if(failed&&!known.empty()&&known!=Version()) {
            rollbackVersion=known; updateAvailable=false; updateLabel=L"回退版本";
        }
        State stoppedState=updateAvailable?State::Update:(Installed()?State::Stopped:State::Missing);
        SetStatus(stoppedState, failed
            ? (rollbackVersion.empty()?L"服务未能启动，请查看日志。":L"服务未能启动；可回退到 "+rollbackVersion+L"。")
            : L"Web 服务已停止。");
        AppendLog(failed?L"启动失败，服务退出代码 "+std::to_wstring((DWORD)wp)+L"。":
            (stopping?L"Web 服务已停止。":L"Web 服务已退出，代码 "+std::to_wstring((DWORD)wp)+L"。"));
        if(failed&&!rollbackVersion.empty())
            AppendLog(L"上次可用版本是 "+rollbackVersion+L"，可点击“回退到 "+rollbackVersion+L"”恢复。");
        if(failed) ExpandLog(true);
        stopping=false; InvalidateRect(hwnd,nullptr,FALSE); return 0;
    }
    case WM_TRAYICON:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP: ToggleLauncherWindow(); break;
        case WM_LBUTTONDBLCLK: ShowLauncherWindow(); break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: ShowTrayMenu(); break;
        }
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        closing=true; KillJob(workJob); RemoveTrayIcon();
        if (logBackground) { DeleteObject(logBackground); logBackground = nullptr; }
        PostQuitMessage(0); return 0;
    default:
        // Explorer restarted: the tray icon has to be put back.
        if (taskbarCreatedMessage && message == taskbarCreatedMessage) { AddTrayIcon(); return 0; }
        break;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}

// A second launch must not create a rival window that fights over the same service:
// bring the running launcher to the front and leave.
static bool ActivateExistingInstance() {
    HWND existing = nullptr;
    for (int attempt = 0; attempt < 20 && !existing; attempt++) {
        existing = FindWindowW(L"DeepSeekHarnessLauncherNative", nullptr);
        if (!existing) Sleep(100);
    }
    if (!existing) return false;
    // The launcher may be sitting in the tray, which is just a hidden window.
    ShowWindow(existing, SW_SHOW);
    if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
    // Windows only lets the foreground process hand over the foreground, and a launcher
    // started from Explorer or a shortcut does not own it: attach to the foreground
    // thread for the duration of the call, and flip the topmost flag to force a raise.
    HWND foreground = GetForegroundWindow();
    DWORD currentThread = GetCurrentThreadId();
    DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    bool attached = foregroundThread && foregroundThread != currentThread &&
        AttachThreadInput(foregroundThread, currentThread, TRUE);
    bool wasTopmost = (GetWindowLongPtrW(existing, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    SetWindowPos(existing, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!wasTopmost) SetWindowPos(existing, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(existing);
    BringWindowToTop(existing);
    if (attached) AttachThreadInput(foregroundThread, currentThread, FALSE);
    FLASHWINFO flash{sizeof(flash), existing, FLASHW_ALL, 3, 0};
    FlashWindowEx(&flash);
    return true;
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\DeepSeekHarnessLauncher.SingleInstance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        ActivateExistingInstance();
        CloseHandle(instanceMutex);
        return 0;
    }
    SetProcessDPIAware();
    GdiplusStartupInput gdiplusInput; ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken,&gdiplusInput,nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES}; InitCommonControlsEx(&controls);
    runtimeDir=LocalAppData()/L"DeepSeekHarnessLauncher"/L"runtime";
    HDC dc=GetDC(nullptr); scaleFactor=(float)GetDeviceCaps(dc,LOGPIXELSX)/96.f; ReleaseDC(nullptr,dc);
    WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.hInstance=instance;
    wc.lpszClassName=L"DeepSeekHarnessLauncherNative"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.style=CS_DBLCLKS;   // needed for WM_LBUTTONDBLCLK on the whale icon
    wc.hIcon=(HICON)LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,0,0,LR_DEFAULTSIZE);
    RegisterClassW(&wc);
    RECT area{}; SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0);
    int width=Scaled(W),height=Scaled(H_COLLAPSED);
    int x=area.left+(area.right-area.left-width)/2,y=area.top+(area.bottom-area.top-height)/2;
    // No WS_CAPTION / WS_THICKFRAME: the non-client area is removed anyway, and a caption
    // or thick frame would make the system refuse to shrink the folded chip below its
    // minimum tracking size (~136 px), which is wider than the icon and the lamp.
    // WS_EX_TOOLWINDOW keeps this floating widget out of the taskbar and out of the
    // Alt+Tab list; a tool window is still restored by launching the exe again.
    HWND hwnd=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,wc.lpszClassName,L"DeepSeek Harness 启动器",
        WS_POPUP|WS_SYSMENU|WS_MINIMIZEBOX,x,y,width,height,nullptr,nullptr,instance,nullptr);
    if(!hwnd) return 1;
    SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
    // Starts in the tray: the window stays hidden until the icon is clicked. The balloon
    // is the only hint that it is running, since there is no taskbar button either.
    taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();
    ShowTrayBalloon(L"DeepSeek Harness 启动器", L"已在托盘运行。点击托盘图标打开窗口，右键显示菜单。");
    UpdateWindow(hwnd);
    MSG message;
    while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    GdiplusShutdown(gdiplusToken);
    return (int)message.wParam;
}
