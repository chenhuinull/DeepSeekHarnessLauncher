// Regression checks for the update check's version parsing and the npm selection.
//
// Reported symptom: the launcher reported a new version on every check
// ("found a new version: 0.1.5-rc.2 -> allow-scripts") and the update button stayed violet, so
// clicking it re-ran the same check forever.
//
// Cause: npm writes diagnostics to the same pipe as its answer, and an .npmrc key that
// the invoked npm does not recognise is quoted verbatim in a warning:
//   npm warn Unknown user config "allow-scripts". This will stop working ...
// The old parser took the first quoted token in the stream and read that config key as
// the newest release, so `Version() != latest` was always true. The warning appeared at
// all because the launcher silently used the npm bundled with system Node.js (11.13.0,
// which does not know `allow-scripts`) instead of the newer one on PATH (11.17.0), so
// the npm chosen for a run is now detected, logged, and the newest same-major copy wins.
//
// This translation unit compiles the real launcher and drives its logic directly.
// Build (from a VS developer prompt, or after Launch-VsDevShell):
//   cl /nologo /std:c++20 /utf-8 /O1 /MT /EHsc /DUNICODE /D_UNICODE /Fe:version_parse_test.exe ^
//      version_parse_test.cpp user32.lib gdi32.lib gdiplus.lib dwmapi.lib shell32.lib ole32.lib ^
//      uuid.lib comctl32.lib iphlpapi.lib ws2_32.lib winhttp.lib bcrypt.lib advapi32.lib version.lib
// Run with --discover to print the Node.js/npm copies found on this machine.

#include "launcher.cpp"

#include <cstdio>

static int failures = 0;

static void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

// A minimal `<root>/node_modules/npm` tree, as npm installs itself.
static std::wstring MakeNpm(const fs::path& root, const std::wstring& version) {
    fs::path npm = root / L"node_modules" / L"npm";
    fs::create_directories(npm / L"bin");
    std::ofstream package(npm / L"package.json");
    package << "{\"name\":\"npm\",\"version\":\"" << Narrow(version) << "\"}\n";
    std::ofstream cli(npm / L"bin" / L"npm-cli.js");
    cli << "// fixture\n";
    return (npm / L"bin" / L"npm-cli.js").wstring();
}

static void Discover() {
    std::wprintf(L"npm copies found on this machine:\n");
    for (const auto& cli : NpmCandidates(L""))
        std::wprintf(L"  %-12s %s\n", NpmCliVersion(cli).c_str(), cli.c_str());
    if (auto tools = SystemNode()) {
        std::wprintf(L"\nselected: Node.js %s (%s)\n          npm %s (%s)\n",
            FileVersionString(tools->node).c_str(), tools->node.c_str(),
            tools->npmVersion.c_str(), NpmPackageRoot(tools->npm).c_str());
    } else {
        std::wprintf(L"\nno system Node.js found\n");
    }
}

// Drives the real log control. It carries no scroll bar styles at all: the launcher measures the log
// itself and draws and drives two bars of its own in a narrow zone next to the painted border, so no
// non-client strip is reserved and the text gets that space. The checks below cover that: the control
// has no strip, the text box really is that much bigger, the launcher's own measurements follow the
// content, and auto-follow, keyboard and the wheel still work.
static void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

static int FirstVisibleLine() {
    return logEdit ? (int)SendMessageW(logEdit, EM_GETFIRSTVISIBLELINE, 0, 0) : 0;
}

static void ParkCaretAtEnd() {
    int lastLine = (int)SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0) - 1;
    int lastStart = (int)SendMessageW(logEdit, EM_LINEINDEX, (WPARAM)lastLine, 0);
    if (lastStart >= 0) SendMessageW(logEdit, EM_SETSEL, lastStart, lastStart);
    SendMessageW(logEdit, EM_SCROLLCARET, 0, 0);
}

static void CheckLogBarCovers();

static int RunChecks(int argc, wchar_t** argv);

static void ReportBars(const char* what) {
    RECT client{};
    GetClientRect(logEdit, &client);
    std::printf("      %-20s client %dx%d  lines=%d  firstVisible=%d  maxLineChars=%d  offset=%d\n",
        what, client.right, client.bottom,
        (int)SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0), FirstVisibleLine(),
        logMaxLineWidth, logHorizontalOffset);
}

