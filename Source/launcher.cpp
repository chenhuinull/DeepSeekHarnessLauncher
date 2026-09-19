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
static constexpr int H_COLLAPSED = 48;
// The log panel: the text box and the border painted around it. The panel used to be
// 157px tall and is shown 30% shorter (157 × 0.7 = 110), so H_EXPANDED follows from the
// panel instead of being a separate number that could drift out of step with it.
static constexpr int logTop = 53, logSide = 20, logHeight = 110, logBottom = 18;
static constexpr int H_EXPANDED = logTop + logHeight + logBottom;
// The painted border sits at logBoxTop/logBoxSide; inside it the text box takes everything except a
// narrow zone next to the border, and that zone is where the two scroll bars live. No system scroll
// bar is involved, so nothing is reserved for a non-client strip and the text gets the space.
static constexpr float logBoxTop = 48.5f, logBoxSide = 14.5f;
static constexpr float logBoxHeight = logHeight + 8;
static constexpr int logBarZone = 7;
// The log font. The log is mostly Chinese, and Consolas has no Chinese glyphs at all: they were font
// linked from another family and squeezed into a Latin cell, which read as squashed. NSimSun draws
// both scripts itself. 13px and not 12: at 12 its glyphs fill the cell exactly (ink 12 rows in a 12px
// line), so the lines touch; at 13 there are 12 rows of ink in a 13px line, which leaves a hairline
// between them and still fits eight whole lines in the panel. Consolas stays as the fallback.
static constexpr int logFontHeight = 13;
static const wchar_t* const logFontFace = L"NSimSun";
static const wchar_t* const logFontFallback = L"Consolas";
// The whole button row is derived from these numbers, so the window width follows the
// button width instead of being a separate constant.
static constexpr int buttonWidth = 46, buttonHeight = 28, buttonTop = 10, buttonGap = 4;
// The status control is a lamp only, so it stays as narrow as a square indicator.
static constexpr int indicatorWidth = 28;
// The lit part of the lamp. It was 10px inside the 28px button, which read as a small dot in the
// middle of a lot of white; the reader asked for a bigger one. Even and one less than a third of the
// button is what makes it centred: 28 - 16 leaves 6 pixels of margin on each side, where an odd
// diameter leaves 6 on one side and 7 on the other and the dot looks off to the left.
static constexpr int lampDotDiameter = 16;
// The title bar reads [close][minimize], then the launcher's own buttons, and ends with the lamp
// sitting next to the whale. The reader asked for all three moves: the window buttons and the whale
// traded places, then close and minimize swapped with each other, and the lamp came over to the whale.
static constexpr int leftMargin = 10, rightMargin = 8;
static constexpr int titleButtonWidth = 25, titleClusterGap = 8;
static constexpr int closeX = leftMargin;                 // close leads the row, minimize follows it
static constexpr int minX = closeX + titleButtonWidth;
// The pair shares one painted plate, so both the plate and the divider that splits it are derived
// from the left edge of the pair. Deriving them from one named button instead is what put the plate
// over the first button and the divider on the outside edge when close and minimize swapped.
static constexpr int titleClusterX = closeX;
static constexpr int titleDividerX = titleClusterX + titleButtonWidth;
static constexpr int firstButtonX = titleClusterX + 2 * titleButtonWidth + titleClusterGap;
// The buttons run 置顶、更新、日志、启动/停止 from the left, so the one that starts and stops the service
// ends up right beside the lamp that reports what it did.
static constexpr int topmostX = firstButtonX;
static constexpr int updateX = topmostX + buttonWidth + buttonGap;
static constexpr int logX = updateX + buttonWidth + buttonGap;
static constexpr int startX = logX + buttonWidth + buttonGap;
static constexpr int lastButtonRight = startX + buttonWidth;
// The lamp comes after the buttons and right before the whale, which closes the row.
static constexpr int statusX = lastButtonRight + titleClusterGap;
static constexpr int iconTop = 8, iconWidth = 26, iconHeight = 32;
// The icon is drawn smaller than its slot and centred in it, so the slot's right margin has to be short
// by that padding for the gap the reader sees to match the one before the close button on the left.
static constexpr int iconDrawSize = 22;
static constexpr int iconPadding = (iconWidth - iconDrawSize) / 2;
static constexpr int iconX = statusX + indicatorWidth + buttonGap;
static constexpr int W = iconX + iconWidth + rightMargin;
// Inside the painted border the text box takes everything except the bar zone, so the text gets the
// space a system scroll bar strip would have taken.
static constexpr int logBoxRightPx = W - 17, logBoxBottomPx = 48 + logHeight + 8;
static constexpr int logEditWidth = logBoxRightPx - logBarZone - logSide;
static constexpr int logEditHeight = logBoxBottomPx - logBarZone - logTop;
// The launcher's own window style. WS_CLIPCHILDREN is what a window that owns controls should carry:
// without it the window's painting is not clipped away from the log box and its bars, and any erase
// path that is not BeginPaint (which clips by itself) can touch them.
static constexpr DWORD mainWindowStyle = WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
// The chip's own controls, in chip coordinates.
static constexpr int miniLampX = leftMargin, miniIconX = miniLampX + indicatorWidth + buttonGap;
static constexpr int W_MINI = miniIconX + iconWidth + rightMargin;
// Folded, the chip is the right end of a window that never changes width: the window rectangle stays
// at W, and a region decides which part of it is the chip. Folding therefore moves no window and
// resizes none, which is what keeps the compositor from showing the surface it already has at a new
// geometry for a frame — the flicker the reader saw when folding moved the window instead.
static constexpr int miniOffsetX = W - W_MINI;
static constexpr USHORT serverPort = 3080;
static constexpr int maxLogLines = 50'000;
static constexpr int trimChunk = 500;
static constexpr DWORD readyProbeIntervalMs = 1'000;
// After the service is up, keep asking the port whether it still answers: a process can
// stay alive while the web server inside it is gone.
static constexpr DWORD healthProbeIntervalMs = 5'000;
static constexpr int healthFailureThreshold = 3;
// Auto restart, the tray toggle: wait a moment before pulling the service back up, back off when it
// keeps failing, and give up after a few tries so a broken install cannot become an endless loop of
// starts. A start that stays up for a minute counts as a success and clears the counter.
static constexpr int maxAutoRestarts = 3;
static constexpr DWORD autoRestartFirstDelayMs = 3'000;
static constexpr DWORD autoRestartMaxDelayMs = 30'000;
static constexpr ULONGLONG autoRestartSettledMs = 60'000;
static constexpr UINT_PTR autoRestartTimerId = 1;
// Shared with the "stopped, update available" lamp: a running service with auto restart armed is
// purple too, and the two can never be on at once because that one needs a stopped service.
static constexpr ARGB lampPurpleArgb = 0xFFA855F7;
// The launcher's outermost border, and the frame the right-click menus ask DWM to draw so they match
// the window they belong to: #B8B8B8, a neutral grey.
static constexpr ARGB windowBorderArgb = 0xFFB8B8B8;
static constexpr COLORREF windowBorderColor = RGB(0xB8, 0xB8, 0xB8);
// One corner radius for everything the launcher draws round: the buttons, the window-button plate,
// the menu highlight, the log box and the window frame itself. The frame used to be rounder (6) than
// the controls (4), which read as two different shapes, and the reader asked for it unified and a
// little tighter.
static constexpr float cornerRadius = 3.0f;
// CreateRoundRectRgn takes the size of the corner ellipse rather than a radius, which is twice as large.
// The region's corner is deliberately one pixel wider than the radius the frame is drawn with: the frame's
// arcs are circles centred a radius in from the corner, so they can never reach the region's own edge, and
// with both at the same radius the corner ended up with a third of a pixel of white between the border and
// the window's silhouette — which reads as a border that does not reach the corner.
static constexpr int windowCornerEllipsePx = (int)(2 * (cornerRadius + 1));
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
static bool expanded = false, topmost = true, busy = false;
// Atomic because the thread that owns the service writes these two as soon as it has taken the service
// over — the window reads them from its own thread to label the button, the tray menu and the paint.
static std::atomic_bool serverRunning{false};
static bool autoRestart = false;          // the tray toggle: bring the service back by itself
static int autoRestartStreak = 0;         // automatic restarts in a row that have not settled yet
static ULONGLONG autoRestartReadyAt = 0;  // when the running service last became ready
static bool mini = false;
static bool stopping = false, updateAvailable = false;
static std::atomic_bool serverExternal{false};   // taken over rather than started here, so stopping asks first
// Whether the system will round the window's corners and draw its border for us. Its corners are
// antialiased, which a window region's are not, so the unfolded window hands its shape over to DWM and
// only the folded chip — which has to be cut out of the window — uses a region and a frame of our own.
static bool systemCorners = false;
static std::atomic_bool serverReady{false};
static State status = State::Missing;
static Button hoverButton = Button::None;
static Button pressedButton = Button::None;   // the button the mouse is holding down, if any
static std::wstring statusTip = L"未安装固件", statusDetail, updateLabel = L"更新", rollbackVersion;
static int logLineCount = 0;
static bool logHovered = false;         // the pointer is over the log box (or a bar is being dragged)
static bool logSelected = false;        // the log has a selection to look at
static int logMaxLineWidth = 0;         // widest line in the retained log, in pixels
static int logMaxLineIndex = -1;        // which line that was, so a trim knows when to rescan
static int logHorizontalOffset = 0;     // pixels the text is scrolled sideways
static int logTextOriginX = INT_MIN;    // x of character 0 while the view is not scrolled
static int logBarGrabOffset = -1;       // where inside the thumb a drag started, -1 when it missed
// Entries that arrived while the reader was dragging a selection out of the log. Writing an entry
// means moving the caret to the end of the text, and doing that under the pointer takes the drag's
// anchor with it, so the selection collapses mid-gesture. The gesture ends when the button comes up.
static std::vector<std::wstring> logDeferred;
static constexpr size_t deferredLogCap = 4096;   // and a drag that lasts absurdly long stops holding
static HWND tooltip;
static TOOLINFOW tipInfo{};
static float scaleFactor = 1.0f;

static const UiRect logRect{logX, buttonTop, buttonWidth, buttonHeight};
static const UiRect statusRect{statusX, buttonTop, indicatorWidth, buttonHeight};
static const UiRect startRect{startX, buttonTop, buttonWidth, buttonHeight};
static const UiRect updateRect{updateX, buttonTop, buttonWidth, buttonHeight};
static const UiRect topRect{topmostX, buttonTop, buttonWidth, buttonHeight};
static const UiRect minRect{minX, buttonTop, titleButtonWidth, buttonHeight};
static const UiRect closeRect{closeX, buttonTop, titleButtonWidth, buttonHeight};
// The whale icon is the fold/unfold handle, the window's drag handle, and now the last thing in the
// unfolded title bar.
static const UiRect iconRect{iconX, iconTop, iconWidth, iconHeight};
static const UiRect miniIconRect{miniIconX, iconTop, iconWidth, iconHeight};
static const UiRect miniLampRect{miniLampX, buttonTop, indicatorWidth, buttonHeight};
// The lamp and the whale sit in different places folded and unfolded, so the paint and hit-test
// paths ask for the rect instead of naming one. Folded they are the chip's own controls shifted to
// the right end of a window that keeps its width.
static UiRect LampRect() {
    return mini ? UiRect{miniOffsetX + miniLampX, buttonTop, indicatorWidth, buttonHeight} : statusRect;
}
static UiRect IconRect() {
    return mini ? UiRect{miniOffsetX + miniIconX, iconTop, iconWidth, iconHeight} : iconRect;
}
// Where the icon is actually drawn inside that rect. Derived rather than a pair of numbers, so the
// drawing cannot be left behind at the old spot when the rect moves.
static UiRect IconDrawRect() {
    UiRect box = IconRect();
    return UiRect{box.x + (box.w - iconDrawSize) / 2, box.y + (box.h - iconDrawSize) / 2,
        iconDrawSize, iconDrawSize};
}

static int Scaled(int n);   // defined with the layout code further down
static void Rounded(GraphicsPath& p, float x, float y, float w, float h, float radius);

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

// What one probe found. Every five-second check is logged, so the result carries enough detail
// to tell the failure modes apart: nothing listening, listening but not answering, or answering
// with something that is not the harness page.
struct HarnessProbe {
    bool answered = false;    // a response arrived at all
    DWORD status = 0;         // HTTP status code, 0 when there was none
    bool recognised = false;  // the body is the DeepSeek Harness page
    unsigned elapsedMs = 0;   // round trip, including the time a timeout costs
};

