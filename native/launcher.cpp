#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <string>
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
static constexpr int W = 470, H_COLLAPSED = 48, H_EXPANDED = 228;
static constexpr float buttonCornerRadius = 4.5f;
static constexpr DWORD updateCheckTimeoutMs = 20'000;
enum class State { Running, Stopped, Missing, Update };
enum class Work { Start, Check, InstallUpdate };
enum class Button { None, Log, Status, Start, Update, Topmost, Minimize, Close };
struct Result { Work work; bool ok; std::wstring message; std::wstring version; bool installed; bool update; };
struct ServerIdentity { DWORD pid = 0; unsigned long long created = 0; };
struct UiRect { int x, y, w, h; bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; } };

static HWND windowHandle, logEdit;
static HICON appIcon;
static std::atomic_bool closing{false};
static std::mutex processMutex;
static HANDLE workJob = nullptr, serverJob = nullptr;
static ServerIdentity serverIdentity;
static fs::path runtimeDir;
static constexpr wchar_t serverJobName[] = L"Local\\DeepSeekHarnessLauncher.Server";
static bool expanded = false, topmost = true, busy = false, serverRunning = false;
static bool systemCorners = false;
static bool serverReady = false, stopping = false, updateAvailable = false;
static State status = State::Missing;
static Button hoverButton = Button::None;
static std::wstring logBuffer, statusTip = L"未安装固件", updateLabel = L"检查更新";
static HWND tooltip;
static TOOLINFOW tipInfo{};
static float scaleFactor = 1.0f;

static const UiRect logRect{42, 10, 68, 28};
static const UiRect statusRect{114, 10, 68, 28};
static const UiRect startRect{186, 10, 68, 28};
static const UiRect updateRect{258, 10, 68, 28};
static const UiRect topRect{330, 10, 68, 28};
static const UiRect minRect{406, 10, 25, 28};
static const UiRect closeRect{431, 10, 25, 28};

static std::wstring Utf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    UINT codepage = CP_UTF8;
    if (!n) { codepage = CP_ACP; n = MultiByteToWideChar(codepage, 0, s.data(), (int)s.size(), nullptr, 0); }
    std::wstring out(n, L'\0');
    MultiByteToWideChar(codepage, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
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

static void InstallLocalNode() {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(2), RT_RCDATA);
    if (!resource) throw std::runtime_error("node installer missing");
    HGLOBAL loaded = LoadResource(module, resource);
    const char* bytes = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
    DWORD size = SizeofResource(module, resource);
    if (!bytes || !size) throw std::runtime_error("node installer missing");

    fs::path toolsDir = LocalNodeRoot().parent_path();
    fs::create_directories(toolsDir);
    fs::path script = toolsDir / L"install-node.ps1";
    {
        std::ofstream file(script, std::ios::binary | std::ios::trunc);
        file.write(bytes, size);
        if (!file) throw std::runtime_error("node installer write failed");
    }
    std::wstring powershell = FindOnPath(L"powershell.exe");
    if (powershell.empty()) throw std::runtime_error("powershell missing");
    DWORD code = RunNode(powershell, {L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass",
        L"-File", script.wstring(), L"-InstallRoot", LocalNodeRoot().wstring()});
    if (code != 0 || !CompleteNode(LocalNodeRoot())) throw std::runtime_error("node download failed");
}

static NodeTools FindNode() {
    if (auto local = CompleteNode(LocalNodeRoot())) {
        PrependLocalNodeToPath();
        PostLog(L"使用本地 Node.js 和 npm。");
        return *local;
    }

    std::wstring systemNode = FindOnPath(L"node.exe");
    if (!systemNode.empty()) {
        fs::path root = fs::path(systemNode).parent_path();
        if (auto system = CompleteNode(root)) {
            PostLog(L"使用系统 Node.js 和 npm。");
            return *system;
        }
        std::wstring npmCmd = FindOnPath(L"npm.cmd");
        if (!npmCmd.empty()) {
            fs::path npm = fs::path(npmCmd).parent_path() / L"node_modules" / L"npm" / L"bin" / L"npm-cli.js";
            if (fs::is_regular_file(npm)) {
                PostLog(L"使用系统 Node.js 和 npm。");
                return {systemNode, npm.wstring()};
            }
        }
    }

    PostLog(L"未找到完整的 Node.js 和 npm，正在下载到本地（首次使用可能需要几分钟）…");
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
    return id;
}