static void CheckLogBars() {
    // Off-screen but shown: this is the state the log box is in while the panel is expanded.
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, -2000, -2000, 420, 320,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent) { Check(false, "a parent window for the log control could be created"); return; }
    CreateLogEdit(parent);
    if (!logEdit) { Check(false, "the log control could be created"); DestroyWindow(parent); return; }
    SetWindowPos(logEdit, nullptr, Scaled(logSide), Scaled(logTop),
        Scaled(logEditWidth), Scaled(logEditHeight), SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(parent, SW_SHOW);
    ShowWindow(logEdit, SW_SHOW);
    Pump();

    LONG_PTR style = GetWindowLongPtrW(logEdit, GWL_STYLE);
    Check((style & WS_VSCROLL) == 0 && (style & WS_HSCROLL) == 0,
        "the log box carries no scroll bar styles, so it reserves no non-client strip");

    RECT window{}, client{};
    GetWindowRect(logEdit, &window);
    GetClientRect(logEdit, &client);
    Check(client.right == window.right - window.left && client.bottom == window.bottom - window.top,
        "the whole control is client area: nothing is set aside for a scroll bar strip");
    int oldWidth = Scaled(W - 2 * logSide) - 17;    // what the text used to get, with the strip
    int oldHeight = Scaled(logHeight) - 17;
    Check(client.right > oldWidth && client.bottom > oldHeight,
        "the text box is wider and taller than the old strip left it");
    std::printf("      text box %dx%d vs %dx%d before  (+%d wide, +%d tall)\n",
        client.right, client.bottom, oldWidth, oldHeight, client.right - oldWidth, client.bottom - oldHeight);

    SetWindowTextW(logEdit, L"one short line");
    Pump();
    ParkCaretAtEnd();
    Check(FirstVisibleLine() == 0, "a short log starts at the top");

    // Content arrives the way the launcher really appends it, so the line-width tracking and the
    // auto-follow below are the production paths and not a shortcut.
    for (int line = 0; line < 40; ++line) AppendLog(L"line " + std::to_wstring(line));
    Pump();
    // Auto-follow is what keeps the newest lines in view while the service is talking; it has to
    // work without a scroll bar, because the launcher no longer has one to move.
    Check(FirstVisibleLine() > 0, "appending lines keeps the view at the bottom");
    ReportBars("40 short lines");

    std::wstring wide(600, L'x');
    AppendLog(wide);
    Pump();
    Check(logMaxLineWidth >= LogLineWidth(wide), "the widest line is tracked in pixels as it arrives");

    ClearLog();
    Pump();
    for (int line = 0; line < 40; ++line) AppendLog(wide + L" " + std::to_wstring(line));
    Pump();
    Check(logMaxLineWidth >= LogLineWidth(wide) && FirstVisibleLine() > 0,
        "content that overflows both ways is measured in both directions");

    // A sideways offset the reader caused by scrolling is read back from the control itself.
    SendMessageW(logEdit, EM_LINESCROLL, 100000, 0);
    Pump();
    SyncLogHorizontalOffset();
    SyncLogHorizontalOffset();
    Check(logHorizontalOffset > 0 && LogCharWidth() > 0, "the sideways offset is read back from the control");
    std::printf("      after scrolling right: offset=%d px, charWidth=%d\n", logHorizontalOffset, LogCharWidth());
    ReportBars("40 long lines");

    // The wheel is the launcher's job now: a control without scroll bar styles ignores it.
    int beforeWheel = FirstVisibleLine();
    SendMessageW(logEdit, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)WHEEL_DELTA), MAKELPARAM(0, 0));
    Pump();
    Check(FirstVisibleLine() < beforeWheel, "a wheel notch scrolls the log back up");

    ClearLog();
    Pump();
    Check(logMaxLineWidth == 0 && logHorizontalOffset == 0 && logMaxLineIndex == -1,
        "clearing the log forgets the measured line width and offset");
    ReportBars("cleared");

    CheckLogBarCovers();

    DestroyWindow(logEdit);
    logEdit = nullptr;
    logVerticalBar = logHorizontalBar = nullptr;
    DestroyWindow(parent);
}

// Repaints the bars into a memory DC through the same function WM_NCPAINT uses, then reads the
// pixels back: the look is "one line, nothing else", which is what these checks pin down.
struct PaintedBar {
    int runs = 0;                             // contiguous painted bands along the bar
    int first = 0, last = 0;                  // extent of the line along the bar
    int acrossFirst = 0, acrossLast = 0;      // extent of the line across the bar
    int widestRow = 0;                        // most pixels painted in one step along the bar
    bool any() const { return runs > 0; }
    int thickness() const { return acrossLast - acrossFirst + 1; }
    int length() const { return last - first + 1; }
};

static PaintedBar ReadPaintedBar(HDC dc, const RECT& strip, bool vertical) {
    PaintedBar bar;
    int alongFrom = vertical ? strip.top : strip.left;
    int alongTo = vertical ? strip.bottom : strip.right;
    int acrossFrom = vertical ? strip.left : strip.top;
    int acrossTo = vertical ? strip.right : strip.bottom;
    bool inRun = false;
    for (int along = alongFrom; along < alongTo; ++along) {
        int painted = 0, rowFirst = 0, rowLast = 0;
        for (int across = acrossFrom; across < acrossTo; ++across) {
            COLORREF pixel = GetPixel(dc, vertical ? across : along, vertical ? along : across);
            if (pixel == CLR_INVALID || pixel == RGB(255, 255, 255)) continue;
            if (!painted) rowFirst = across;
            rowLast = across;
            ++painted;
        }
        if (painted > bar.widestRow) bar.widestRow = painted;
        if (!painted) { inRun = false; continue; }
        if (!inRun) {
            inRun = true;
            ++bar.runs;
            if (bar.runs == 1) { bar.first = along; bar.acrossFirst = rowFirst; bar.acrossLast = rowLast; }
        }
        if (bar.runs == 1) {
            bar.last = along;
            if (rowFirst < bar.acrossFirst) bar.acrossFirst = rowFirst;
            if (rowLast > bar.acrossLast) bar.acrossLast = rowLast;
        }
    }
    return bar;
}