// The DSH web server answers an unauthenticated request with "dsh web authentication required".
static HarnessProbe ProbeHarness(USHORT port) {
    HarnessProbe probe;
    ULONGLONG started = GetTickCount64();
    HINTERNET session = WinHttpOpen(L"DeepSeekHarnessLauncher/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return probe;
    WinHttpSetTimeouts(session, 600, 600, 1200, 1200);
    if (HINTERNET connect = WinHttpConnect(session, L"127.0.0.1", port, 0)) {
        if (HINTERNET request = WinHttpOpenRequest(connect, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, 0)) {
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                probe.answered = true;
                DWORD status = 0;
                DWORD size = sizeof(status);
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
                    probe.status = status;
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
                probe.recognised = lower.find("dsh web") != std::string::npos ||
                    lower.find("deepseek harness") != std::string::npos;
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    probe.elapsedMs = (unsigned)(GetTickCount64() - started);
    return probe;
}

// One line per health check. It is written every healthProbeIntervalMs, so it stays short, but
// it has to say which failure it is: "the port is gone" and "the port is open and silent" send
// the reader to different places.
static std::wstring HealthProbeMessage(const HarnessProbe& probe, int failures, bool portListening) {
    std::wstring took = std::to_wstring(probe.elapsedMs) + L" ms";
    if (probe.recognised)
        return failures
            ? L"健康检查：恢复正常（HTTP " + std::to_wstring(probe.status) + L"，" + took + L"，此前连续 " +
                std::to_wstring(failures) + L" 次无应答）。"
            : L"健康检查：正常（HTTP " + std::to_wstring(probe.status) + L"，" + took + L"）。";
    std::wstring reason = probe.answered
        ? L"应答不是 DeepSeek Harness 页面（HTTP " + std::to_wstring(probe.status) + L"）"
        : (portListening ? L"端口仍在监听但没有应答" : L"端口已无监听");
    return L"健康检查：" + reason + L"（" + took + L"，连续 " + std::to_wstring(failures) + L"/" +
        std::to_wstring(healthFailureThreshold) + L" 次）。";
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
    service.harness = ProbeHarness(port).recognised;
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

// The `version` field of a package.json, read without executing anything.
static std::wstring JsonVersion(const fs::path& packageJson) {
    std::ifstream file(packageJson, std::ios::binary);
    if (!file) return {};
    std::string json((std::istreambuf_iterator<char>(file)), {});
    size_t key = json.find("\"version\"");
    if (key == std::string::npos) return {};
    size_t colon = json.find(':', key + 9), first = json.find('"', colon);
    if (colon == std::string::npos || first == std::string::npos) return {};
    size_t last = json.find('"', first + 1);
    return last == std::string::npos ? L"" : Utf8(json.substr(first + 1, last - first - 1));
}

static std::wstring Version() { return JsonVersion(PackageJson()); }

// npm writes diagnostics to the same pipe as its answer, and an unknown key in the
// user's .npmrc is quoted verbatim, e.g.
//   npm warn Unknown user config "allow-scripts". This will stop working ...
// Taking the first quoted token would read that config key as a release: the check
// would then report an update on every run, and the button would stay violet forever.
// Only a token shaped like a version may be trusted, so a warning is skipped instead.
static bool LooksLikeVersion(const std::wstring& text) {
    if (text.empty() || text.size() > 64) return false;
    bool digit = false, dot = false;
    for (wchar_t c : text) {
        if (c >= L'0' && c <= L'9') { digit = true; continue; }
        if (c == L'.') { dot = true; continue; }
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'-' || c == L'+') continue;
        return false;
    }
    return digit && dot;
}

static std::wstring QuotedVersion(const std::wstring& output) {
    for (size_t open = output.find(L'"'); open != std::wstring::npos; ) {
        size_t close = output.find(L'"', open + 1);
        if (close == std::wstring::npos) break;
        std::wstring candidate = output.substr(open + 1, close - open - 1);
        if (LooksLikeVersion(candidate)) return candidate;
        open = output.find(L'"', close + 1);
    }
    return {};
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

struct NodeTools { std::wstring node, npm, npmVersion; };
static std::optional<NodeTools> CompleteNode(const fs::path& root) {
    fs::path node = root / L"node.exe";
    fs::path npm = root / L"node_modules" / L"npm" / L"bin" / L"npm-cli.js";
    if (fs::is_regular_file(node) && fs::is_regular_file(npm)) return NodeTools{node.wstring(), npm.wstring(), {}};
    return std::nullopt;
}

static std::wstring PathVariable() {
    DWORD length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring path(length ? length : 1, L'\0');
    if (length) { DWORD used = GetEnvironmentVariableW(L"PATH", path.data(), length); path.resize(used); }
    return path;
}

static void PrependLocalNodeToPath() {
    std::wstring root = LocalNodeRoot().wstring();
    std::wstring path = PathVariable();
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

// npm ships as plain JavaScript, so its version is readable from the package next to
// npm-cli.js without running it. Which npm runs matters: 11.13 rejects `allow-scripts`
// as an unknown .npmrc key while 11.17 accepts it, and the older copy's warning then
// polluted the update check. The launcher therefore prefers the newest npm the chosen
// Node.js can be expected to run, and logs which one it picked.
static std::wstring NpmCliVersion(const std::wstring& cli) {
    return JsonVersion(fs::path(cli).parent_path().parent_path() / L"package.json");
}

static std::wstring NpmPackageRoot(const std::wstring& cli) {
    return fs::path(cli).parent_path().parent_path().wstring();
}

static std::wstring NpmLabel(const NodeTools& tools) {
    return tools.npmVersion.empty() ? L"npm（版本未知）" : L"npm v" + tools.npmVersion;
}

static bool SamePath(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x += 32;
        if (y >= L'A' && y <= L'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

static long MajorOf(const std::wstring& version) {
    long major = 0;
    for (wchar_t c : version) {
        if (c < L'0' || c > L'9') break;
        major = major * 10 + (c - L'0');
    }
    return major;
}

static int CompareVersions(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    for (int component = 0; component < 3; ++component) {
        long left = 0, right = 0;
        while (i < a.size() && a[i] >= L'0' && a[i] <= L'9') left = left * 10 + (a[i++] - L'0');
        while (j < b.size() && b[j] >= L'0' && b[j] <= L'9') right = right * 10 + (b[j++] - L'0');
        if (left != right) return left < right ? -1 : 1;
        if (i < a.size() && a[i] == L'.') ++i;
        if (j < b.size() && b[j] == L'.') ++j;
    }
    return 0;
}

// Whether the release npm advertises should replace what is installed. A different string is not
// enough: the installed build can be newer than what the tag npm answers with — an rc put there by
// hand, or a version installed from a branch — and calling that "发现新版本" with the button lit up
// walks the reader into a downgrade. Equal numbers still count (an rc against its own release, and
// the pre-release tags CompareVersions cannot order); a lower number never does.
static bool RemoteIsNewer(const std::wstring& remote, const std::wstring& installed) {
    if (remote == installed) return false;
    return CompareVersions(remote, installed) >= 0;
}

// Every npm this machine offers: the one paired with the chosen Node.js, the launcher's
// private runtime, the installation holding node.exe, and any directory on PATH.
static std::vector<std::wstring> NpmCandidates(const std::wstring& paired) {
    std::vector<std::wstring> found;
    auto add = [&](const std::wstring& cli) {
        if (cli.empty() || !fs::is_regular_file(cli)) return;
        for (const auto& seen : found) if (SamePath(seen, cli)) return;
        found.push_back(cli);
    };
    add(paired);
    if (auto local = CompleteNode(LocalNodeRoot())) add(local->npm);
    std::wstring node = FindOnPath(L"node.exe");
    if (!node.empty()) { if (auto system = CompleteNode(fs::path(node).parent_path())) add(system->npm); }
    std::wstring path = PathVariable();
    for (size_t start = 0; start < path.size(); ) {
        size_t end = path.find(L';', start);
        std::wstring dir = path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'"')) dir.pop_back();
        if (!dir.empty() && dir.front() == L'"') dir.erase(0, 1);
        if (!dir.empty()) add((fs::path(dir) / L"node_modules" / L"npm" / L"bin" / L"npm-cli.js").wstring());
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return found;
}

// Keep the npm paired with the chosen Node.js unless another copy is newer. The upgrade
// stays inside one major version: a cross-major jump may require a Node.js newer than
// the one that would run it. The decision is logged, so the log always names the npm
// that actually ran.
static void PreferNewestNpm(NodeTools& tools) {
    tools.npmVersion = NpmCliVersion(tools.npm);
    if (tools.npmVersion.empty()) return;
    std::wstring best = tools.npm, bestVersion = tools.npmVersion;
    for (const auto& cli : NpmCandidates(tools.npm)) {
        std::wstring version = NpmCliVersion(cli);
        if (version.empty() || MajorOf(version) != MajorOf(tools.npmVersion)) continue;
        if (CompareVersions(version, bestVersion) > 0) { best = cli; bestVersion = version; }
    }
    if (!SamePath(best, tools.npm)) {
        PostLog(L"发现更新的 npm v" + bestVersion + L"（" + NpmPackageRoot(best) + L"），未使用 v" +
            tools.npmVersion + L"。");
        tools.npm = best;
        tools.npmVersion = bestVersion;
    }
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

// Adopt the launcher's private runtime: put it first on PATH for child processes, take a
// newer npm if the machine has one, and report the Node.js/npm pair that will run.
static NodeTools AdoptLocalNode(NodeTools local) {
    PrependLocalNodeToPath();
    PreferNewestNpm(local);
    std::wstring version = FileVersionString(LocalNodeRoot() / L"node.exe");
    PostLog(L"使用本地 Node.js " + (version.empty() ? L"" : L"v" + version + L" ") + L"和 " + NpmLabel(local) + L"。");
    return local;
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
    std::optional<NodeTools> system;
    if (auto bundled = CompleteNode(fs::path(systemNode).parent_path())) {
        system = bundled;
    } else {
        // Node.js installed without its bundled npm: borrow the npm found on PATH.
        std::wstring npmCmd = FindOnPath(L"npm.cmd");
        if (!npmCmd.empty()) {
            fs::path npm = fs::path(npmCmd).parent_path() / L"node_modules" / L"npm" / L"bin" / L"npm-cli.js";
            if (fs::is_regular_file(npm)) system = NodeTools{systemNode, npm.wstring(), {}};
        }
    }
    if (!system) return std::nullopt;
    PreferNewestNpm(*system);
    std::wstring version = FileVersionString(systemNode);
    PostLog(L"使用系统 Node.js " + (version.empty() ? L"" : L"v" + version + L" ") + L"和 " + NpmLabel(*system) + L"。");
    return system;
}

static std::optional<NodeTools> FindNodeWithoutDownload() {
    if (auto local = CompleteNode(LocalNodeRoot())) return AdoptLocalNode(*local);
    return SystemNode();
}

static NodeTools FindNode() {
    if (auto local = CompleteNode(LocalNodeRoot())) {
        std::wstring version = FileVersionString(LocalNodeRoot() / L"node.exe");
        if (version.empty() || version == pinnedNodeVersion) return AdoptLocalNode(*local);
        // The launcher ships a pinned runtime. Refreshing the private copy keeps the pin
        // meaningful after an upgrade, but a network failure must not break a working copy.
        PostLog(L"本地 Node.js v" + version + L" 与启动器内置的 v" + pinnedNodeVersion + L" 不一致，正在更新运行时…");
        try {
            InstallLocalNode();
        } catch (const std::exception& e) {
            PostLog(L"更新 Node.js 失败：" + ExceptionMessage(e) + L"；继续使用本地 v" + version + L"。");
        }
        if (auto refreshed = CompleteNode(LocalNodeRoot())) return AdoptLocalNode(*refreshed);
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
            // The flag goes up with the message, not with the handler: the port probe below runs in
            // this same pass, and reading the ready banner twice — once here and once there — put two
            // WM_SERVER_READY messages on the queue, and the launcher opened the page twice.
            if (line.find("dsh web: http") != std::string::npos && !closing && !serverReady) {
                serverReady = true;
                PostMessageW(windowHandle, WM_SERVER_READY, 0, 0);
            }
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
            if (ProbeHarness(serverPort).recognised) {
                serverReady = true;
                if (!closing) PostMessageW(windowHandle, WM_SERVER_READY, 0, 0);
            }
        } else if (serverReady && GetTickCount64() >= nextHealth) {
            // The process handle says nothing about the web server inside it, so keep
            // asking the port and report when it stops answering.
            nextHealth = GetTickCount64() + healthProbeIntervalMs;
            // Every check is written to the log, healthy or not: the reader asked to see the
            // five-second heartbeat, and a line that only appears on failure cannot say how long
            // the service has been quiet.
            HarnessProbe probe = ProbeHarness(serverPort);
            if (probe.recognised) {
                int previous = healthFailures;
                healthFailures = 0;
                if (unhealthy) {
                    unhealthy = false;
                    if (!closing) PostMessageW(windowHandle, WM_SERVER_HEALTHY, 0, 0);
                }
                PostLog(HealthProbeMessage(probe, previous, true));
            } else {
                ++healthFailures;
                bool listening = PortOwnerPid(serverPort) != 0;
                if (healthFailures >= healthFailureThreshold && !unhealthy) {
                    unhealthy = true;
                    if (!closing) PostMessageW(windowHandle, WM_SERVER_UNHEALTHY, listening ? 1 : 0, 0);
                }
                PostLog(HealthProbeMessage(probe, healthFailures, listening));
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

// Launcher settings that outlive a run. A small key=value file next to server.log rather than the
// registry, so the launcher stays a folder you can look at: settings.ini holds nothing but the
// toggles the reader flipped. A missing file, a missing key or an unreadable value keeps whatever
// the launcher started with, and unknown keys are ignored so a newer build can add more.
static fs::path SettingsFile() { return runtimeDir.parent_path() / L"settings.ini"; }

static std::optional<bool> ParseSetting(const std::string& text, const std::string& key) {
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t equals = line.find('=');
        if (equals != std::string::npos && line.compare(0, equals, key) == 0) {
            std::string value = line.substr(equals + 1);
            while (!value.empty() && value.front() == ' ') value.erase(0, 1);
            if (value == "1" || value == "true" || value == "yes") return true;
            if (value == "0" || value == "false" || value == "no") return false;
            return std::nullopt;
        }
        if (end == text.size()) break;
        at = end + 1;
    }
    return std::nullopt;
}

static std::string FormatSettings(bool windowTopmost, bool restartAutomatically) {
    return std::string("topmost=") + (windowTopmost ? "1" : "0") + "\n" +
        "autoRestart=" + (restartAutomatically ? "1" : "0") + "\n";
}

static void SaveSettings() {
    std::error_code error;
    fs::create_directories(SettingsFile().parent_path(), error);
    fs::path staging = SettingsFile().wstring() + L".tmp";
    {
        std::ofstream file(staging, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file << FormatSettings(topmost, autoRestart);
        file.flush();
        if (!file) return;
    }
    // Replace rather than truncate in place: a half-written file would lose both toggles.
    if (!MoveFileExW(staging.c_str(), SettingsFile().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error.clear();
        fs::remove(staging, error);
    }
}

static void LoadSettings() {
    std::ifstream file(SettingsFile(), std::ios::binary);
    if (!file) return;
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (auto value = ParseSetting(text, "topmost")) topmost = *value;
    if (auto value = ParseSetting(text, "autoRestart")) autoRestart = *value;
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
    if (what == "check failed") return L"查询 npm 上的最新版本失败；请检查网络或代理后重试。";
    if (what == "version missing") return L"npm 返回的最新版本号无法识别，请稍后重试。";
    if (what.rfind("extract failed: ", 0) == 0) return L"解压 Node.js 压缩包失败（退出码 " + Utf8(what.substr(16)) + L"）。";
    if (what.rfind("install failed:", 0) == 0) return L"安装失败（退出码 " + Utf8(what.substr(16)) + L"）。请查看运行日志。";
    if (what.rfind("port busy: ", 0) == 0) return L"本地端口 " + std::to_wstring((int)serverPort) +
        L" 已被其它程序占用（" + Utf8(what.substr(11)) + L"），已取消启动；请先结束占用该端口的程序。";
    return L"操作失败：" + Utf8(what);
}

static void Worker(Work work, const std::wstring& rollbackTarget) {
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
            // Keep warnings off the stream as well, so the log stays readable.
            DWORD code = RunNode(existing->node,
                {existing->npm, L"view", L"@deepseek-ai/dsh", L"version", L"--json", L"--loglevel=error"},
                &output, updateCheckTimeoutMs);
            if (code != 0) throw std::runtime_error("check failed");
            result->version = QuotedVersion(output);
            if (result->version.empty()) throw std::runtime_error("version missing");
            result->installed = Installed();
            result->update = result->installed && RemoteIsNewer(result->version, Version());
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
            if (rollbackTarget.empty()) throw std::runtime_error("rollback missing");
            InstallHarness(tools, rollbackTarget);
        }
        result->ok = true; result->installed = true; result->version = Version();
    } catch (const std::exception& e) { result->message = ExceptionMessage(e); }
    finish();
}

// EM_GETSEL's return value packs both positions into 16 bits, which is wrong once the
// log passes 64k characters; the pointer form reports the real 32-bit range.
static bool LogSelectionRange(LONG& start, LONG& end) {
    DWORD from = 0, to = 0;
    SendMessageW(logEdit, EM_GETSEL, (WPARAM)&from, (LPARAM)&to);
    start = (LONG)from; end = (LONG)to;
    return end > start;
}

// Both scroll bars are ordinary always-present edit bars, one per direction; only their painting is
// replaced below. Keeping them out of the way until they were needed was tried and cannot work: the
// edit only maintains a bar's range while that bar is shown, so a hidden bar reports a frozen range
// and the box can no longer tell whether new content overflows.
static void InvalidateLogBars();

static void ClearLog() {
    if (!logEdit) return;
    logLineCount = 0;
    logMaxLineWidth = 0;
    logMaxLineIndex = -1;
    logHorizontalOffset = 0;
    logDeferred.clear();
    SetWindowTextW(logEdit, L"");
    InvalidateLogBars();
}

// ---------------------------------------------------------------- log scroll bars
//
// The log box shows a bare line where a scroll bar would be: no track, no arrow buttons, no system
// chrome at all. The control carries no scroll bar styles, so there is no non-client strip to hide
// and the text takes the whole panel except a narrow zone next to the painted border. What the bars
// need is measured here rather than read from the control's scroll bars:
//
//   * vertical: line count, line height and the first visible line, all exact;
//   * horizontal: the longest line, tracked as lines arrive, and the pixel offset, read back
//     through the position of character 0 whenever it is representable.
//
// The two bars are small windows of our own placed in that zone. They paint the line and turn
// dragging and the wheel into scrolling.

static constexpr int logBarThickness = 4;         // the width of the line
static constexpr int logBarEdgeGap = 1;           // the line sits this close to the box border
static constexpr int logBarShortestThumb = 24;    // so a huge log still leaves something to grab
static HWND logVerticalBar = nullptr, logHorizontalBar = nullptr;

// A bar's numbers in the units the launcher uses: lines for the vertical bar, pixels for the
// horizontal one. Painting and dragging both work from this.
struct LogBarMetrics {
    int range = 0, page = 0, position = 0;   // content size, visible size, current offset
    int track = 0, thumb = 0, offset = 0;    // and the same three mapped onto the bar
    int scrollable = 0;
};

static int LogCharWidth();

static int LogLineHeight() {
    static int height = 0;
    if (height > 0 || !logEdit) return height > 0 ? height : 12;
    HDC dc = GetDC(logEdit);
    if (!dc) return 12;
    HFONT font = (HFONT)SendMessageW(logEdit, WM_GETFONT, 0, 0);
    HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW metrics{};
    // tmHeight alone: this is the spacing the edit packs its lines with. Adding tmExternalLeading
    // looks more correct but is not what the control does — measured with NSimSun it fits eight
    // 12px lines into the 106px box, while tmHeight + leading predicted only seven.
    if (GetTextMetricsW(dc, &metrics)) height = metrics.tmHeight;
    if (previous) SelectObject(dc, previous);
    ReleaseDC(logEdit, dc);
    if (height <= 0) height = 12;
    return height;
}

static int LogContentWidth() { return logMaxLineWidth; }

// Width of a line of log text in pixels, using the control's own font. Character counts are not
// enough here: a Chinese glyph is twice as wide as a Latin one.
static int LogLineWidth(const std::wstring& text) {
    if (!logEdit || text.empty()) return 0;
    HDC dc = GetDC(logEdit);
    if (!dc) return 0;
    HFONT font = (HFONT)SendMessageW(logEdit, WM_GETFONT, 0, 0);
    HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &size);
    if (previous) SelectObject(dc, previous);
    ReleaseDC(logEdit, dc);
    return size.cx;
}

// Walking every line only happens when the widest one was among those just dropped, so an ordinary
// append costs nothing.
static void RescanLogLineWidths() {
    if (!logEdit) return;
    int lines = (int)SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0);
    logMaxLineWidth = 0;
    logMaxLineIndex = lines > 0 ? 0 : -1;
    std::wstring buffer;
    for (int line = 0; line < lines; ++line) {
        int start = (int)SendMessageW(logEdit, EM_LINEINDEX, (WPARAM)line, 0);
        if (start < 0) continue;
        int length = (int)SendMessageW(logEdit, EM_LINELENGTH, (WPARAM)start, 0);
        if (length <= 0) continue;
        buffer.assign((size_t)length + 1, L'\0');
        *reinterpret_cast<WORD*>(buffer.data()) = (WORD)length;   // EM_GETLINE wants the size up front
        int copied = (int)SendMessageW(logEdit, EM_GETLINE, (WPARAM)line, (LPARAM)buffer.data());
        if (copied <= 0) continue;
        buffer.resize((size_t)copied);
        int width = LogLineWidth(buffer);
        if (width > logMaxLineWidth) { logMaxLineWidth = width; logMaxLineIndex = line; }
    }
}

// Character 0 sits at the text's own left inset when nothing is scrolled and moves left by exactly
// the offset. The value comes back as a short, so a very large offset would not be representable and
// the tracked one is kept instead — but the control caps its own horizontal scroll range well inside
// that: measured, a 200k-character line stops at x = -6852 and further EM_LINESCROLL has no effect, so
// the read is always representable. EM_GETSCROLLPOS and GetScrollInfo(SB_HORZ) are no help — a
// multiline edit without WS_HSCROLL answers neither.
static void SyncLogHorizontalOffset() {
    if (!logEdit) return;
    int x = (int)(short)LOWORD(SendMessageW(logEdit, EM_POSFROMCHAR, 0, 0));
    if (logTextOriginX == INT_MIN) { logTextOriginX = x; return; }
    if (x <= logTextOriginX && x > -32000) logHorizontalOffset = logTextOriginX - x;
}

static bool LogBarMetricsFor(int bar, int track, LogBarMetrics& metrics) {
    if (!logEdit || track <= 0) return false;
    RECT client{};
    if (!GetClientRect(logEdit, &client)) return false;
    if (bar == SB_VERT) {
        int lines = (int)SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0);
        int lineHeight = LogLineHeight();
        if (lines <= 0 || lineHeight <= 0) return false;
        metrics.range = lines;
        metrics.page = client.bottom / lineHeight;
        if (metrics.page < 1) metrics.page = 1;
        metrics.position = (int)SendMessageW(logEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
    } else {
        metrics.range = LogContentWidth();
        metrics.page = client.right;
        metrics.position = logHorizontalOffset;
    }
    if (metrics.range <= metrics.page) return false;   // everything fits: no line at all
    metrics.scrollable = metrics.range - metrics.page;
    if (metrics.position > metrics.scrollable) metrics.position = metrics.scrollable;
    if (metrics.position < 0) metrics.position = 0;
    metrics.track = track;
    metrics.thumb = (int)((long long)track * metrics.page / metrics.range);
    int shortest = Scaled(logBarShortestThumb);
    if (metrics.thumb < shortest) metrics.thumb = shortest;
    if (metrics.thumb > track) metrics.thumb = track;
    metrics.offset = (int)((long long)(track - metrics.thumb) * metrics.position / metrics.scrollable);
    return true;
}

static HBRUSH LogBackgroundBrush() {
    return logBackground ? logBackground : (HBRUSH)GetStockObject(WHITE_BRUSH);
}

// The log font is fixed pitch, so pixels and characters convert through one number.
static int LogCharWidth() {
    static int width = 0;
    if (width > 0 || !logEdit) return width > 0 ? width : 8;
    HDC dc = GetDC(logEdit);
    if (!dc) return 8;
    HFONT font = (HFONT)SendMessageW(logEdit, WM_GETFONT, 0, 0);
    HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    SIZE size{};
    if (GetTextExtentPoint32W(dc, L"0123456789", 10, &size) && size.cx > 0) width = (size.cx + 5) / 10;
    if (previous) SelectObject(dc, previous);
    ReleaseDC(logEdit, dc);
    if (width <= 0) width = 8;
    return width;
}

static void PaintLogBarLine(HDC dc, HWND cover, int bar) {
    RECT client{};
    GetClientRect(cover, &client);
    FillRect(dc, &client, LogBackgroundBrush());
    // Nothing but the box colour until the reader is working with the log.
    if (!logHovered && !logSelected) return;
    bool vertical = bar == SB_VERT;
    LogBarMetrics metrics;
    if (!LogBarMetricsFor(bar, vertical ? client.bottom : client.right, metrics)) return;
    int thickness = Scaled(logBarThickness);
    int span = vertical ? client.right : client.bottom;
    if (thickness > span) thickness = span;
    // The line hugs the outside of the strip, next to the box border, instead of floating in the
    // middle of it: what is left over then reads as the box's inner margin rather than as blank
    // space on both sides of the bar.
    int edge = span - thickness - Scaled(logBarEdgeGap);
    if (edge < 0) edge = 0;
    RECT line{};
    if (vertical) {
        line = {edge, metrics.offset, edge + thickness, metrics.offset + metrics.thumb};
    } else {
        line = {metrics.offset, edge, metrics.offset + metrics.thumb, edge + thickness};
    }
    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    GraphicsPath path;
    Rounded(path, (float)line.left, (float)line.top, (float)(line.right - line.left),
        (float)(line.bottom - line.top), thickness / 2.0f);
    SolidBrush brush(Color(203, 213, 225));
    graphics.FillPath(&brush, &path);
}

static void InvalidateLogBars() {
    if (logVerticalBar) InvalidateRect(logVerticalBar, nullptr, FALSE);
    if (logHorizontalBar) InvalidateRect(logHorizontalBar, nullptr, FALSE);
}

// The bars belong to the reader's attention, not to the panel's looks: the line is painted only while
// the pointer is over the log box, or while something is selected to look at. Nothing has to be polled
// for that — the box and the two bars are separate windows, so each of them reports the pointer
// arriving and leaving, which a system scroll bar inside the control's non-client area could not.
static bool LogBoxContainsCursor() {
    if (!logEdit) return false;
    RECT box{};
    if (!GetWindowRect(logEdit, &box)) return false;
    for (HWND bar : {logVerticalBar, logHorizontalBar}) {
        RECT barRect{};
        if (bar && GetWindowRect(bar, &barRect)) UnionRect(&box, &box, &barRect);
    }
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return false;
    return PtInRect(&box, cursor) != 0;
}

static void RefreshLogBars() {
    if (!logEdit) return;
    LONG start = 0, end = 0;
    bool selected = LogSelectionRange(start, end);
    bool dragging = (logVerticalBar && GetCapture() == logVerticalBar) ||
                    (logHorizontalBar && GetCapture() == logHorizontalBar);
    bool hovered = dragging || LogBoxContainsCursor();
    logHovered = hovered;
    logSelected = selected;
    // Repainted whether or not the bars just appeared: the keyboard and the caret move the view
    // without the launcher asking, so a line left at its old place while the text scrolled behind it
    // is the alternative to redrawing two seven-pixel strips.
    InvalidateLogBars();
}

// EM_LINESCROLL counts characters, and a Chinese glyph is two cells wide, so a single step can land
// short of the pixel target: ask again until the offset is close enough. The tracked offset is read
// back from the control on every step, so what it lands on is what the thumb then reports.
static void ScrollLogToOffset(int target) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        int step = (target - logHorizontalOffset) / LogCharWidth();
        if (!step) break;
        SendMessageW(logEdit, EM_LINESCROLL, step, 0);
        SyncLogHorizontalOffset();
    }
}

// Put the line where the pointer asks for, in the launcher's own units: lines for the vertical bar,
// pixels (through the fixed pitch width) for the horizontal one.
static void DragLogBarTo(HWND cover, int bar, int position) {
    RECT client{};
    GetClientRect(cover, &client);
    bool vertical = bar == SB_VERT;
    LogBarMetrics metrics;
    if (!LogBarMetricsFor(bar, vertical ? client.bottom : client.right, metrics)) return;
    int travel = metrics.track - metrics.thumb;
    if (travel <= 0) return;
    // A press that landed on the thumb keeps hold of the point it took: pulling the middle of the thumb
    // onto the pointer first made the content jump by half a thumb before it started following.
    int along = position - (logBarGrabOffset >= 0 ? logBarGrabOffset : metrics.thumb / 2);
    if (along < 0) along = 0;
    if (along > travel) along = travel;
    int target = (int)((long long)metrics.scrollable * along / travel);
    if (vertical) {
        int delta = target - metrics.position;
        if (delta) SendMessageW(logEdit, EM_LINESCROLL, 0, delta);
    } else {
        ScrollLogToOffset(target);
    }
    InvalidateLogBars();
}

static LRESULT CALLBACK LogBarProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    int bar = hwnd == logVerticalBar ? SB_VERT : SB_HORZ;
    switch (message) {
    // No WM_ERASEBKGND shortcut here: claiming the background is already erased while the window
    // has no background brush of its own leaves newly exposed areas uninitialised, and whatever is
    // underneath — the system scroll bar — shows through them.
    case WM_PRINTCLIENT: {
        PaintLogBarLine((HDC)wp, hwnd, bar);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        // Through a memory bitmap: filling the strip and then drawing the line straight onto the
        // window is two separate steps, and during a drag that reads as a white blink at every step.
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ previous = SelectObject(buffer, bitmap);
        PaintLogBarLine(buffer, hwnd, bar);
        BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, previous);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(hwnd, &ps);
        return 0;
    }
    // Clicking a bar must not take the caret away from the log, or the keyboard stops scrolling it.
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: {
        if (logEdit) SetFocus(logEdit);
        SetCapture(hwnd);
        int position = bar == SB_VERT ? GET_Y_LPARAM(lp) : GET_X_LPARAM(lp);
        // Landing on the line itself holds on to the point that was pressed; a press on the empty track
        // still brings the line's middle to the pointer.
        RECT client{};
        GetClientRect(hwnd, &client);
        LogBarMetrics metrics;
        logBarGrabOffset = -1;
        if (LogBarMetricsFor(bar, bar == SB_VERT ? client.bottom : client.right, metrics) &&
            position >= metrics.offset && position < metrics.offset + metrics.thumb)
            logBarGrabOffset = position - metrics.offset;
        DragLogBarTo(hwnd, bar, position);
        RefreshLogBars();
        return 0;
    }
    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd) DragLogBarTo(hwnd, bar, bar == SB_VERT ? GET_Y_LPARAM(lp) : GET_X_LPARAM(lp));
        else {
            TRACKMOUSEEVENT leaving{sizeof(leaving), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&leaving);
            RefreshLogBars();
        }
        return 0;
    case WM_MOUSELEAVE:
        RefreshLogBars();
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        logBarGrabOffset = -1;
        RefreshLogBars();
        return 0;
    case WM_MOUSEWHEEL:
        // The wheel over a bar is still meant for the log.
        if (logEdit) { SendMessageW(logEdit, WM_MOUSEWHEEL, wp, lp); RefreshLogBars(); }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

// The two bars fill the zone next to the painted border: a vertical one beside the text, a
// horizontal one under it. Both are siblings of the control raised above it.
static void PositionLogBars() {
    if (!logEdit || !logVerticalBar || !logHorizontalBar) return;
    int right = Scaled(logSide + logEditWidth);
    int bottom = Scaled(logTop + logEditHeight);
    int zone = Scaled(logBarZone);
    // HWND_TOP and not SWP_NOZORDER: a newly created child is not guaranteed to sit above its
    // siblings, and a bar that ends up below the control would be invisible.
    SetWindowPos(logVerticalBar, HWND_TOP, right, Scaled(logTop), zone, Scaled(logEditHeight), SWP_NOACTIVATE);
    SetWindowPos(logHorizontalBar, HWND_TOP, Scaled(logSide), bottom, Scaled(logEditWidth), zone, SWP_NOACTIVATE);
    // Resizing and raising happen outside WM_PAINT, so paint both covers right away rather than
    // leaving the new area to be filled whenever Windows gets round to it.
    for (HWND cover : {logVerticalBar, logHorizontalBar}) {
        InvalidateRect(cover, nullptr, TRUE);
        UpdateWindow(cover);
    }
}

static void CreateLogBars(HWND parent) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW info{sizeof(info)};
        info.lpfnWndProc = LogBarProc;
        info.hInstance = GetModuleHandleW(nullptr);
        info.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        info.lpszClassName = L"DeepSeekHarnessLogBar";
        // A background brush is what makes the window opaque: exposed parts are erased to it before
        // WM_PAINT runs, so nothing from underneath can ever show through.
        info.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        RegisterClassExW(&info);
        registered = true;
    }
    logVerticalBar = CreateWindowExW(0, L"DeepSeekHarnessLogBar", L"", WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    logHorizontalBar = CreateWindowExW(0, L"DeepSeekHarnessLogBar", L"", WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

static std::wstring ComposeLogEntry(const std::wstring& line) {
    SYSTEMTIME now; GetLocalTime(&now);
    // No trailing space after the bracket: this font draws a space half a Chinese glyph wide, which
    // reads as a conspicuous gap between the timestamp and a Chinese message.
    wchar_t stamp[32]; wsprintfW(stamp, L"[%02d:%02d:%02d]", now.wHour, now.wMinute, now.wSecond);
    // One entry is one line, whatever arrived. A break inside the text is a line to the control when it
    // comes as CRLF, and the line count, the trim and the widest-line record are all line numbers — one
    // entry that carried a break would put all three out of step with the text. Measured, a lone CR or
    // LF in the middle is stored but does not break the line; a CR still means nothing on screen and
    // comes out in a copy as a stray control character, and npm's progress output really does write one.
    // A tab is flattened for its own reason: the control expands it to the next tab stop, so a line
    // holding one is wider on screen than the width measured for it, and the horizontal bar could not
    // reach its end.
    std::wstring entry = stamp;
    for (wchar_t c : line) entry += (c == L'\r' || c == L'\n' || c == L'\t') ? L' ' : c;
    return entry;
}

// One entry into the box. Appending is a two-step move — put the caret at the end of the text, then
// replace the (empty) selection there — and both steps used to take whatever the reader had selected
// with them. The log streams while the service is talking, so a selection made to be copied was
// regularly gone before the menu came up. The reader's range is therefore read before the insert and
// put back after it, and the view is not scrolled to the caret in that case: it stays where the
// reader left it, at the end of their own selection.
static void WriteLogEntry(const std::wstring& entry) {
    // IsWindow as well: losing the capture is also what a control being destroyed reports, and that
    // path reaches here through FlushDeferredLog with a handle that is on its way out.
    if (!logEdit || !IsWindow(logEdit)) return;
    // The separator goes in front of the entry, never after it. Ending the text with a newline leaves
    // an empty line at the bottom that the view is pinned to, so the last line of the panel shows
    // nothing but white — the space the reader sees going to waste. This way the newest line is the
    // last line of the text and sits at the bottom of the box.
    bool first = GetWindowTextLengthW(logEdit) == 0;
    std::wstring appended = first ? entry : (L"\r\n" + entry);
    DWORD selectedFrom = 0, selectedTo = 0;
    SendMessageW(logEdit, EM_GETSEL, (WPARAM)&selectedFrom, (LPARAM)&selectedTo);
    bool hadSelection = selectedTo > selectedFrom;
    // Append instead of rebuilding the whole box: with a 50k line cap a full
    // SetWindowText per line would be quadratic.
    int length = GetWindowTextLengthW(logEdit);
    SendMessageW(logEdit, EM_SETSEL, length, length);
    SendMessageW(logEdit, EM_REPLACESEL, FALSE, (LPARAM)appended.c_str());
    // The control's own count, not a running total: the trim below cuts whole lines out by
    // EM_LINEINDEX and the widest-line record is a line number, so this has to be the number the
    // control itself indexes lines with.
    logLineCount = (int)SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0);
    // The horizontal bar needs the widest line, in pixels: a Chinese glyph is two cells wide, so a
    // character count would not describe the line's width.
    int width = LogLineWidth(entry);
    if (width > logMaxLineWidth) { logMaxLineWidth = width; logMaxLineIndex = logLineCount - 1; }
    int dropped = 0;   // characters the trim below took off the front, which a kept range shifts by
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
            dropped = cut;
            // Only a trim that drops the longest line costs a rescan of what is left.
            if (logMaxLineIndex >= 0) {
                if (logMaxLineIndex < excess) RescanLogLineWidths();
                else logMaxLineIndex -= excess;
            }
        }
    }
    if (hadSelection) {
        // Back to the reader's own range, and no EM_SCROLLCARET: the two ends of it are inside what
        // the box is already showing, so the view does not move either.
        LONG from = (LONG)selectedFrom - dropped, to = (LONG)selectedTo - dropped;
        // A range that ran to the end of the text was "everything so far" rather than a fixed stretch:
        // it grows with the entry, so Ctrl+A followed by 复制 a while later still holds the whole log
        // and the highlight does not visibly stop covering the newest lines.
        if ((LONG)selectedTo == (LONG)length) to = (LONG)length + (LONG)appended.size() - dropped;
        if (from < 0) from = 0;
        if (to < from) to = from;
        SendMessageW(logEdit, EM_SETSEL, from, to);
    } else {
        // Park the caret at the start of the last line: it keeps the view pinned to the
        // bottom without dragging it sideways for long lines.
        int lastLine = (int)SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0) - 1;
        int lastStart = (int)SendMessageW(logEdit, EM_LINEINDEX, (WPARAM)lastLine, 0);
        int keepOffset = logHorizontalOffset;
        if (lastStart >= 0) SendMessageW(logEdit, EM_SETSEL, lastStart, lastStart);
        SendMessageW(logEdit, EM_SCROLLCARET, 0, 0);
        // Putting the caret on the start of the line drags the view back to the left edge, and with the
        // log streaming that happened again with every entry: a reader who had scrolled sideways to the
        // right half of a long line could never stay there. Their own position is measured again and put
        // back.
        if (keepOffset > 0) {
            SyncLogHorizontalOffset();
            ScrollLogToOffset(keepOffset);
        }
    }
    SyncLogHorizontalOffset();
    InvalidateLogBars();
}

// Entries held back while a drag is in progress. Called as soon as the button comes up (and before
// the next entry is written), so a queued line waits no longer than the gesture it interrupted.
static void FlushDeferredLog() {
    if (logDeferred.empty() || !logEdit) return;
    std::vector<std::wstring> queued;
    queued.swap(logDeferred);
    for (const std::wstring& entry : queued) WriteLogEntry(entry);
}

static void AppendLog(const std::wstring& line) {
    if (!logEdit) return;
    // The left button down means the reader is marking text: the control holds the capture for the
    // whole drag, and the caret must not be moved out from under it until the button is released.
    // The cap only exists so that a drag nobody ever finishes cannot queue without bound; past it
    // the log wins, because losing the tail of the output is worse than a broken drag.
    if (GetCapture() == logEdit && logDeferred.size() < deferredLogCap) {
        logDeferred.push_back(ComposeLogEntry(line));
        return;
    }
    FlushDeferredLog();
    WriteLogEntry(ComposeLogEntry(line));
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
// The items are owner drawn so the hover/selection background is exactly the same width for both of
// them, with the same margin from the popup's frame on every side.
static constexpr int menuItemWidth = 78, menuItemHeight = 26, menuItemInset = 3;
// An owner drawn item gets its label and its position through itemData, so the layout does not have to
// guess which item it is looking at from the band the shell handed over.
struct LogMenuItemLabel { const wchar_t* label; int index; };
static const LogMenuItemLabel menuCopyItem{L"复制", 0}, menuClearItem{L"清除", 1};
static constexpr UINT_PTR menuCopyCommand = 1, menuClearCommand = 2;

// Popup menus are a "#32768" window owned by whichever process shows them. Rounding its
// frame and giving it the launcher's border colour makes the menus match the window.
static void RoundMenuFrame(HWND popup) {
    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(popup, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    COLORREF border = windowBorderColor;
    DwmSetWindowAttribute(popup, DWMWA_BORDER_COLOR, &border, sizeof(border));
    SetWindowPos(popup, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// That window does not exist yet while WM_INITMENUPOPUP runs — it is created just before
// the menu is shown — so a short-lived helper waits for it instead. It gives up after a
// few seconds and never blocks the UI thread, which is inside TrackPopupMenu by then.
static void RoundOwnedMenuFrameSoon() {
    std::thread([] {
        for (int attempt = 0; attempt < 200; ++attempt) {
            HWND popup = FindWindowW(L"#32768", nullptr);
            DWORD pid = 0;
            if (popup) GetWindowThreadProcessId(popup, &pid);
            if (popup && pid == GetCurrentProcessId()) { RoundMenuFrame(popup); return; }
            Sleep(15);
        }
    }).detach();
}

static void ShowLogMenu(HWND owner, LPARAM screenPosition) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_OWNERDRAW, menuCopyCommand, (LPCWSTR)&menuCopyItem);
    AppendMenuW(menu, MF_OWNERDRAW, menuClearCommand, (LPCWSTR)&menuClearItem);
    if (logBackground) {
        MENUINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = MIM_BACKGROUND;
        info.hbrBack = logBackground;
        SetMenuInfo(menu, &info);
    }
    LONG from = 0, to = 0;
    if (!LogSelectionRange(from, to)) EnableMenuItem(menu, menuCopyCommand, MF_BYCOMMAND | MF_GRAYED);
    POINT point{};
    if (screenPosition == (LPARAM)-1) GetCursorPos(&point);
    else { point.x = GET_X_LPARAM(screenPosition); point.y = GET_Y_LPARAM(screenPosition); }
    SetForegroundWindow(windowHandle);
    RoundOwnedMenuFrameSoon();
    int command = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        point.x, point.y, 0, windowHandle, nullptr);
    DestroyMenu(menu);
    PostMessageW(windowHandle, WM_NULL, 0, 0);
    if (command == (int)menuCopyCommand) SendMessageW(logEdit, WM_COPY, 0, 0);
    else if (command == (int)menuClearCommand) ClearLog();
}

static void MeasureLogMenuItem(MEASUREITEMSTRUCT* measure) {
    measure->itemWidth = Scaled(menuItemWidth);
    measure->itemHeight = Scaled(menuItemHeight);
}

// The filled box, inset the same amount from every side of the item it sits in — one number for both
// axes, so the gap above the first item matches the gap to its left.
static RECT HighlightRect(const RECT& box, int inset) {
    return RECT{box.left + inset, box.top + inset, box.right - inset, box.bottom - inset};
}

// The item's own band, in the popup's client area. The shell's bands are not laid out squarely inside
// the popup (measured on the reported menu: 6px from the left edge but 5px from the right, 4px above
// the first item and 1px below the last), so the items tile the client from its top edge instead. The
// only margin left then is the one the highlight applies, and it is the same on all four sides.
// One item's band in the popup's client area: the items tile the client from its top edge, and the
// last one takes whatever is left. Pure geometry, so the equal-margin rule can be checked without a
// live popup.
static RECT MenuItemBand(const RECT& client, int count, int index) {
    int height = client.bottom - client.top;
    if (count <= 0 || index < 0 || index >= count || height <= 0) return client;
    int band = height / count;
    if (band <= 0) return client;
    RECT box{client.left, client.top + index * band, client.right, client.top + (index + 1) * band};
    if (index + 1 == count) box.bottom = client.bottom;
    return box;
}

static RECT LogMenuItemBox(const DRAWITEMSTRUCT* draw, int index) {
    RECT box = draw->rcItem;
    HWND popup = WindowFromDC(draw->hDC);
    RECT client{};
    if (!popup || !GetClientRect(popup, &client)) return box;
    HMENU menu = (HMENU)SendMessageW(popup, MN_GETHMENU, 0, 0);
    int count = menu ? GetMenuItemCount(menu) : 0;
    if (client.right <= client.left || count <= 0 || index < 0 || index >= count) return box;
    return MenuItemBand(client, count, index);
}

static void DrawLogMenuItem(const DRAWITEMSTRUCT* draw) {
    const LogMenuItemLabel* item = (const LogMenuItemLabel*)draw->itemData;
    if (!item || !item->label) return;
    bool selected = (draw->itemState & ODS_SELECTED) != 0;
    bool disabled = (draw->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    RECT box = LogMenuItemBox(draw, item->index);
    HBRUSH background = logBackground ? logBackground : (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(draw->hDC, &box, background);

    RECT highlight = HighlightRect(box, Scaled(menuItemInset));
    Graphics g(draw->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    GraphicsPath path;
    // Filled, not stroked, so the box is not shrunk by a pen width: both insets stay equal.
    Rounded(path, (float)highlight.left, (float)highlight.top,
        (float)(highlight.right - highlight.left), (float)(highlight.bottom - highlight.top),
        cornerRadius);
    SolidBrush fill(selected ? Color(219,234,254) : Color(255,255,255));
    if (selected) g.FillPath(&fill, &path);

    HFONT font = CreateFontW(-Scaled(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    HGDIOBJ previous = SelectObject(draw->hDC, font);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, disabled ? RGB(160,170,185) : selected ? RGB(30,64,175) : RGB(35,50,76));
    RECT text = box; text.top += Scaled(1); text.bottom += Scaled(1);
    DrawTextW(draw->hDC, item->label, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(draw->hDC, previous);
    DeleteObject(font);
}

static int VisibleLogLines() {
    int lineHeight = LogLineHeight();
    RECT client{};
    if (!logEdit || lineHeight <= 0 || !GetClientRect(logEdit, &client)) return 1;
    int visible = client.bottom / lineHeight;
    return visible > 0 ? visible : 1;
}

static LRESULT CALLBACK LogEditProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (message) {
    case WM_CONTEXTMENU: ShowLogMenu(hwnd, lp); return 0;
    // A control without scroll bar styles ignores the wheel, so the launcher scrolls it. How far one
    // notch goes is the user's own setting.
    case WM_MOUSEWHEEL: {
        UINT perNotch = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &perNotch, 0);
        // The remainder is kept. A precise wheel or a touchpad reports a fraction of a notch — 60, 30,
        // even 15 — and dividing that by WHEEL_DELTA gives zero: the whole gesture scrolled nothing at
        // all, and the wheel is the only way to scroll this control.
        static int wheelRemainder = 0;
        wheelRemainder += GET_WHEEL_DELTA_WPARAM(wp);
        int notches = wheelRemainder / WHEEL_DELTA;
        wheelRemainder -= notches * WHEEL_DELTA;
        int step = perNotch == WHEEL_PAGESCROLL ? VisibleLogLines() : (int)perNotch;
        if (notches && step > 0) {
            SendMessageW(hwnd, EM_LINESCROLL, 0, -notches * step);
            RefreshLogBars();   // the wheel moves the view as well as a drag does
        }
        return 0;
    }
    case WM_KEYDOWN:
        // The stock edit menu is gone, so select-all has to be provided here. The async
        // state covers key messages that were posted rather than generated by real input.
        if (wp == 'A' && ((GetKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_CONTROL) & 0x8000))) {
            SendMessageW(hwnd, EM_SETSEL, 0, (LPARAM)-1);
            RefreshLogBars();
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        // The pointer arriving is one of the two things that bring the bars out. The move itself must
        // go on to the control: an edit extends a selection by handling the moves between the button
        // going down and coming up, so swallowing them here left a drag with nothing selected at all
        // ("日志框里无法进行选择复制了") — only the keyboard and the menu could still make a selection.
        TRACKMOUSEEVENT leaving{sizeof(leaving), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&leaving);
        RefreshLogBars();
        break;
    }
    case WM_MOUSELEAVE:
        RefreshLogBars();
        return 0;
    case WM_KEYUP:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        // The caret moving can scroll the view sideways without the launcher asking, so the tracked
        // offset is refreshed from the control itself. A click or a key can also change the selection,
        // which is the other thing that brings the bars out.
        break;
    case WM_CAPTURECHANGED:
        // The gesture can end without a button-up ever reaching the control: Alt+Tab, a system prompt,
        // the panel folding away. The entries it held back are due at that moment, not at whatever
        // line the service happens to print next.
        FlushDeferredLog();
        break;
    }
    LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
    switch (message) {
    case WM_KEYUP:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        // A drag that just ended is also the moment the entries it held back are due; the release
        // has been through the control by now, so the selection is final.
        if (message == WM_LBUTTONUP) FlushDeferredLog();
        SyncLogHorizontalOffset();
        RefreshLogBars();
        break;
    }
    return result;
}

// Single place that defines the log control, so the behaviour under test is the same
// one the window creates.
static int Scaled(int n);
static void CreateLogEdit(HWND parent) {
    // No WS_VSCROLL/WS_HSCROLL: the launcher draws and drives its own bars, so the control has no
    // non-client strip and the text box can use the whole panel. WS_CLIPSIBLINGS still matters,
    // because the bars are siblings that sit on top of this control.
    logEdit = CreateWindowExW(0,L"EDIT",L"",
        WS_CHILD|WS_CLIPSIBLINGS|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_AUTOHSCROLL|ES_NOHIDESEL,
        0,0,0,0,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
    // Ask for NSimSun with plain pitch flags: SimSun's family bits are not "fixed pitch", so asking
    // for fixed pitch would make Windows substitute a different font. If the face is not there, fall
    // back to Consolas rather than let GDI pick something proportional.
    auto makeFont = [](const wchar_t* face) {
        return CreateFontW(-Scaled(logFontHeight),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,face);
    };
    HFONT font = makeFont(logFontFace);
    {
        HDC dc = GetDC(logEdit);
        if (dc) {
            HGDIOBJ previous = SelectObject(dc, font);
            wchar_t face[64]{};
            GetTextFaceW(dc, 64, face);
            if (previous) SelectObject(dc, previous);
            ReleaseDC(logEdit, dc);
            // A missing family comes back with a different name; the localised name is accepted too.
            if (wcscmp(face, logFontFace) != 0 && wcscmp(face, L"\u65B0\u5B8B\u4F53") != 0 &&
                wcscmp(face, L"\u5B8B\u4F53") != 0) {
                DeleteObject(font);
                font = makeFont(logFontFallback);
            }
        }
    }
    SendMessageW(logEdit,WM_SETFONT,(WPARAM)font,TRUE);
    logBackground = CreateSolidBrush(RGB(255,255,255));
    logDeferred.clear();
    SetWindowSubclass(logEdit, LogEditProc, 1, 0);
    // A multiline edit defaults to a 30k character limit; the line cap is the only limit
    // this box should have, otherwise appends fail silently once it is hit.
    SendMessageW(logEdit, EM_SETLIMITTEXT, 0, 0);
    // The bars come last so they sit above the edit and hide the system bars it keeps underneath.
    CreateLogBars(parent);
}

// ---------------------------------------------------------------- tray icon

static int Scaled(int n);   // defined with the layout code below

static constexpr UINT WM_TRAYICON = WM_APP + 20;
static constexpr UINT trayIconId = 1;
static NOTIFYICONDATAW trayIconData{};
static bool trayIconAdded = false;
static UINT taskbarCreatedMessage = 0;
static void OnClick(Button button);
static void ToggleAutoRestart();

static std::wstring StatusName() {
    switch (status) {
    case State::Running: return autoRestart ? L"已启动，自动重启已开启" : L"已启动";
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

// The tray menu items and the popup are built together here, on their own, so the regression checks
// can look at the items without ever showing the popup.
static constexpr UINT trayToggleWindow = 1, trayToggleService = 2, trayExit = 3, trayToggleAutoRestart = 4;
static HMENU BuildTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return nullptr;
    AppendMenuW(menu, MF_STRING, trayToggleWindow, IsWindowVisible(windowHandle) ? L"隐藏窗口" : L"显示窗口");
    AppendMenuW(menu, MF_STRING, trayToggleService, serverRunning ? L"停止服务" : L"启动服务");
    if (busy) EnableMenuItem(menu, trayToggleService, MF_BYCOMMAND | MF_GRAYED);
    AppendMenuW(menu, MF_STRING | (autoRestart ? MF_CHECKED : MF_UNCHECKED), trayToggleAutoRestart, L"自动重启");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, trayExit, L"退出启动器");
    return menu;
}

static void ShowTrayMenu() {
    HMENU menu = BuildTrayMenu();
    if (!menu) return;
    POINT cursor{};
    GetCursorPos(&cursor);
    // Unconditionally, and before the popup: a notification-icon menu needs its owner to be the
    // foreground window when it opens, or clicking outside the menu leaves it on screen. The usual way
    // in is a right click on the tray icon while the window is hidden, which the old test skipped —
    // exactly the case the rule exists for.
    SetForegroundWindow(windowHandle);
    RoundOwnedMenuFrameSoon();
    int command = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        cursor.x, cursor.y, 0, windowHandle, nullptr);
    DestroyMenu(menu);
    PostMessageW(windowHandle, WM_NULL, 0, 0);
    if (command == trayToggleWindow) ToggleLauncherWindow();
    else if (command == trayToggleService) OnClick(Button::Start);   // same path as the window button
    else if (command == trayToggleAutoRestart) ToggleAutoRestart();
    else if (command == trayExit) DestroyWindow(windowHandle);
}

// The tip is rebuilt from the current status name, so a toggle that changes the name (auto restart)
// can refresh it without inventing a new detail line.
static bool PaintLayeredChip();   // defined with the painting code below
static void SetLayered(bool layered);

// Anything that changes how the panel looks goes through here. A layered window's WM_PAINT output is never
// composited — its content is the bitmap the system was handed — so invalidating it would leave the stale
// picture on screen: folded, the lamp kept the colour it had while the service came and went.
static void RepaintWindow() {
    if (!windowHandle) return;
    if (mini) PaintLayeredChip();
    else { InvalidateRect(windowHandle, nullptr, FALSE); UpdateWindow(windowHandle); }
}

static void RefreshStatusTip() {
    std::wstring name = StatusName();
    statusTip = statusDetail.empty() ? name : name + L" · " + statusDetail;
    if (tooltip) { tipInfo.lpszText = statusTip.data(); SendMessageW(tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tipInfo); }
    UpdateTrayTip();
}

static void SetStatus(State state, const std::wstring& detail) {
    status = state;
    statusDetail = detail;
    RefreshStatusTip();
    RepaintWindow();
}

// Backoff for the automatic restarts: 3s, 6s, 12s, ... capped at 30s.
static DWORD AutoRestartDelayMs(int streak) {
    DWORD delay = autoRestartFirstDelayMs;
    for (int i = 0; i < streak && delay < autoRestartMaxDelayMs; ++i) delay *= 2;
    return delay > autoRestartMaxDelayMs ? autoRestartMaxDelayMs : delay;
}

// A start that stayed up for a minute counts as a success, so one bad moment does not leave the
// launcher permanently backed off (and does not use up the give-up budget).
static bool AutoRestartSettled(ULONGLONG readyAt, ULONGLONG now) {
    return readyAt != 0 && now - readyAt >= autoRestartSettledMs;
}

// One place decides whether to try again, so both failure paths (the service exited on its own, and
// the health probe found it silent) go through the same counter, backoff and give-up rule.
static void ScheduleAutoRestart(const std::wstring& why) {
    ULONGLONG now = GetTickCount64();
    // The stamp is spent by the check: it describes the start that just died, and leaving it in place
    // would let the next attempt count as settled too — the streak would drop back to zero on every
    // failure, so the give-up rule would never fire and the delay would never grow past the first
    // step. Only a start that has been ready for a whole minute resets the streak, once.
    if (AutoRestartSettled(autoRestartReadyAt, now)) { autoRestartStreak = 0; autoRestartReadyAt = 0; }
    if (autoRestartStreak >= maxAutoRestarts) {
        autoRestart = false;
        AppendLog(L"自动重启已连续失败 " + std::to_wstring(autoRestartStreak) + L" 次，已自动关闭该功能；"
            L"请查看日志后手动启动。");
        SaveSettings();          // the file says the feature is off now, not only the running copy
        RefreshStatusTip();
        RepaintWindow();
        return;
    }
    DWORD delay = AutoRestartDelayMs(autoRestartStreak);
    ++autoRestartStreak;
    AppendLog(why + L"自动重启（第 " + std::to_wstring(autoRestartStreak) + L"/" +
        std::to_wstring(maxAutoRestarts) + L" 次）将在 " + std::to_wstring(delay / 1000) +
        L" 秒后重新启动服务。");
    SetTimer(windowHandle, autoRestartTimerId, delay, nullptr);
}

static void ToggleAutoRestart() {
    autoRestart = !autoRestart;
    if (autoRestart) {
        autoRestartStreak = 0;
        autoRestartReadyAt = (serverRunning && serverReady) ? GetTickCount64() : 0;
        AppendLog(L"已开启自动重启：服务无响应或意外退出后会自动重新启动。");
    } else {
        KillTimer(windowHandle, autoRestartTimerId);
        autoRestartReadyAt = 0;
        AppendLog(L"已关闭自动重启。");
    }
    SaveSettings();
    RefreshStatusTip();
    RepaintWindow();
}

static int Scaled(int n) { return (int)(n * scaleFactor + .5f); }
// DWM's frame and a window region do not combine: once a corner preference is set, DWM rounds and frames
// the window's whole rectangle and the region is ignored — folding then left the entire window visible
// with only the chip painted. So the folded chip asks DWM to draw nothing and draws its own frame, and the
// unfolded window hands its frame back to DWM.
static void ApplySystemFrame() {
    if (!systemCorners) return;                   // the system never rounded anything here
    DWM_WINDOW_CORNER_PREFERENCE corners = mini ? DWMWCP_DONOTROUND : DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(windowHandle, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    COLORREF border = mini ? (COLORREF)DWMWA_COLOR_NONE : windowBorderColor;
    DwmSetWindowAttribute(windowHandle, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

// The folded chip is drawn into a 32-bit ARGB bitmap and handed to the system in one call, instead of
// being cut out of the window by a region and framed by hand. A region's edge has no antialiasing at all:
// the silhouette was a staircase, the frame drawn inside it never quite reached that edge, and the reader
// saw both. Here the silhouette, the border, the lamp and the whale are painted together, so the corners
// are as smooth as the ones DWM draws for the unfolded window. The window rectangle and the region are
// left alone, so folding still moves nothing and shows no stale frame.
static void Layout() {
    if (!windowHandle) return;
    // The width never changes: the folded chip is a region of the same window rectangle, anchored to
    // its right edge. Only the height follows the log panel, which keeps the top edge.
    int width = Scaled(W);
    int height = Scaled(mini ? H_COLLAPSED : (expanded ? H_EXPANDED : H_COLLAPSED));
    RECT current{};
    GetWindowRect(windowHandle, &current);
    SetWindowPos(windowHandle, nullptr, current.left, current.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    ApplySystemFrame();
    {
        // The unfolded window keeps its rectangle and lets DWM round the corners and draw the border, which
        // is antialiased and smooth. The folded chip is a layered window instead, whose alpha channel is its
        // shape; on systems without DWM corners the unfolded window rounds itself with a region and paints
        // its own frame.
        if (mini) {
            // Switching a window to layered mode is not atomic: the compositor keeps showing the old surface
            // for a frame. So the window is clipped to the chip first — a region change is what is atomic,
            // and the chip's box in the unfolded window already holds the lamp, the whale and white, which
            // is what the chip looks like — and only then is the antialiased version handed over. Either the
            // whole switch lands inside one frame, or the frame in between shows the same picture.
            // CreateRoundRectRgn returns null when GDI is out of handles, and SetWindowRgn takes that
            // null as "no region at all": the whole 338px window would be left unclipped instead of
            // failing loudly. Only hand over a region that was really made.
            if (HRGN chip = CreateRoundRectRgn(Scaled(miniOffsetX), 0, width + 1,
                    Scaled(H_COLLAPSED) + 1, Scaled(windowCornerEllipsePx), Scaled(windowCornerEllipsePx)))
                SetWindowRgn(windowHandle, chip, FALSE);
            InvalidateRect(windowHandle, nullptr, FALSE);
            UpdateWindow(windowHandle);
            SetLayered(true);
            // The region is what the window is clipped to until the alpha surface has really landed.
            // Once it has, the region goes: the alpha channel is the shape now, and the region's own
            // corner (a wider radius than the chip's) would clip the antialiased corners away again.
            if (PaintLayeredChip()) SetWindowRgn(windowHandle, nullptr, FALSE);
        } else {
            // The other way round: dropping layered mode leaves the surface that is already there — the chip —
            // on screen for a frame, which is what the window looks like anyway, and the normal paint below
            // then fills in the unfolded title bar.
            SetLayered(false);
            if (systemCorners) {
                SetWindowRgn(windowHandle, nullptr, FALSE);
            } else if (HRGN whole = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                    Scaled(windowCornerEllipsePx), Scaled(windowCornerEllipsePx))) {
                SetWindowRgn(windowHandle, whole, FALSE);
            }
        }
    }
    if (logEdit) {
        SetWindowPos(logEdit, nullptr, Scaled(logSide), Scaled(logTop), Scaled(logEditWidth), Scaled(logEditHeight), SWP_NOZORDER | SWP_NOACTIVATE);
        bool show = expanded && !mini;
        ShowWindow(logEdit, show ? SW_SHOW : SW_HIDE);
        if (logVerticalBar) {
            PositionLogBars();
            ShowWindow(logVerticalBar, show ? SW_SHOW : SW_HIDE);
            ShowWindow(logHorizontalBar, show ? SW_SHOW : SW_HIDE);
        }
    }
    // The lamp moves when the chip folds, so the tooltip's hot spot has to move with it.
    if (tooltip) {
        UiRect lamp = LampRect();
        tipInfo.rect = {Scaled(lamp.x), Scaled(buttonTop), Scaled(lamp.x + indicatorWidth), Scaled(buttonTop + buttonHeight)};
        SendMessageW(tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&tipInfo);
    }
    // Paint it now rather than letting the message loop get to it. The window's geometry does not
    // change any more when it folds, so what the compositor has is always the right shape; this keeps
    // its content in step with it within the same call. A layered chip has already been handed over whole,
    // and a layered window's WM_PAINT output goes nowhere anyway.
    if (!mini) {
        InvalidateRect(windowHandle, nullptr, FALSE);
        UpdateWindow(windowHandle);
    }
}

// Folded, the chip has no log panel: a request to show it has to be dropped, not remembered. Left to
// set the flag, a start failure or a service exit behind the chip's back would unfold the whole panel
// the next time the whale was double-clicked, with no click having asked for it.
static void ExpandLog(bool value) { if (mini || expanded == value) return; expanded = value; Layout(); }
static void ToggleMini() {
    mini = !mini;
    // One Layout, not two: setting expanded directly skips the extra full repaint that going through
    // ExpandLog would add, and both changes land in the same move.
    if (mini) expanded = false;
    Layout();
    AppendLog(mini ? L"已折叠为图标模式，双击鲸鱼图标可展开。"
                   : L"已展开全部按钮。");
}
static Button Hit(int x, int y) {
    if (mini) return Button::None;   // folded: the whole chip drags, the icon toggles
    if (logRect.contains(x,y)) return Button::Log;
    if (LampRect().contains(x,y)) return Button::Status;
    if (startRect.contains(x,y)) return Button::Start;
    if (updateRect.contains(x,y)) return Button::Update;
    if (topRect.contains(x,y)) return Button::Topmost;
    if (minRect.contains(x,y)) return Button::Minimize;
    if (closeRect.contains(x,y)) return Button::Close;
    return Button::None;
}

// Only the button whose hover state changed has to be redrawn. Repainting the whole window for that
// was wasted work, and it is what made the mouse crossing the buttons show up over the log at all —
// the reader reported that flicker and it stopped once this became a per-button repaint.
static void InvalidateButton(Button button) {
    UiRect ui{};
    switch (button) {
    case Button::Log: ui = logRect; break;
    case Button::Status: ui = LampRect(); break;
    case Button::Start: ui = startRect; break;
    case Button::Update: ui = updateRect; break;
    case Button::Topmost: ui = topRect; break;
    case Button::Minimize: ui = minRect; break;
    case Button::Close: ui = closeRect; break;
    default: return;
    }
    // One pixel of padding for the border, which is drawn just outside the button itself.
    RECT rect{Scaled(ui.x) - 1, Scaled(ui.y) - 1, Scaled(ui.x + ui.w) + 1, Scaled(ui.y + ui.h) + 1};
    InvalidateRect(windowHandle, &rect, FALSE);
}

static Color ColorForStatus() {
    switch (status) {
    // A running service with auto restart armed is purple: the reader asked for a colour that says
    // "this one picks itself back up". Green stays for a plain run.
    case State::Running: return autoRestart ? Color(lampPurpleArgb) : Color(34,197,94);
    case State::Stopped: return Color(234,179,8);
    case State::Update: return Color(lampPurpleArgb);
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

// Buttons carry their state in the background instead of in a longer label: 置顶 is tinted
// while it is on, 更新 turns violet when a new version is ready and amber when the last
// version can be restored.
enum class Tone { Plain, Accent, Update, Rollback };

static void ToneColors(Tone tone, Color& background, Color& border, Color& text, bool hovered) {
    switch (tone) {
    case Tone::Accent:
        background = hovered ? Color(191,219,254) : Color(219,234,254);
        border = Color(147,197,253); text = Color(30,64,175); break;
    case Tone::Update:
        background = hovered ? Color(221,214,254) : Color(237,233,254);
        border = Color(196,181,253); text = Color(109,40,217); break;
    case Tone::Rollback:
        background = hovered ? Color(254,215,170) : Color(255,237,213);
        border = Color(253,186,116); text = Color(194,65,12); break;
    default:
        background = hovered ? Color(239,246,255) : Color(255,255,255);
        border = Color(203,213,225); text = Color(35,50,76); break;
    }
}

static Tone UpdateTone() {
    if (!rollbackVersion.empty()) return Tone::Rollback;
    return updateAvailable ? Tone::Update : Tone::Plain;
}

static void DrawButton(Graphics& g, const UiRect& r, const std::wstring& label, Button button,
    bool primary = false, bool disabled = false, Tone tone = Tone::Plain) {
    Color bg = primary ? Color(37,99,235) : Color(255,255,255);
    Color border = primary ? Color(37,99,235) : Color(203,213,225);
    Color fg = primary ? Color(255,255,255) : Color(35,50,76);
    if (!primary) ToneColors(tone, bg, border, fg, hoverButton == button);
    if (disabled) { bg = Color(248,250,252); fg = Color(160,170,185); border = Color(221,228,238); }
    else if (primary && hoverButton == button) { bg = Color(29,78,216); }
    GraphicsPath path; Rounded(path,r.x+.5f, r.y+.5f, r.w-1.f, r.h-1.f, cornerRadius);
    SolidBrush fill(bg); Pen edge(border, 1); g.FillPath(&fill, &path); g.DrawPath(&edge, &path);
}

static void DrawCaption(HDC dc, const UiRect& r, const std::wstring& text, COLORREF color) {
    // DT_VCENTER centres the font's line box, which reserves descender space that CJK-only
    // text never uses, so the ink sits one pixel above the middle of the button — visible
    // next to the status lamp, which is centred geometrically. Nudge the box down by one.
    RECT bounds{Scaled(r.x), Scaled(r.y) + Scaled(1), Scaled(r.x + r.w), Scaled(r.y + r.h) + Scaled(1)};
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
    // The whole surface is cleared, not just the part the region shows: the window keeps its width
    // when it folds, and the part a region hides must not hold pixels waiting to appear when it grows
    // back. What the reader sees of it is the chip box.
    int chipLeft = mini ? miniOffsetX : 0;
    int chipWidth = mini ? W_MINI : W;
    int h = mini ? H_COLLAPSED : (expanded ? H_EXPANDED : H_COLLAPSED);
    SolidBrush white(Color::White); g.FillRectangle(&white, 0, 0, W, H_EXPANDED);
    // The frame is drawn only when the window is not being rounded and framed by DWM: the folded chip
    // (which is a region, so DWM's border would be around the whole window rectangle) and any system
    // without DWM corners. Its sides are drawn with GDI rather than GDI+: a 1px GDI+ stroke straddles its
    // path and leaves half a pixel in one column and half in the next, so the sides came out blurred and
    // uneven. GDI fills land exactly on the pixels they are given. The corners stay GDI+, because at this
    // radius a GDI arc is a visible staircase and GDI+ antialiases the curve.
    g.Flush();
    if (mini || !systemCorners) {
        const int r = Scaled((int)cornerRadius);
        const int x0 = Scaled(chipLeft), y0 = 0, w = Scaled(chipWidth), hgt = Scaled(h);
        HBRUSH brush = CreateSolidBrush(windowBorderColor);
        // The four sides sit on the outermost pixels, stopping where the corner arcs take over.
        RECT top{x0 + r, y0, x0 + w - r, y0 + 1};
        RECT bottom{x0 + r, y0 + hgt - 1, x0 + w - r, y0 + hgt};
        RECT left{x0, y0 + r, x0 + 1, y0 + hgt - r};
        RECT right{x0 + w - 1, y0 + r, x0 + w, y0 + hgt - r};
        FillRect(memory, &top, brush);
        FillRect(memory, &bottom, brush);
        FillRect(memory, &left, brush);
        FillRect(memory, &right, brush);
        DeleteObject(brush);
        // The corners are filled quarter-rings, not stroked arcs. A stroked curve straddles the pixel grid
        // and covers each pixel by about three quarters, so the corners came out visibly lighter than the
        // straight sides even at the same colour and a wider pen did not help. A filled ring is solid in
        // the middle and antialiased only along its two edges: smooth curve, same weight as the sides.
        g.Flush();
        SolidBrush cornerBrush{Color(windowBorderArgb)};
        float fx = (float)chipLeft, fy = 0.f, fw = (float)chipWidth, fh = (float)h;
        const float outer = (float)cornerRadius - 0.1f;   // stays just inside the window region
        // A little wider than the sides on purpose: an antialiased curve running diagonally covers a pixel
        // by about three quarters at 1px, so the corners came out lighter than the straight sides even in
        // the same colour. 1.4px brings the core of the curve up to full colour, and the eye reads a
        // diagonal line as thinner than a straight one of the same width anyway.
        const float inner = outer - 1.4f;
        const float centre = (float)cornerRadius;
        // chipWidth and h are exclusive sizes: the last pixel column and row are one less, and the corner
        // circles have to be measured from those. Using the exclusive edges put the right and bottom rings a
        // pixel out, so their outer half fell outside the window and was clipped — the bottom right corner
        // lost nearly all of its arc and the other two lost their ends.
        const float chipRight = fx + fw - 1.f, chipBottom = fy + fh - 1.f;
        auto ring = [&](float cx, float cy, float from) {
            GraphicsPath path;
            path.AddArc(RectF(cx - outer, cy - outer, 2 * outer, 2 * outer), from, 90.f);
            path.AddArc(RectF(cx - inner, cy - inner, 2 * inner, 2 * inner), from + 90.f, -90.f);
            path.CloseFigure();
            g.FillPath(&cornerBrush, &path);
        };
        ring(fx + centre, fy + centre, 180.f);
        ring(chipRight - centre, fy + centre, 270.f);
        ring(chipRight - centre, chipBottom - centre, 0.f);
        ring(fx + centre, chipBottom - centre, 90.f);
        g.Flush();
    }
    DrawButton(g,LampRect(),L"",Button::Status);
    SolidBrush dot(ColorForStatus());
    UiRect lamp = LampRect();
    g.FillEllipse(&dot, lamp.x + (indicatorWidth - lampDotDiameter) / 2,
        buttonTop + (buttonHeight - lampDotDiameter) / 2, lampDotDiameter, lampDotDiameter);
    if (!mini) {
    DrawButton(g,startRect,serverRunning ? L"停止" : L"启动",Button::Start,true,busy);
    DrawButton(g,logRect,L"日志",Button::Log);
    DrawButton(g,updateRect,updateLabel,Button::Update,false,busy || serverRunning, UpdateTone());
    DrawButton(g,topRect,L"置顶",Button::Topmost,false,false,topmost ? Tone::Accent : Tone::Plain);
    GraphicsPath titlePath;
    Rounded(titlePath,titleClusterX + .5f,10.5f,(float)(2 * titleButtonWidth - 1),27,cornerRadius);
    SolidBrush pale(Color(248,250,252));
    Pen light(Color(203,213,225),1); g.FillPath(&pale,&titlePath);
    if (hoverButton == Button::Minimize || hoverButton == Button::Close) {
        GraphicsState saved = g.Save(); g.SetClip(&titlePath);
        SolidBrush hl(Color(236,242,249));
        g.FillRectangle(&hl,hoverButton == Button::Minimize ? minRect.x + 1 : closeRect.x + 1,11,24,26);
        g.Restore(saved);
    }
    g.DrawPath(&light,&titlePath);
    Pen divider(Color(226,232,240),1); g.DrawLine(&divider,titleDividerX,16,titleDividerX,32);
    Pen dash(Color(71,85,105),1.8f); g.DrawLine(&dash,minRect.x + 8,25,minRect.x + 17,25);
    Pen cross(Color(185,28,28),1.8f);
    g.DrawLine(&cross,closeRect.x + 8,19,closeRect.x + 16,29);
    g.DrawLine(&cross,closeRect.x + 16,19,closeRect.x + 8,29);
    }   // end of the buttons-only block
    if (expanded && !mini) {
        GraphicsPath logBox; Rounded(logBox,logBoxSide,logBoxTop,(float)(W - 2 * logBoxSide),logBoxHeight,cornerRadius); Pen logBorder(Color(221,228,238),1);
        g.DrawPath(&logBorder,&logBox);
    }
    g.Flush();
    if (appIcon) {
        UiRect icon = IconDrawRect();
        DrawIconEx(memory, Scaled(icon.x), Scaled(icon.y), appIcon, Scaled(icon.w), Scaled(icon.h), 0, nullptr, DI_NORMAL);
    }
    if (!mini) {
    HFONT font = CreateFontW(-Scaled(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    HGDIOBJ previousFont = SelectObject(memory,font);
    DrawCaption(memory,logRect,L"日志",RGB(35,50,76));
    DrawCaption(memory,startRect,serverRunning?L"停止":L"启动",busy?RGB(160,170,185):RGB(255,255,255));
    Color updateBg, updateBorder, updateText;
    ToneColors(UpdateTone(), updateBg, updateBorder, updateText, false);
    DrawCaption(memory,updateRect,updateLabel,busy||serverRunning?RGB(160,170,185):RGB(updateText.GetR(),updateText.GetG(),updateText.GetB()));
    Color topBg, topBorder, topText;
    ToneColors(topmost ? Tone::Accent : Tone::Plain, topBg, topBorder, topText, false);
    DrawCaption(memory,topRect,L"置顶",RGB(topText.GetR(),topText.GetG(),topText.GetB()));
    SelectObject(memory,previousFont); DeleteObject(font);
    }
    BitBlt(hdc,0,0,client.right,client.bottom,memory,0,0,SRCCOPY);
    SelectObject(memory,old); DeleteObject(bitmap); DeleteDC(memory); EndPaint(windowHandle,&ps);
}

static bool PaintLayeredChip() {
    if (!windowHandle) return false;
    const int width = Scaled(W), height = Scaled(H_COLLAPSED);
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = width;
    header.bV5Height = -height;                 // top-down, like the rest of the painting here
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, (BITMAPINFO*)&header, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib || !bits) { if (dib) DeleteObject(dib); return false; }
    memset(bits, 0, (size_t)width * height * 4);          // nothing of the window is opaque to begin with
    {
        // GDI+ writes proper alpha into those bits when the bitmap wraps them, which a compatible DC would
        // not: drawing through a DC leaves the alpha channel at zero and the chip would be invisible.
        Gdiplus::Bitmap canvas(width, height, width * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        // Same transform the unfolded paint uses: the chip's box, the lamp and the whale are all laid
        // out in the launcher's own units. Without it the white body was put down in device pixels
        // while the lamp and the whale went down unscaled, so on a scaled display the chip was drawn
        // 1.5x too far right and the two glyphs floated beside it instead of on it.
        g.ScaleTransform(scaleFactor, scaleFactor);
        const float left = (float)miniOffsetX, top = 0.f;
        const float chipW = (float)W_MINI, chipH = (float)H_COLLAPSED;
        // The white inside is filled first, then the border is stroked on top with a 1px pen. Measured, GDI+
        // renders a stroked path half a pixel to the right of its coordinates here, so the path is put on
        // whole pixels and the stroke lands exactly on one column (or row) of the grid — filling two rounded
        // rectangles and taking the difference instead left the straight sides at half coverage over two
        // columns. The corners stay antialiased because the path's arcs are.
        GraphicsPath border;
        Rounded(border, left, top, chipW - 1.f, chipH - 1.f, cornerRadius - 0.5f);
        SolidBrush white(Color::White);
        g.FillPath(&white, &border);
        Pen edge(Color(windowBorderArgb), 1.0f);
        g.DrawPath(&edge, &border);
        DrawButton(g, LampRect(), L"", Button::Status);
        SolidBrush dot{ColorForStatus()};
        UiRect lamp = LampRect();
        g.FillEllipse(&dot, lamp.x + (indicatorWidth - lampDotDiameter) / 2,
            buttonTop + (buttonHeight - lampDotDiameter) / 2, lampDotDiameter, lampDotDiameter);
        if (appIcon) {
            UiRect icon = IconDrawRect();
            if (Gdiplus::Bitmap* glyph = Gdiplus::Bitmap::FromHICON(appIcon)) {
                g.DrawImage(glyph, RectF((float)icon.x, (float)icon.y, (float)icon.w, (float)icon.h),
                    0.f, 0.f, (float)glyph->GetWidth(), (float)glyph->GetHeight(), UnitPixel);
                delete glyph;
            }
        }
    }
    bool applied = false;
    HDC memory = CreateCompatibleDC(nullptr);
    if (memory) {
        HGDIOBJ previous = SelectObject(memory, dib);
        SIZE size{width, height};
        POINT source{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        applied = UpdateLayeredWindow(windowHandle, nullptr, nullptr, &size, memory, &source, 0, &blend, ULW_ALPHA) != FALSE;
        SelectObject(memory, previous);
        DeleteDC(memory);
    }
    DeleteObject(dib);
    if (!applied) {
        // The surface could not be handed over (GDI or the compositor refused it). A layered window with
        // nothing in it is an invisible launcher, so fall back to the shape the fold path had already
        // cut: the window keeps a region on the chip's box and paints through it the ordinary way. The
        // antialiased corners are lost, the chip is not.
        SetLayered(false);
        if (HRGN chip = CreateRoundRectRgn(Scaled(miniOffsetX), 0, Scaled(W) + 1, Scaled(H_COLLAPSED) + 1,
                Scaled(windowCornerEllipsePx), Scaled(windowCornerEllipsePx)))
            SetWindowRgn(windowHandle, chip, FALSE);
        InvalidateRect(windowHandle, nullptr, FALSE);
        UpdateWindow(windowHandle);
    }
    return applied;
}

// The folded chip is a layered window: its shape comes from the bitmap's alpha channel, so it must not also
// carry a window region, whose aliased edge would cut the antialiased corners off again.
static void SetLayered(bool layered) {
    if (!windowHandle) return;
    LONG_PTR style = GetWindowLongPtrW(windowHandle, GWL_EXSTYLE);
    bool has = (style & WS_EX_LAYERED) != 0;
    if (has == layered) return;
    SetWindowLongPtrW(windowHandle, GWL_EXSTYLE, layered ? (style | WS_EX_LAYERED) : (style & ~WS_EX_LAYERED));
    SetWindowPos(windowHandle, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void BeginWork(Work work) {
    if (busy || serverRunning) return;
    busy = true; RepaintWindow();
    // The fallback version is copied here, on the UI thread. It is a std::wstring the panel reassigns
    // from the exit handler and from every finished job, so a worker that read the global could be
    // reading the old buffer of a string that was just reallocated underneath it.
    std::wstring rollbackTarget = work == Work::Rollback ? rollbackVersion : std::wstring();
    std::thread([work, rollbackTarget]{ Worker(work, rollbackTarget); }).detach();
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
        } else {
            // A manual start takes over from a scheduled retry: leaving the timer armed would let it
            // fire after this attempt has already failed, starting a second one that the streak and the
            // backoff know nothing about.
            KillTimer(windowHandle, autoRestartTimerId);
            serverReady = false; autoRestartStreak = 0; autoRestartReadyAt = 0; BeginWork(Work::Start);
        }
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
        SaveSettings();
        RepaintWindow(); break;
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
        if (IconRect().contains(x, y)) return HTCAPTION;
        if (mini) return LampRect().contains(x, y) ? HTCLIENT : HTCAPTION;
        if (y < 50 && Hit(x, y) == Button::None) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCLBUTTONDBLCLK: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &point);
        int x = (int)(point.x / scaleFactor), y = (int)(point.y / scaleFactor);
        if (IconRect().contains(x, y)) ToggleMini();
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
        // DWM rounds the window's corners and draws its border, in the launcher's own border colour. That
        // is what the unfolded window shows: its corners are antialiased, where a window region's are a
        // hard staircase, and a hand-drawn frame inside a region never lines up with the region's edge.
        DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUNDSMALL;
        systemCorners = SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners)));
        if (systemCorners) {
            COLORREF borderColor = windowBorderColor;
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
        { UiRect lamp = LampRect();
          // Only as wide as the lamp itself: it now sits beside the whale, and a wider hot spot would
          // steal the whale's own hover.
          tipInfo.rect={Scaled(lamp.x),Scaled(buttonTop),Scaled(lamp.x + indicatorWidth),Scaled(buttonTop + buttonHeight)}; }
        tipInfo.lpszText=statusTip.data(); SendMessageW(tooltip,TTM_ADDTOOLW,0,(LPARAM)&tipInfo);
        Layout();
        bool attached = AttachRunningServer() || AdoptRunningService();
        SetStatus(attached?State::Running:(Installed()?State::Stopped:State::Missing),
            attached?L"检测到正在运行的 DeepSeek Harness Web 服务。":
            (Installed()?L"点击“启动”运行 DeepSeek Harness。":L"点击“启动”自动安装 DeepSeek Harness。"));
        AppendLog(L"启动器已就绪。首次启动会按需下载工具并安装 DeepSeek Harness，可能需要几分钟。");
        if (autoRestart) AppendLog(L"自动重启已开启（上次退出前是开启的）：服务无响应或意外退出后会自动重新启动。");
        if (attached) {
            AppendLog(L"检测到已运行的 DeepSeek Harness，可以点击“停止”结束服务。");
            OpenBrowserIfNoPage();
        } else {
            // No service to take over: open straight into a running harness.
            AppendLog(L"未检测到运行中的服务，自动启动 DeepSeek Harness。");
            BeginWork(Work::Start);
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
        Button hit=Hit(x,y);
        if(hit!=hoverButton){
            Button previous=hoverButton; hoverButton=hit;
            InvalidateButton(previous); InvalidateButton(hoverButton);
        }
        // The pointer may be leaving the log box for the window's own area; the bars follow that too.
        RefreshLogBars();
        TRACKMOUSEEVENT tme{sizeof(tme),TME_LEAVE,hwnd,0}; TrackMouseEvent(&tme); return 0;
    }
    case WM_MOUSELEAVE: {
        Button previous=hoverButton; hoverButton=Button::None; InvalidateButton(previous);
        RefreshLogBars();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x=(int)(GET_X_LPARAM(lp)/scaleFactor), y=(int)(GET_Y_LPARAM(lp)/scaleFactor);
        // The press only remembers which button it is on; the action follows the release, so dragging off
        // a button before letting go cancels it the way it does everywhere else. Moving the window is not
        // affected: that is HTCAPTION, which the system takes over straight from WM_NCHITTEST.
        pressedButton = Hit(x,y);
        if (pressedButton != Button::None) SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP: {
        Button was = pressedButton;
        pressedButton = Button::None;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (was == Button::None) return 0;
        int x=(int)(GET_X_LPARAM(lp)/scaleFactor), y=(int)(GET_Y_LPARAM(lp)/scaleFactor);
        // Same button under the pointer at the end: the press meant it. Anywhere else: it did not.
        if (Hit(x,y) == was) OnClick(was);
        return 0;
    }
    case WM_CAPTURECHANGED:
        pressedButton = Button::None;   // the gesture was taken over by something else: it is over
        return 0;
    case WM_LBUTTONDBLCLK: {
        int x=(int)(GET_X_LPARAM(lp)/scaleFactor), y=(int)(GET_Y_LPARAM(lp)/scaleFactor);
        if(IconRect().contains(x,y)) ToggleMini();
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
        } else if(!r->info.empty()) {
            AppendLog(r->info);
            SetStatus(Installed()?State::Stopped:State::Missing,r->info);
        } else if(r->work==Work::Start) {
            updateAvailable=false; updateLabel=L"更新"; rollbackVersion.clear();
            SetStatus(State::Stopped,L"正在等待 Web 服务就绪…");
        } else if(r->work==Work::InstallUpdate) {
            updateAvailable=false; updateLabel=L"更新"; rollbackVersion.clear();
            AppendLog(L"更新完成，版本 "+r->version+L"。");
            SetStatus(State::Stopped,L"已安装版本 "+r->version+L"。");
        } else if(r->work==Work::Rollback) {
            updateAvailable=false; updateLabel=L"更新"; rollbackVersion.clear();
            AppendLog(L"已回退到版本 "+r->version+L"。");
            SetStatus(State::Stopped,L"已回退到版本 "+r->version+L"。");
        } else if(!r->installed) {
            AppendLog(L"最新版本 "+r->version+L"；本机尚未安装，点击“启动”即可安装。");
            SetStatus(State::Missing,L"npm 最新版本 "+r->version+L"。启动时会自动安装。");
        } else if(r->update) {
            updateAvailable=true; updateLabel=L"更新"; rollbackVersion.clear();
            AppendLog(L"发现新版本："+Version()+L" → "+r->version+L"。“更新”按钮已变为紫色，点击即可安装。");
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
        autoRestartReadyAt=GetTickCount64();
        KillTimer(hwnd,autoRestartTimerId);
        SetStatus(State::Running,L"检测到已在运行的 Web 服务（PID "+std::to_wstring((DWORD)wp)+L"），已接管；点击“停止”可结束它。");
        InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_SERVER_READY:
        serverReady=true;
        autoRestartReadyAt=GetTickCount64();
        KillTimer(hwnd,autoRestartTimerId);   // this start made it, so a pending retry is off
        if(!serverExternal) SaveKnownGoodVersion(Version());
        rollbackVersion.clear(); updateLabel=L"更新";
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
        if (autoRestart) {
            // The process is up but the web server inside it is not: end it and let the exit handler
            // schedule the restart, so both failure paths share one counter and one backoff. This is
            // not the user asking for a stop, so the exit that follows counts as unexpected.
            ServerIdentity id;
            { std::lock_guard lock(processMutex); id = serverIdentity; }
            AppendLog(L"自动重启已介入：正在结束无响应的服务进程（PID "+std::to_wstring(id.pid)+L"）。");
            if (!TerminateServer(id)) AppendLog(L"自动重启：无法结束该进程，请手动处理。");
        }
        return 0;
    case WM_SERVER_HEALTHY:
        if (!serverRunning || status != State::Unresponsive) return 0;
        SetStatus(State::Running, L"Web 服务已恢复响应。");
        AppendLog(L"Web 服务已恢复响应，状态灯已变回绿色。");
        return 0;
    case WM_SERVER_EXIT: {
        if (!serverRunning && !busy) return 0;
        bool failed=!stopping&&!serverReady;
        bool unexpected=!stopping;          // nobody asked for this exit, and auto restart wants to know
        serverRunning=false; busy=false; serverReady=false; serverExternal=false;
        std::wstring known=KnownGoodVersion();
        if(failed&&!known.empty()&&known!=Version()) {
            rollbackVersion=known; updateAvailable=false; updateLabel=L"回退";
        }
        State stoppedState=updateAvailable?State::Update:(Installed()?State::Stopped:State::Missing);
        SetStatus(stoppedState, failed
            ? (rollbackVersion.empty()?L"服务未能启动，请查看日志。":L"服务未能启动；可回退到 "+rollbackVersion+L"。")
            : L"Web 服务已停止。");
        AppendLog(failed?L"启动失败，服务退出代码 "+std::to_wstring((DWORD)wp)+L"。":
            (stopping?L"Web 服务已停止。":L"Web 服务已退出，代码 "+std::to_wstring((DWORD)wp)+L"。"));
        if(failed&&!rollbackVersion.empty())
            AppendLog(L"上次可用版本是 "+rollbackVersion+L"，“更新”按钮已变为橙色，点击即可回退。");
        if(failed) ExpandLog(true);
        stopping=false; InvalidateRect(hwnd,nullptr,FALSE);
        if(unexpected&&autoRestart&&!closing) ScheduleAutoRestart(failed?L"服务未能启动，":L"服务意外退出，");
        return 0;
    }
    case WM_TIMER:
        if (wp == autoRestartTimerId) {
            KillTimer(hwnd,autoRestartTimerId);
            // A job already in flight owns the start: announcing a restart that BeginWork would then
            // refuse to make would leave the log describing something that never happened.
            if (autoRestart&&!closing&&!serverRunning&&!busy) {
                AppendLog(L"自动重启：正在重新启动 Web 服务…");
                serverReady=false; BeginWork(Work::Start);
            }
            return 0;
        }
        break;
    case WM_INITMENUPOPUP: {
        // Popup menus are a "#32768" window owned by the shell, so their frame is square by
        // default. Round it and give it the launcher's border colour so both menus match the
        // window they belong to. Harmless where DWM corner support is missing.
        if (HWND popup = FindWindowW(L"#32768", nullptr)) {
            DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUNDSMALL;
            DwmSetWindowAttribute(popup, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
            COLORREF border = windowBorderColor;
            DwmSetWindowAttribute(popup, DWMWA_BORDER_COLOR, &border, sizeof(border));
        }
        return 0;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* measure = (MEASUREITEMSTRUCT*)lp;
        if (measure && measure->CtlType == ODT_MENU) { MeasureLogMenuItem(measure); return TRUE; }
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* draw = (DRAWITEMSTRUCT*)lp;
        if (draw && draw->CtlType == ODT_MENU) { DrawLogMenuItem(draw); return TRUE; }
        break;
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
    LoadSettings();   // before the window exists, so a remembered "not always on top" is honoured
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
    // WS_CLIPCHILDREN matters: the buttons repaint the window whenever the pointer moves over
    // them, and without this the window's paint covers the log box and its bars too, which are
    // then repainted a moment later — that is the flicker the reader sees.
    HWND hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|(topmost?WS_EX_TOPMOST:0),wc.lpszClassName,L"DeepSeek Harness 启动器",
        mainWindowStyle,x,y,width,height,nullptr,nullptr,instance,nullptr);
    if(!hwnd) return 1;
    // The ex style is not enough on its own: a remembered "not on top" has to be pushed once the
    // window exists, or the first show would still raise it above everything.
    SetWindowPos(hwnd,topmost?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
    // Shown by default; the tray icon is still there for "hide to tray" and the menu.
    taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();
    ShowWindow(hwnd,SW_SHOW);
    UpdateWindow(hwnd);
    MSG message;
    while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    GdiplusShutdown(gdiplusToken);
    return (int)message.wParam;
}