static void SaveServerIdentity(ServerIdentity id) {
    fs::create_directories(ServerPidFile().parent_path());
    std::ofstream file(ServerPidFile(), std::ios::trunc);
    file << id.pid << ' ' << id.created << '\n';
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
            if (line.find("dsh web: http") != std::string::npos && !closing)
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
    while (!closing) {
        ReadServerLog(offset, pending);
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
    std::wstring cmd = Quote(node) + L" " + Quote(EntryPoint().wstring()) + L" web";
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
    ServerIdentity id{pi.dwProcessId, CreationTime(pi.hProcess)};
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
    serverReady = true;
    std::thread([process, job, id = *saved] {
        DWORD code = MonitorServer(process, job, id, true);
        if (!closing) PostMessageW(windowHandle, WM_SERVER_EXIT, code, 0);
    }).detach();
    return true;
}

static void InstallHarness(const NodeTools& tools) {
    fs::create_directories(runtimeDir);
    PostLog(L"安装 @deepseek-ai/dsh@latest …");
    DWORD code = RunNode(tools.node,
        {tools.npm, L"install", L"--prefix", runtimeDir.wstring(), L"--no-audit", L"--no-fund", L"@deepseek-ai/dsh@latest"});
    if (code != 0 || !Installed()) throw std::runtime_error("install failed: " + std::to_string(code));
    PostLog(L"安装完成，版本 " + Version() + L"。");
}

static std::wstring ExceptionMessage(const std::exception& e) {
    std::string what = e.what();
    if (what == "powershell missing") return L"未找到 Windows PowerShell，无法自动下载 Node.js。";
    if (what == "node download failed") return L"下载 Node.js 失败。请检查网络后重试，或自行安装 Node.js。";
    if (what == "pipe") return L"无法建立子进程输出管道。";
    if (what == "update timeout") return L"检查更新超过 " + std::to_wstring(updateCheckTimeoutMs / 1000) + L" 秒，已停止检查。";
    if (what.rfind("install failed:", 0) == 0) return L"安装失败（退出码 " + Utf8(what.substr(16)) + L"）。请查看运行日志。";
    return L"操作失败：" + Utf8(what);
}

static void Worker(Work work) {
    auto result = std::make_unique<Result>(); result->work = work; result->ok = false;
    try {
        NodeTools tools = FindNode();
        if (work == Work::Start) {
            if (!Installed()) InstallHarness(tools);
            if (closing) return;
            result->installed = true;
            if (!PostMessageW(windowHandle, WM_WORK_DONE, 0, (LPARAM)result.get())) return;
            result.release();
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
            InstallHarness(tools);
            result->ok = true; result->installed = true; result->version = Version();
        } else {
            PostLog(L"正在检查 npm 上的最新版本…");
            std::wstring output;
            DWORD code = RunNode(tools.node, {tools.npm, L"view", L"@deepseek-ai/dsh", L"version", L"--json"}, &output, updateCheckTimeoutMs);
            if (code != 0) throw std::runtime_error("check failed");
            size_t first = output.find(L'\"'), last = output.find(L'\"', first + 1);
            if (first == std::wstring::npos || last == std::wstring::npos) throw std::runtime_error("version missing");
            result->version = output.substr(first + 1, last - first - 1);
            result->installed = Installed();
            result->update = result->installed && Version() != result->version;
            result->ok = true;
        }
    } catch (const std::exception& e) { result->message = ExceptionMessage(e); }
    if (!closing && !PostMessageW(windowHandle, WM_WORK_DONE, 0, (LPARAM)result.get())) return;
    result.release();
}

static void AppendLog(const std::wstring& line) {
    SYSTEMTIME now; GetLocalTime(&now);
    wchar_t stamp[32]; wsprintfW(stamp, L"[%02d:%02d:%02d] ", now.wHour, now.wMinute, now.wSecond);
    logBuffer += stamp; logBuffer += line; logBuffer += L"\r\n";
    if (logBuffer.size() > 200000) logBuffer.erase(0, logBuffer.size() - 150000);
    if (logEdit) {
        SetWindowTextW(logEdit, logBuffer.c_str());
        SendMessageW(logEdit, EM_SETSEL, (WPARAM)logBuffer.size(), (LPARAM)logBuffer.size());
        SendMessageW(logEdit, EM_SCROLLCARET, 0, 0);
    }
}

static void SetStatus(State state, const std::wstring& detail) {
    status = state;
    std::wstring name = state == State::Running ? L"已启动" : state == State::Stopped ? L"未启动" :
        state == State::Update ? L"未启动，有更新" : L"未安装固件";
    statusTip = name + L" · " + detail;
    if (tooltip) { tipInfo.lpszText = statusTip.data(); SendMessageW(tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tipInfo); }
    InvalidateRect(windowHandle, nullptr, FALSE);
}

static int Scaled(int n) { return (int)(n * scaleFactor + .5f); }
static void Layout() {
    if (!windowHandle) return;
    int width = Scaled(W), height = Scaled(expanded ? H_EXPANDED : H_COLLAPSED);
    SetWindowPos(windowHandle, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!systemCorners)
        SetWindowRgn(windowHandle, CreateRoundRectRgn(0, 0, width, height, Scaled(12), Scaled(12)), TRUE);
    if (logEdit) {
        SetWindowPos(logEdit, nullptr, Scaled(20), Scaled(53), Scaled(430), Scaled(157), SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(logEdit, expanded ? SW_SHOW : SW_HIDE);
    }
    InvalidateRect(windowHandle, nullptr, TRUE);
}

static void ExpandLog(bool value) { if (expanded == value) return; expanded = value; Layout(); }
static Button Hit(int x, int y) {
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
    GraphicsPath path; Rounded(path,r.x+.5f, r.y+.5f, r.w-1.f, r.h-1.f, buttonCornerRadius);
    SolidBrush fill(bg); Pen edge(border, 1); g.FillPath(&fill, &path); g.DrawPath(&edge, &path);
}

static void DrawCaption(HDC dc, const UiRect& r, const std::wstring& text, COLORREF color, bool statusLabel = false) {
    RECT bounds{Scaled(r.x + (statusLabel ? 15 : 0)), Scaled(r.y), Scaled(r.x + r.w), Scaled(r.y + r.h)};
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
    int h = expanded ? H_EXPANDED : H_COLLAPSED;
    SolidBrush white(Color::White); g.FillRectangle(&white, 0, 0, W, h);
    if (!systemCorners) {
        GraphicsPath frame; Rounded(frame,1.f,1.f,W-2.f,h-2.f,6);
        Pen border(Color(169,184,204),1.5f); g.DrawPath(&border,&frame);
    }
    DrawButton(g,logRect,L"日志",Button::Log);
    DrawButton(g,statusRect,L"状态",Button::Status);
    SolidBrush dot(ColorForStatus()); g.FillEllipse(&dot,126,19,10,10);
    DrawButton(g,startRect,serverRunning ? L"停止" : L"启动",Button::Start,true,busy);
    DrawButton(g,updateRect,updateLabel,Button::Update,false,busy || serverRunning);
    DrawButton(g,topRect,topmost ? L"关闭置顶" : L"开启置顶",Button::Topmost);
    GraphicsPath titlePath; Rounded(titlePath,406.5f,10.5f,49,27,buttonCornerRadius); SolidBrush pale(Color(248,250,252));
    Pen light(Color(203,213,225),1); g.FillPath(&pale,&titlePath);
    if (hoverButton == Button::Minimize || hoverButton == Button::Close) {
        GraphicsState saved = g.Save(); g.SetClip(&titlePath);
        SolidBrush hl(Color(236,242,249)); g.FillRectangle(&hl,hoverButton == Button::Minimize ? 407 : 431,11,24,26);
        g.Restore(saved);
    }
    g.DrawPath(&light,&titlePath);
    Pen divider(Color(226,232,240),1); g.DrawLine(&divider,431,16,431,32);
    Pen dash(Color(71,85,105),1.8f); g.DrawLine(&dash,414,25,423,25);
    Pen cross(Color(185,28,28),1.8f); g.DrawLine(&cross,439,19,447,29); g.DrawLine(&cross,447,19,439,29);
    if (expanded) {
        GraphicsPath logBox; Rounded(logBox,14.5f,48.5f,441,165,7); Pen logBorder(Color(221,228,238),1);
        g.DrawPath(&logBorder,&logBox);
    }
    g.Flush();
    if (appIcon) DrawIconEx(memory, Scaled(12), Scaled(13), appIcon, Scaled(22), Scaled(22), 0, nullptr, DI_NORMAL);
    HFONT font = CreateFontW(-Scaled(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    HGDIOBJ previousFont = SelectObject(memory,font);
    DrawCaption(memory,logRect,L"日志",RGB(35,50,76));
    DrawCaption(memory,statusRect,L"状态",RGB(35,50,76),true);
    DrawCaption(memory,startRect,serverRunning?L"停止":L"启动",busy?RGB(160,170,185):RGB(255,255,255));
    DrawCaption(memory,updateRect,updateLabel,busy||serverRunning?RGB(160,170,185):RGB(35,50,76));
    DrawCaption(memory,topRect,topmost?L"关闭置顶":L"开启置顶",RGB(35,50,76));
    SelectObject(memory,previousFont); DeleteObject(font);
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
            stopping = true; busy = true;
            bool terminated = false;
            ServerIdentity id;
            {
                std::lock_guard lock(processMutex);
                id = serverIdentity;
                if (serverJob) terminated = !!TerminateJobObject(serverJob, 1);
            }
            if (!terminated) {
                HANDLE process = OpenLiveServer(id);
                if (process) { terminated = !!TerminateProcess(process, 1); CloseHandle(process); }
            }
            if (terminated) SetStatus(State::Stopped,L"正在关闭 Web 服务…");
            else { busy = false; stopping = false; AppendLog(L"停止失败：无法结束 Web 服务进程。"); }
        } else { serverReady = false; BeginWork(Work::Start); }
        break;
    case Button::Update: if (!busy && !serverRunning) BeginWork(updateAvailable ? Work::InstallUpdate : Work::Check); break;
    case Button::Topmost:
        topmost = !topmost;
        SetWindowPos(windowHandle,topmost ? HWND_TOPMOST : HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
        AppendLog(topmost ? L"已开启窗口始终置顶。" : L"已关闭窗口始终置顶。");
        InvalidateRect(windowHandle,nullptr,FALSE); break;
    case Button::Minimize: ShowWindow(windowHandle,SW_MINIMIZE); break;
    case Button::Close: DestroyWindow(windowHandle); break;
    default: break;
    }
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_NCCALCSIZE: if (wp) return 0; break;
    case WM_NCHITTEST: return HTCLIENT;
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
        logEdit = CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            0,0,0,0,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        HFONT font = CreateFontW(-Scaled(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
        SendMessageW(logEdit,WM_SETFONT,(WPARAM)font,TRUE);
        tooltip = CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP,
            CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        tipInfo.cbSize=sizeof(tipInfo); tipInfo.uFlags=TTF_SUBCLASS; tipInfo.hwnd=hwnd;
        tipInfo.uId=1; tipInfo.rect={Scaled(114),Scaled(10),Scaled(182),Scaled(38)};
        tipInfo.lpszText=statusTip.data(); SendMessageW(tooltip,TTM_ADDTOOLW,0,(LPARAM)&tipInfo);
        Layout();
        bool attached = AttachRunningServer();
        SetStatus(attached?State::Running:(Installed()?State::Stopped:State::Missing),
            attached?L"检测到正在运行的 DeepSeek Harness Web 服务。":
            (Installed()?L"点击“启动”运行 DeepSeek Harness。":L"点击“启动”自动安装 DeepSeek Harness。"));
        AppendLog(L"启动器已就绪。首次启动会按需下载工具并安装 DeepSeek Harness，可能需要几分钟。");
        if (attached) AppendLog(L"检测到已运行的 DeepSeek Harness，可以点击“停止”结束服务。");
        return 0;
    }
    case WM_PAINT: Paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        int x=(int)(GET_X_LPARAM(lp)/scaleFactor), y=(int)(GET_Y_LPARAM(lp)/scaleFactor);
        Button hit=Hit(x,y); if(hit!=hoverButton){hoverButton=hit;InvalidateRect(hwnd,nullptr,FALSE);}
        TRACKMOUSEEVENT tme{sizeof(tme),TME_LEAVE,hwnd,0}; TrackMouseEvent(&tme); return 0;
    }
    case WM_MOUSELEAVE: hoverButton=Button::None; InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_LBUTTONDOWN: {
        int x=(int)(GET_X_LPARAM(lp)/scaleFactor), y=(int)(GET_Y_LPARAM(lp)/scaleFactor);
        Button hit=Hit(x,y);
        if(hit==Button::None && y<50) { ReleaseCapture(); SendMessageW(hwnd,WM_NCLBUTTONDOWN,HTCAPTION,0); }
        else OnClick(hit);
        return 0;
    }
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
        } else if(r->work==Work::Start) {
            updateAvailable=false; updateLabel=L"检查更新";
            SetStatus(State::Stopped,L"正在等待 Web 服务就绪…");
        } else if(r->work==Work::InstallUpdate) {
            updateAvailable=false; updateLabel=L"检查更新";
            AppendLog(L"更新完成，版本 "+r->version+L"。");
            SetStatus(State::Stopped,L"已安装版本 "+r->version+L"。");
        } else if(!r->installed) {
            AppendLog(L"最新版本 "+r->version+L"；本机尚未安装，点击“启动”即可安装。");
            SetStatus(State::Missing,L"npm 最新版本 "+r->version+L"。启动时会自动安装。");
        } else if(r->update) {
            updateAvailable=true; updateLabel=L"安装更新";
            AppendLog(L"发现新版本："+Version()+L" → "+r->version+L"。再次点击“安装更新”执行更新。");
            SetStatus(State::Update,L"当前 "+Version()+L"，npm 最新 "+r->version+L"。");
        } else {
            AppendLog(L"已是最新版本："+r->version+L"。");
            SetStatus(State::Stopped,L"当前版本 "+r->version+L"。");
        }
        InvalidateRect(hwnd,nullptr,FALSE); return 0;
    }
    case WM_SERVER_STARTED: serverRunning=true; busy=false; InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_SERVER_READY:
        serverReady=true; SetStatus(State::Running,L"Web 服务已就绪；浏览器将自动打开，访问地址见运行日志。");
        ExpandLog(false); return 0;
    case WM_SERVER_EXIT: {
        if (!serverRunning && !busy) return 0;
        bool failed=!stopping&&!serverReady;
        serverRunning=false; busy=false;
        State stoppedState=updateAvailable?State::Update:(Installed()?State::Stopped:State::Missing);
        SetStatus(stoppedState,failed?L"服务未能启动，请查看日志。":L"Web 服务已停止。");
        AppendLog(failed?L"启动失败，服务退出代码 "+std::to_wstring((DWORD)wp)+L"。":
            (stopping?L"Web 服务已停止。":L"Web 服务已退出，代码 "+std::to_wstring((DWORD)wp)+L"。"));
        if(failed) ExpandLog(true);
        stopping=false; InvalidateRect(hwnd,nullptr,FALSE); return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        closing=true; KillJob(workJob); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    SetProcessDPIAware();
    GdiplusStartupInput gdiplusInput; ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken,&gdiplusInput,nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES}; InitCommonControlsEx(&controls);
    runtimeDir=LocalAppData()/L"DeepSeekHarnessLauncher"/L"runtime";
    HDC dc=GetDC(nullptr); scaleFactor=(float)GetDeviceCaps(dc,LOGPIXELSX)/96.f; ReleaseDC(nullptr,dc);
    WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.hInstance=instance;
    wc.lpszClassName=L"DeepSeekHarnessLauncherNative"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hIcon=(HICON)LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,0,0,LR_DEFAULTSIZE);
    RegisterClassW(&wc);
    RECT area{}; SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0);
    int width=Scaled(W),height=Scaled(H_COLLAPSED);
    int x=area.left+(area.right-area.left-width)/2,y=area.top+(area.bottom-area.top-height)/2;
    HWND hwnd=CreateWindowExW(WS_EX_TOPMOST|WS_EX_APPWINDOW,wc.lpszClassName,L"DeepSeek Harness 启动器",
        WS_POPUP|WS_CAPTION|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX,x,y,width,height,nullptr,nullptr,instance,nullptr);
    if(!hwnd) return 1;
    SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
    ShowWindow(hwnd,SW_SHOW); UpdateWindow(hwnd);
    MSG message;
    while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    GdiplusShutdown(gdiplusToken);
    return (int)message.wParam;
}