// The covers are what makes the bare line possible: the system repaints its own bar from inside the
// control at times this file cannot intercept, so the only reliable answer is to put opaque windows
// over those strips. These checks cover the three things that makes work: the covers line up with
// the strips, they really are on top, and they draw one line each and drag the log.
static void CheckLogBarCovers() {
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, -2000, -2000, 320, 320,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent) { Check(false, "a parent window for the log control could be created"); return; }
    CreateLogEdit(parent);
    if (!logEdit) { Check(false, "the log control could be created"); DestroyWindow(parent); return; }
    Check(logVerticalBar && logHorizontalBar, "a bar window exists for each direction");
    Check(logVerticalBar && GetParent(logVerticalBar) == parent && GetParent(logHorizontalBar) == parent,
        "the bars are siblings of the log control, which is what puts them on top of it");
    Check(GetClassLongPtrW(logVerticalBar, GCLP_HBRBACKGROUND) != 0,
        "the bar windows have a background brush, so exposed areas are never left uninitialised");
    // Without this the control paints straight over the covers whenever it redraws its own scroll
    // bar, which is what put the system bar back on screen in the first place.
    Check((GetWindowLongPtrW(logEdit, GWL_STYLE) & WS_CLIPSIBLINGS) != 0 &&
          (GetWindowLongPtrW(logVerticalBar, GWL_STYLE) & WS_CLIPSIBLINGS) != 0 &&
          (GetWindowLongPtrW(logHorizontalBar, GWL_STYLE) & WS_CLIPSIBLINGS) != 0,
        "the control and its bars clip each other, so the control cannot paint over the covers");
    SetWindowPos(logEdit, nullptr, Scaled(logSide), Scaled(logTop),
        Scaled(logEditWidth), Scaled(logEditHeight), SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(parent, SW_SHOW);
    ShowWindow(logEdit, SW_SHOW);
    ShowWindow(logVerticalBar, SW_SHOW);
    ShowWindow(logHorizontalBar, SW_SHOW);
    Pump();

    // The zone the bars belong in, worked out here from the layout constants rather than from the
    // code under test: beside the text on the right, and under it at the bottom.
    RECT zoneVertical{Scaled(logSide + logEditWidth), Scaled(logTop),
        Scaled(logSide + logEditWidth + logBarZone), Scaled(logTop + logEditHeight)};
    RECT zoneHorizontal{Scaled(logSide), Scaled(logTop + logEditHeight),
        Scaled(logSide + logEditWidth), Scaled(logTop + logEditHeight + logBarZone)};

    PositionLogBars();
    RECT vertical{}, horizontal{};
    GetWindowRect(logVerticalBar, &vertical);
    GetWindowRect(logHorizontalBar, &horizontal);
    MapWindowPoints(nullptr, parent, (POINT*)&vertical, 2);
    MapWindowPoints(nullptr, parent, (POINT*)&horizontal, 2);
    Check(EqualRect(&vertical, &zoneVertical) && EqualRect(&horizontal, &zoneHorizontal),
        "each bar fills its zone next to the painted border");

    POINT probe{zoneVertical.left + (zoneVertical.right - zoneVertical.left) / 2,
        zoneVertical.top + (zoneVertical.bottom - zoneVertical.top) / 2};
    RECT barRect{};
    GetWindowRect(logVerticalBar, &barRect);
    MapWindowPoints(nullptr, parent, (POINT*)&barRect, 2);
    // Off screen ChildWindowFromPointEx will not name the bar, so what is checked here is the part
    // that API does report: the bar owns that rectangle. That a click there really reaches the bar
    // is checked against the screen in the on-screen suite, where WindowFromPoint is authoritative.
    Check(PtInRect(&barRect, probe) && IsWindowVisible(logVerticalBar),
        "the bar owns its zone and is visible");
    POINT textProbe{Scaled(logSide) + 5, Scaled(logTop) + 5};
    Check(ChildWindowFromPointEx(parent, textProbe, CWP_SKIPINVISIBLE) == logEdit,
        "the space the bars do not use belongs to the text box");

    RECT barClient{}, sideClient{};
    GetClientRect(logVerticalBar, &barClient);
    GetClientRect(logHorizontalBar, &sideClient);
    int height = barClient.bottom, width = sideClient.right;
    RECT verticalBox{0, 0, barClient.right, barClient.bottom};
    RECT horizontalBox{0, 0, sideClient.right, sideClient.bottom};
    int memoryWidth = verticalBox.right > horizontalBox.right ? verticalBox.right : horizontalBox.right;
    int memoryHeight = verticalBox.bottom > horizontalBox.bottom ? verticalBox.bottom : horizontalBox.bottom;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, memoryWidth, memoryHeight);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    RECT whole{0, 0, memoryWidth, memoryHeight};
    auto repaint = [&](HWND cover, int bar) {
        FillRect(memory, &whole, (HBRUSH)GetStockObject(WHITE_BRUSH));
        PaintLogBarLine(memory, cover, bar);
    };

    // Short content: completely blank, so no greyed-out bar and no arrow buttons.
    SetWindowTextW(logEdit, L"one short line");
    Pump();
    repaint(logVerticalBar, SB_VERT);
    PaintedBar blankVertical = ReadPaintedBar(memory, verticalBox, true);
    repaint(logHorizontalBar, SB_HORZ);
    PaintedBar blankHorizontal = ReadPaintedBar(memory, horizontalBox, false);
    Check(!blankVertical.any() && !blankHorizontal.any(),
        "content that fits paints nothing at all: no track and no arrow buttons");

    // Overflowing both ways: one continuous line per bar, thin, and hugging the outer edge. The
    // content goes in through the production append path so the launcher's own measurements see it.
    std::wstring wide(600, L'x');
    AppendLog(wide);
    for (int line = 0; line < 40; ++line) AppendLog(wide + L" " + std::to_wstring(line));
    Pump();
    SendMessageW(logEdit, EM_LINESCROLL, 0, -100000);
    SendMessageW(logEdit, EM_LINESCROLL, -100000, 0);
    Pump();
    SyncLogHorizontalOffset();

    // Overflow alone is not enough any more: the bars belong to the reader's attention, so with the
    // pointer elsewhere and nothing selected they paint nothing.
    logHovered = false;
    logSelected = false;
    repaint(logVerticalBar, SB_VERT);
    PaintedBar quietVertical = ReadPaintedBar(memory, verticalBox, true);
    repaint(logHorizontalBar, SB_HORZ);
    PaintedBar quietHorizontal = ReadPaintedBar(memory, horizontalBox, false);
    Check(!quietVertical.any() && !quietHorizontal.any(),
        "overflowing content on its own shows no line: the bars wait for a hover or a selection");

    // Selecting the log is one of the two things that brings them out.
    SendMessageW(logEdit, EM_SETSEL, 0, (LPARAM)-1);
    RefreshLogBars();
    Check(logSelected, "a selection is noticed");
    repaint(logVerticalBar, SB_VERT);
    PaintedBar verticalLine = ReadPaintedBar(memory, verticalBox, true);
    repaint(logHorizontalBar, SB_HORZ);
    PaintedBar horizontalLine = ReadPaintedBar(memory, horizontalBox, false);
    Check(verticalLine.any() && horizontalLine.any(), "with a selection each bar paints its line");
    Check(verticalLine.runs == 1 && horizontalLine.runs == 1,
        "each cover holds a single line: no track, no arrow buttons, no end caps");
    int wanted = Scaled(logBarThickness);
    Check(verticalLine.widestRow >= wanted && verticalLine.widestRow <= wanted + 1 &&
          horizontalLine.widestRow >= wanted && horizontalLine.widestRow <= wanted + 1,
        "the line is as thick as configured (the one extra pixel is anti-aliasing)");
    Check(verticalLine.length() < height && horizontalLine.length() < width,
        "each line covers part of its cover only, so it reads as a bar and not as a filled track");
    int leftGap = verticalLine.acrossFirst;
    int rightGap = verticalBox.right - 1 - verticalLine.acrossLast;
    int topGap = horizontalLine.acrossFirst;
    int bottomGap = horizontalBox.bottom - 1 - horizontalLine.acrossLast;
    int edge = Scaled(logBarEdgeGap);
    Check(rightGap >= edge - 1 && rightGap <= edge + 1 && leftGap > rightGap,
        "the vertical line hugs the box border, leaving the blank space on the text side");
    Check(bottomGap >= edge - 1 && bottomGap <= edge + 1 && topGap > bottomGap,
        "the horizontal line hugs the box border, leaving the blank space on the text side");
    std::printf("      bars: vertical %d long, %dpx thick, gaps %d/%d of %d   horizontal %d long, %dpx thick, gaps %d/%d of %d\n",
        verticalLine.length(), verticalLine.widestRow, leftGap, rightGap, height,
        horizontalLine.length(), horizontalLine.widestRow, topGap, bottomGap, width);

    // A shorter log leaves a larger visible share, so its line has to grow.
    std::wstring twelve;
    ClearLog();
    for (int line = 0; line < 12; ++line) AppendLog(L"line " + std::to_wstring(line));
    SendMessageW(logEdit, EM_LINESCROLL, 0, -100000);
    Pump();
    repaint(logVerticalBar, SB_VERT);
    PaintedBar shorter = ReadPaintedBar(memory, verticalBox, true);
    Check(shorter.any() && shorter.length() > verticalLine.length(),
        "a shorter log draws a longer line, following the visible share of the content");

    // Dragging a bar has to move the log: that is what the reader actually uses it for. Clearing the
    // log throws the selection away, so it is made again — that is what keeps the line on screen here,
    // the pointer being wherever the machine's mouse happens to be.
    ClearLog();
    for (int line = 0; line < 40; ++line) AppendLog(wide + L" " + std::to_wstring(line));
    SendMessageW(logEdit, EM_LINESCROLL, 0, -100000);
    SendMessageW(logEdit, EM_SETSEL, 0, (LPARAM)-1);
    RefreshLogBars();
    Pump();
    int lineBefore = (int)SendMessageW(logEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
    LPARAM bottomOfBar = MAKELPARAM(2, height - 2);
    SendMessageW(logVerticalBar, WM_LBUTTONDOWN, MK_LBUTTON, bottomOfBar);
    SendMessageW(logVerticalBar, WM_MOUSEMOVE, MK_LBUTTON, bottomOfBar);
    SendMessageW(logVerticalBar, WM_LBUTTONUP, 0, bottomOfBar);
    Pump();
    int lineAfter = (int)SendMessageW(logEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
    Check(lineAfter > lineBefore, "dragging the vertical cover scrolls the log down");

    SendMessageW(logEdit, EM_LINESCROLL, 0, -100000);
    SendMessageW(logEdit, EM_LINESCROLL, -100000, 0);
    Pump();
    int xBefore = (int)(short)LOWORD(SendMessageW(logEdit, EM_POSFROMCHAR, 0, 0));
    LPARAM rightOfBar = MAKELPARAM(width - 2, 2);
    SendMessageW(logHorizontalBar, WM_LBUTTONDOWN, MK_LBUTTON, rightOfBar);
    SendMessageW(logHorizontalBar, WM_MOUSEMOVE, MK_LBUTTON, rightOfBar);
    SendMessageW(logHorizontalBar, WM_LBUTTONUP, 0, rightOfBar);
    Pump();
    int xAfter = (int)(short)LOWORD(SendMessageW(logEdit, EM_POSFROMCHAR, 0, 0));
    Check(xAfter < xBefore, "dragging the horizontal cover scrolls the text sideways");

    // Scrolled to the end, the line has to travel to the far end of its cover.
    SendMessageW(logEdit, EM_LINESCROLL, 0, 100000);
    Pump();
    repaint(logVerticalBar, SB_VERT);
    PaintedBar atEnd = ReadPaintedBar(memory, verticalBox, true);
    Check(atEnd.last >= height - 2, "the vertical line follows the scroll position to the end");

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    DestroyWindow(logEdit);
    logEdit = nullptr;
    logVerticalBar = logHorizontalBar = nullptr;
    DestroyWindow(parent);
}

// ---------------------------------------------------------------- on-screen check
//
// Everything above drives the windows off screen, where a cover can look perfect while the real
// screen still shows the system bar underneath it ?which is exactly how a "background brush missing"
// bug survived a green test run. This mode puts the log box on screen, captures the screen itself,
// and repeats that through the states that used to give the system bar away: hover, scrolling, a
// resize and a drag. It ends with a negative control that hides the covers and insists the system
// bar becomes visible, so a capture that is simply showing nothing cannot pass.

struct Seen { int systemTrack = 0, systemThumb = 0, ourLine = 0, white = 0, other = 0; };

static void Settle(int milliseconds) {
    DWORD until = GetTickCount() + milliseconds;
    while (GetTickCount() < until) { Pump(); Sleep(15); }
}

// Capture one bar's own rectangle off the screen. There is no system scroll bar left to look for:
// what matters is that the bar window really is what the reader sees there, line and all.
static Seen LookAtScreen(HWND barWindow) {
    Seen seen;
    RECT strip{};
    GetWindowRect(barWindow, &strip);
    int width = strip.right - strip.left, height = strip.bottom - strip.top;
    if (width <= 0 || height <= 0) return seen;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, screen, strip.left, strip.top, SRCCOPY);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            COLORREF pixel = GetPixel(memory, x, y);
            if (pixel == RGB(240, 240, 240)) ++seen.systemTrack;
            else if (pixel == RGB(133, 133, 133)) ++seen.systemThumb;
            else if (pixel == RGB(203, 213, 225)) ++seen.ourLine;
            else if (pixel == RGB(255, 255, 255)) ++seen.white;
            else ++seen.other;
        }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return seen;
}

static void ReportScreen(const char* what, HWND barWindow) {
    Seen seen = LookAtScreen(barWindow);
    std::printf("      %-26s systemTrack=%-5d systemThumb=%-5d ourLine=%-5d white=%-5d other=%d\n",
        what, seen.systemTrack, seen.systemThumb, seen.ourLine, seen.white, seen.other);
}

static void RunScreenChecks() {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int frameWidth = Scaled(W) + 40, frameHeight = Scaled(H_EXPANDED) + 40;
    int left = work.right - frameWidth - 8, top = work.bottom - frameHeight - 8;
    HWND frame = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, left, top,
        frameWidth, frameHeight, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!frame) { Check(false, "a frame window for the on-screen check could be created"); return; }
    CreateLogEdit(frame);
    SetWindowPos(logEdit, nullptr, Scaled(logSide), Scaled(logTop),
        Scaled(logEditWidth), Scaled(logEditHeight), SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(frame, SW_SHOW);
    SetWindowPos(frame, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(logEdit, SW_SHOW);
    ShowWindow(logVerticalBar, SW_SHOW);
    ShowWindow(logHorizontalBar, SW_HIDE);
    Settle(150);

    std::wstring wide(600, L'x');
    AppendLog(wide);
    for (int line = 0; line < 40; ++line) AppendLog(wide + L" " + std::to_wstring(line));
    Pump();
    SendMessageW(logEdit, EM_LINESCROLL, 0, -100000);
    PositionLogBars();
    Settle(250);

    // The bars belong to the reader's attention: with the pointer away from the box and nothing
    // selected they must not be on screen at all. The cursor is parked out of the way first so this is
    // not a question of where the machine's mouse happens to be.
    POINT savedCursor{};
    GetCursorPos(&savedCursor);
    SetCursorPos(work.left + 4, work.top + 4);
    SendMessageW(logEdit, EM_SETSEL, 0, 0);
    RefreshLogBars();
    Settle(200);
    Seen idle = LookAtScreen(logVerticalBar);
    ReportScreen("idle (pointer away, no selection)", logVerticalBar);
    Check(!logHovered && !logSelected && idle.ourLine == 0 && idle.systemTrack == 0,
        "on screen: nothing is drawn while the reader is not working with the log");

    // Selecting the log is enough on its own.
    SendMessageW(logEdit, EM_SETSEL, 0, (LPARAM)-1);
    RefreshLogBars();
    Settle(200);
    Seen selected = LookAtScreen(logVerticalBar);
    ReportScreen("with a selection", logVerticalBar);
    Check(selected.ourLine > 0, "on screen: a selection brings the line out");

    // So is the pointer over the box.
    SendMessageW(logEdit, EM_SETSEL, 0, 0);
    RECT boxScreen{};
    GetWindowRect(logEdit, &boxScreen);
    SetCursorPos((boxScreen.left + boxScreen.right) / 2, (boxScreen.top + boxScreen.bottom) / 2);
    mouse_event(MOUSEEVENTF_MOVE, 1, 0, 0, 0);
    Settle(250);
    Seen hovering = LookAtScreen(logVerticalBar);
    ReportScreen("pointer over the log box", logVerticalBar);
    Check(hovering.ourLine > 0, "on screen: hovering the log box brings the line out");

    Seen rest = LookAtScreen(logVerticalBar);
    // Real hit testing, on the real screen: a click on the bar has to reach the bar, not the text.
    RECT barScreen{};
    GetWindowRect(logVerticalBar, &barScreen);
    POINT barCentre{(barScreen.left + barScreen.right) / 2, (barScreen.top + barScreen.bottom) / 2};
    Check(WindowFromPoint(barCentre) == logVerticalBar,
        "on screen: the bar is the window at that point, so it can be grabbed");
    ReportScreen("at rest", logVerticalBar);
    Check(rest.systemTrack == 0 && rest.systemThumb == 0 && rest.ourLine > 0,
        "on screen: the system bar is hidden and only our line shows");

    // Keep a selection for the remaining states, so the line stays out whatever the pointer does.
    SendMessageW(logEdit, EM_SETSEL, 0, (LPARAM)-1);
    RefreshLogBars();
    Settle(150);

    // The pointer leaving takes the line away again, once the selection is gone as well.
    SendMessageW(logEdit, EM_SETSEL, 0, 0);
    RefreshLogBars();
    Settle(200);
    Seen stillOverBox = LookAtScreen(logVerticalBar);
    ReportScreen("selection cleared, pointer still over the box", logVerticalBar);
    Check(stillOverBox.ourLine > 0, "on screen: the pointer alone keeps the line out");
    SetCursorPos(work.left + 4, work.top + 4);
    mouse_event(MOUSEEVENTF_MOVE, 1, 0, 0, 0);
    Settle(250);
    Seen pointerAway = LookAtScreen(logVerticalBar);
    ReportScreen("pointer away again", logVerticalBar);
    Check(pointerAway.ourLine == 0, "on screen: the line goes away when the pointer leaves");

    // Keep a selection for the remaining states, so they do not depend on where the pointer is.
    SendMessageW(logEdit, EM_SETSEL, 0, (LPARAM)-1);
    RefreshLogBars();
    Settle(150);

    SendMessageW(logEdit, EM_LINESCROLL, 0, 7);
    InvalidateLogBars();
    Settle(200);
    Seen scrolled = LookAtScreen(logVerticalBar);
    ReportScreen("after scrolling", logVerticalBar);
    Check(scrolled.systemTrack == 0 && scrolled.systemThumb == 0 && scrolled.ourLine > 0,
        "on screen: scrolling keeps the system bar hidden");

    SetWindowPos(logEdit, nullptr, Scaled(logSide), Scaled(logTop), Scaled(W - 2 * logSide) - 40,
        Scaled(logHeight) - 30, SWP_NOZORDER | SWP_NOACTIVATE);
    PositionLogBars();
    Settle(250);
    Seen resized = LookAtScreen(logVerticalBar);
    ReportScreen("after resize", logVerticalBar);
    Check(resized.systemTrack == 0 && resized.systemThumb == 0 && resized.ourLine > 0,
        "on screen: resizing keeps the system bar hidden");

    RECT barClient{};
    GetClientRect(logVerticalBar, &barClient);
    SendMessageW(logVerticalBar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, barClient.bottom / 2));
    SendMessageW(logVerticalBar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(2, barClient.bottom - 3));
    Settle(250);
    Seen dragging = LookAtScreen(logVerticalBar);
    ReportScreen("mid-drag", logVerticalBar);
    Check(dragging.systemTrack == 0 && dragging.systemThumb == 0 && dragging.ourLine > 0,
        "on screen: dragging shows our own line, not the system bar");
    SendMessageW(logVerticalBar, WM_LBUTTONUP, 0, MAKELPARAM(2, barClient.bottom - 3));

    // The control: with the bar hidden its line has to disappear from the screen. Without this, a
    // capture that is simply showing nothing could pass for a working bar.
    ShowWindow(logVerticalBar, SW_HIDE);
    Settle(250);
    Seen control = LookAtScreen(logVerticalBar);
    ReportScreen("control: bar hidden", logVerticalBar);
    Check(control.ourLine == 0,
        "on screen: hiding the bar does remove the line, so these captures are meaningful");
    ShowWindow(logVerticalBar, SW_SHOW);
    PositionLogBars();
    Settle(200);
    Seen restored = LookAtScreen(logVerticalBar);
    Check(restored.ourLine > 0, "on screen: showing the bar brings the line back");

    // The horizontal bar as well, since it is a separate window and had its own history.
    ShowWindow(logHorizontalBar, SW_SHOW);
    PositionLogBars();
    Settle(250);
    Seen horizontal = LookAtScreen(logHorizontalBar);
    ReportScreen("horizontal bar", logHorizontalBar);
    Check(horizontal.systemTrack == 0 && horizontal.systemThumb == 0 && horizontal.ourLine > 0,
        "on screen: the horizontal bar shows its own line and no system chrome");

    SetCursorPos(savedCursor.x, savedCursor.y);
    DestroyWindow(logEdit);
    logEdit = nullptr;
    logVerticalBar = logHorizontalBar = nullptr;
    DestroyWindow(frame);
}

// ---------------------------------------------------------------- parent repaint vs the log box
//
// What the window does on a hover, and whether any of it reaches the log box. Measured through the
// region Windows is about to repaint rather than by watching pixels: a flicker is a transient, so a
// before/after screenshot cannot see it — by the time it is taken the box has already been redrawn.

static LRESULT CALLBACK PlainParentProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        // Same shape as the launcher's own painting: fill the whole client through a memory bitmap.
        // Red here, so a parent that paints over its child is unmistakable on screen.
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ previous = SelectObject(buffer, bitmap);
        RECT whole{0, 0, client.right, client.bottom};
        HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
        FillRect(buffer, &whole, red);
        DeleteObject(red);
        BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, previous);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

// A window repaint that covers the log box leaves the box needing a repaint of its own — that is the
// flicker: the box is wiped and comes back a moment later, which a before/after screenshot cannot see
// because by then it has already come back. The box's own update state can see it though.
static bool ChildNeedsRepaintAfterParentPaint(DWORD style) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW info{sizeof(info)};
        info.lpfnWndProc = PlainParentProc;
        info.hInstance = GetModuleHandleW(nullptr);
        info.lpszClassName = L"ProbeRepaintParent";
        RegisterClassExW(&info);
        registered = true;
    }
    HWND parent = CreateWindowExW(0, L"ProbeRepaintParent", L"", style,
        -3000, -3000, 420, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent) return false;
    CreateLogEdit(parent);
    SetWindowPos(logEdit, nullptr, Scaled(logSide), Scaled(logTop),
        Scaled(logEditWidth), Scaled(logEditHeight), SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(parent, SW_SHOW);
    ShowWindow(logEdit, SW_SHOW);
    Pump();
    UpdateWindow(logEdit);          // start from a clean slate

    // What a hover on a button does: the whole window repaints.
    InvalidateRect(parent, nullptr, TRUE);
    UpdateWindow(parent);
    Pump();
    RECT update{};
    bool dirty = GetUpdateRect(logEdit, &update, FALSE) != 0;

    DestroyWindow(logEdit);
    logEdit = nullptr;
    logVerticalBar = logHorizontalBar = nullptr;
    DestroyWindow(parent);
    return dirty;
}

static void CheckParentRepaint() {
    Check((mainWindowStyle & WS_CLIPCHILDREN) != 0,
        "the launcher window clips its children, which is what a window that owns controls should do");
    bool dirty = ChildNeedsRepaintAfterParentPaint(mainWindowStyle);
    // Measured, not assumed: repainting the window does not disturb the log box — the DC BeginPaint
    // hands out is already clipped to exclude the children. The flicker the reader saw over the log
    // while the pointer crossed the buttons was fixed by repainting only the button involved (see
    // CheckHoverRepaint), not by this style; it stays because it is what the erase path wants.
    Check(!dirty, "repainting the window leaves the log box alone");
}

// ---------------------------------------------------------------- hover repaints only the button
//
// The flicker over the log box while the pointer crossed the buttons stopped once the hover handling
// stopped repainting the whole window. This drives the launcher's own window procedure and checks the
// region Windows is about to repaint: it has to be the button, and it must not include the log box.
static LRESULT CALLBACK TestWindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CREATE) {
        // The launcher's WM_CREATE without anything that talks to the network or the service.
        windowHandle = hwnd;
        logEdit = nullptr;
        CreateLogEdit(hwnd);
        Layout();
        return 0;
    }
    return WindowProc(hwnd, message, wp, lp);
}

static void CheckHoverRepaint() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW info{};
        info.lpfnWndProc = TestWindowProc;
        info.hInstance = GetModuleHandleW(nullptr);
        info.lpszClassName = L"TestLauncherWindow";
        RegisterClassW(&info);
        registered = true;
    }
    HWND window = CreateWindowExW(0, L"TestLauncherWindow", L"", mainWindowStyle,
        -2000, -2000, Scaled(W), Scaled(H_EXPANDED), nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) { Check(false, "the launcher window could be created for the hover check"); return; }
    ShowWindow(window, SW_SHOW);
    ExpandLog(true);
    Pump();
    UpdateWindow(window);
    UpdateWindow(logEdit);

    // From no button onto the 启动 button: what a reader does when the pointer crosses the row. The
    // region is read before pumping, because the paint consumes it.
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(Scaled(firstButtonX + 2), Scaled(buttonTop + 2)));
    RECT update{};
    GetUpdateRect(window, &update, FALSE);
    int wide = update.right - update.left, high = update.bottom - update.top;
    RECT box{Scaled(logSide), Scaled(logTop), Scaled(logSide + logEditWidth), Scaled(logTop + logEditHeight)};
    RECT overlap{};
    std::printf("      hover repaint region: %dx%d at (%d,%d), log box at (%d,%d)-(%d,%d)\n",
        wide, high, update.left, update.top, box.left, box.top, box.right, box.bottom);
    Check(wide > 0 && wide <= Scaled(buttonWidth) + 4 && high <= Scaled(buttonHeight) + 4,
        "hovering a button invalidates that button only, not the whole window");
    Check(!IntersectRect(&overlap, &update, &box), "a hover repaint does not include the log box");
    Pump();

    // And leaving the window puts only the hovered button back.
    SendMessageW(window, WM_MOUSELEAVE, 0, 0);
    GetUpdateRect(window, &update, FALSE);
    Check(!IntersectRect(&overlap, &update, &box), "leaving the window does not repaint the log box");
    Pump();

    DestroyWindow(window);
    // The launcher's WM_DESTROY posts a quit message; the checks after this one still need to pump.
    MSG quit{};
    PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE);
    windowHandle = nullptr;
    logEdit = nullptr;
    logVerticalBar = logHorizontalBar = nullptr;
}

int wmain(int argc, wchar_t** argv) {
    // Same startup the launcher performs: the bar painting goes through GDI+, which silently draws
    // nothing until it has been initialised.
    GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);
    int result = RunChecks(argc, argv);
    GdiplusShutdown(gdiplusToken);
    return result;
}

static int RunChecks(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--discover") { Discover(); return 0; }
    if (argc > 1 && std::wstring(argv[1]) == L"--screen") {
        RunScreenChecks();
        std::printf(failures ? "\n%d check(s) failed\n" : "\non-screen checks passed\n", failures);
        return failures ? 1 : 0;
    }

    // Observed on the reporting machine: ~/.npmrc holds `allow-scripts=opencode-ai`, and
    // the npm bundled with the system Node (11.13.0) quotes the key back as a warning.
    std::wstring noisy =
        L"npm warn Unknown user config \"allow-scripts\". This will stop working in the next major version of npm. "
        L"See `npm help npmrc` for supported config options.\n"
        L"\"0.1.5-rc.2\"\n";
    Check(QuotedVersion(noisy) == L"0.1.5-rc.2", "warning + answer yields the real version");
    Check(QuotedVersion(L"\"0.1.5-rc.2\"\n") == L"0.1.5-rc.2", "clean output still parses");
    Check(QuotedVersion(L"npm warn Unknown user config \"allow-scripts\". This will stop working.\n").empty(),
        "a warning alone yields no version (the check reports a failure instead of a fake update)");
    Check(QuotedVersion(L"").empty(), "empty output yields no version");
    Check(QuotedVersion(L"{\"error\":{\"code\":\"EPERM\"}}\n").empty(), "npm error json yields no version");
    Check(QuotedVersion(L"npm notice New major version of npm available! 10.9.0 -> 11.17.0\n").empty(),
        "npm self-update notice is not mistaken for the package version");
    Check(LooksLikeVersion(L"0.1.5-rc.2") && LooksLikeVersion(L"1.0.0") && LooksLikeVersion(L"0.1"),
        "version shapes accepted");
    Check(!LooksLikeVersion(L"allow-scripts") && !LooksLikeVersion(L"latest") && !LooksLikeVersion(L""),
        "non-versions rejected");
    Check(!(std::wstring(L"0.1.5-rc.2") != QuotedVersion(noisy)),
        "installed 0.1.5-rc.2 vs that output is NOT an update");

    Check(CompareVersions(L"11.17.0", L"11.13.0") > 0 && CompareVersions(L"11.13.0", L"11.17.0") < 0 &&
        CompareVersions(L"11.13.0", L"11.13.0") == 0, "version comparison orders correctly");
    Check(CompareVersions(L"11.13.0", L"11.13") == 0 && CompareVersions(L"12.0.0", L"11.99.99") > 0,
        "version comparison handles missing components");
    Check(MajorOf(L"11.17.0") == 11 && MajorOf(L"12.0.0") == 12 && MajorOf(L"") == 0, "major extraction");
    Check(SamePath(L"C:\\A\\b", L"c:\\a\\B") && !SamePath(L"a", L"ab"), "path comparison ignores case");

    // npm discovery and selection through the real code paths: three stand-in npm copies
    // are put on PATH, the middle one newer than the paired 11.13.0, the last one a major
    // version ahead (which must be ignored so it cannot outrun its Node.js).
    fs::path fixtures = fs::current_path() / L"npm-fixtures";
    fs::remove_all(fixtures);
    std::wstring a = MakeNpm(fixtures / L"a", L"11.13.0");
    std::wstring b = MakeNpm(fixtures / L"b", L"11.17.0");
    std::wstring c = MakeNpm(fixtures / L"c", L"12.0.0");
    Check(NpmCliVersion(a) == L"11.13.0" && NpmCliVersion(b) == L"11.17.0" && NpmCliVersion(c) == L"12.0.0",
        "npm version is read from the package next to npm-cli.js");

    std::wstring savedPath = PathVariable();
    std::wstring fixturesPath = (fixtures / L"a").wstring() + L";" + (fixtures / L"b").wstring() + L";" +
        (fixtures / L"c").wstring() + L";" + savedPath;
    SetEnvironmentVariableW(L"PATH", fixturesPath.c_str());
    NodeTools tools;
    tools.npm = a;  // paired with a stand-in Node.js; only the npm side is under test
    PreferNewestNpm(tools);
    std::string chosen = Narrow(tools.npmVersion) + " " + Narrow(tools.npm);
    Check(MajorOf(tools.npmVersion) == 11, ("npm upgrade stays inside one major version: " + chosen).c_str());
    Check(!SamePath(tools.npm, c), "the major-ahead npm 12.0.0 is not chosen");
    Check(CompareVersions(tools.npmVersion, L"11.17.0") >= 0,
        ("the newest same-major npm wins: " + chosen).c_str());
    Check(!SamePath(tools.npm, a), "the older paired npm is replaced");

    SetEnvironmentVariableW(L"PATH", savedPath.c_str());
    fs::remove_all(fixtures);

    // System detection on this machine, without the fixtures in the way.
    if (auto system = SystemNode()) {
        Check(!system->npmVersion.empty(), "the system npm version is detected and reported");
        fs::path bundled = fs::path(system->node).parent_path() / L"node_modules" / L"npm" / L"bin" / L"npm-cli.js";
        std::wstring bundledVersion = NpmCliVersion(bundled.wstring());
        if (!bundledVersion.empty() && MajorOf(bundledVersion) == MajorOf(system->npmVersion))
            Check(CompareVersions(system->npmVersion, bundledVersion) >= 0,
                "the selected npm is at least as new as the one bundled with Node.js");
    } else {
        std::printf("note: no system Node.js here, skipped the detection checks\n");
    }

    // Log panel geometry: the panel is 30% shorter than the original 157px, the expanded
    // window follows the panel height, and the painted border still encloses the text box
    // (Layout and Paint encode this separately, so the two must not drift apart).
    Check(logHeight == 110, "log panel height is 110px (30% shorter than the original 157px)");
    Check(H_EXPANDED == 181, "expanded window height follows the panel: 53 + 110 + 18 = 181");
    Check(logBoxTop < logTop && logBoxTop + logBoxHeight > logTop + logHeight,
        "the painted log border encloses the text box");
    Check(W - 2 * logSide > 0, "the log text box keeps a positive width");

    // Right-click menu geometry. The shell's own item bands are not laid out squarely inside the popup
    // (measured: 6px from the left edge but 5px from the right, 4px above the first item and 1px below
    // the last), and the selection box used to inset 3px sideways but only 2px vertically on top of
    // that. The items tile the client now and the box takes the same margin on every side, which is
    // what the reader asked for: the gap above the first item matches the gap to its left.
    RECT menuClient{0, 0, 96, 57};
    int menuCount = 2, menuMargin = Scaled(menuItemInset);
    RECT firstBand = MenuItemBand(menuClient, menuCount, 0);
    RECT secondBand = MenuItemBand(menuClient, menuCount, 1);
    Check(firstBand.top == menuClient.top && secondBand.bottom == menuClient.bottom &&
          firstBand.bottom == secondBand.top,
        "the two menu items tile the popup, leaving no leftover padding on any side");
    Check(firstBand.right == menuClient.right && firstBand.left == menuClient.left,
        "each item spans the popup's width, so both boxes are the same width");
    RECT first = HighlightRect(firstBand, menuMargin);
    RECT second = HighlightRect(secondBand, menuMargin);
    Check(first.left - menuClient.left == first.top - menuClient.top &&
          first.left - menuClient.left == menuClient.right - first.right,
        "the first selection box keeps the same margin on the top, left and right");
    Check(second.left - menuClient.left == menuClient.bottom - second.bottom,
        "the last selection box keeps the same margin at the bottom as at its left");
    Check(second.top > first.bottom, "the two boxes stay apart, with a gap between them");
    std::printf("      menu box margins: top %d, left %d, right %d, bottom %d (item %dx%d)\n",
        first.top - menuClient.top, first.left - menuClient.left, menuClient.right - first.right,
        menuClient.bottom - second.bottom, firstBand.right - firstBand.left, firstBand.bottom - firstBand.top);

    CheckLogBars();

    CheckParentRepaint();
    CheckHoverRepaint();

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
