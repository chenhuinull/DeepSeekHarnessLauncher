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
// Run with --discover to print the Node.js/npm copies found on this machine, --screen for the
// on-screen checks, and --probe to run one health probe against the service that is running now
// and print the line the launcher writes to its log every five seconds.

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
// content, auto-follow, keyboard and the wheel still work, and a selection can be made and kept —
// reported as "日志框里无法进行选择复制了", which had two causes: the launcher's own mouse handling
// swallowed the pointer moves an edit extends a selection with, so a drag selected nothing at all,
// and every appended line parked the caret at the end of the text and took a selection made by
// double-click or the keyboard with it.
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

// What the menu's "复制" ends up doing, read back the way the reader would.
static std::wstring ClipboardText() {
    std::wstring text;
    if (!OpenClipboard(nullptr)) return text;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* data = (const wchar_t*)GlobalLock(handle)) {
            text = data;
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return text;
}

// Watches the log control from behind the launcher's own subclass: a message reaches this only if the
// launcher passed it on with DefSubclassProc, which is exactly what an edit needs to extend a
// selection from the pointer moves. Swallowing them was the reason "日志框里无法进行选择复制了": a drag
// left nothing selected, because only the button going down and coming up reached the control.
static LPARAM lastMoveSeen = 0;
static LRESULT CALLBACK MoveWatcherProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEMOVE) lastMoveSeen = lp;
    return DefSubclassProc(hwnd, message, wp, lp);
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
    {
        // The timestamp is followed straight away by the message: in this font a space is half a
        // Chinese glyph wide and showed up as a gap the reader did not want.
        int last = (int)SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0) - 1;
        wchar_t first[128]{};
        *reinterpret_cast<WORD*>(first) = 127;
        int copied = (int)SendMessageW(logEdit, EM_GETLINE, (WPARAM)last, (LPARAM)first);
        // The stamp is ten characters wide: [HH:MM:SS], so the bracket is at 9 and the message's
        // first character at 10.
        Check(copied > 10 && first[0] == L'[' && first[9] == L']' && first[10] != L' ',
            "the newest line starts with the timestamp, followed straight away by the message");
        int stamp = LogLineWidth(L"[00:11:39]");
        int space = LogLineWidth(L"[00:11:39] ") - stamp;
        std::printf("      timestamp %d px, the space it no longer carries %d px, a Chinese glyph %d px\n",
            stamp, space, LogLineWidth(L"\u5df2"));
    }

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

    // The reader's selection has to survive the output that keeps arriving. The log streams while the
    // service is talking, and appending means moving the caret to the end of the text to insert: that
    // took the selection with it, so "复制" was greyed out by the time the reader got the menu open.
    {
        ClearLog();
        Pump();
        for (int line = 0; line < 60; ++line) AppendLog(L"line " + std::to_wstring(line));
        Pump();
        int tail = (int)SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0);
        Check(FirstVisibleLine() > 0, "with nothing selected the view still follows the output");

        // The moves between button down and button up are what an edit extends a selection with, so
        // the launcher's own mouse handling has to pass them on. This watcher sits behind it in the
        // subclass chain and only sees a message the launcher forwarded; the coordinates are chosen so
        // that a move made by the machine's own mouse cannot be mistaken for it.
        RemoveWindowSubclass(logEdit, LogEditProc, 1);
        SetWindowSubclass(logEdit, MoveWatcherProc, 1, 0);
        SetWindowSubclass(logEdit, LogEditProc, 1, 0);
        lastMoveSeen = 0;
        LPARAM probe = MAKELPARAM(1234, 567);
        SendMessageW(logEdit, WM_MOUSEMOVE, 0, probe);
        Check(lastMoveSeen == probe,
            "the pointer moves reach the control, which is what extends a drag selection");
        RemoveWindowSubclass(logEdit, MoveWatcherProc, 1);

        SendMessageW(logEdit, EM_SETSEL, 120, 180);
        AppendLog(L"a line that arrives while the reader has something selected");
        Pump();
        LONG from = -1, to = -1;
        Check(LogSelectionRange(from, to) && from == 120 && to == 180,
            "a line arriving mid-read leaves the selection exactly where it was");
        Check(FirstVisibleLine() < tail, "and the view stays with the selection rather than at the tail");

        // What the selection says, read out of the box itself, so the clipboard check is against the
        // text the reader sees marked rather than against a substring this file assumed.
        int textLength = GetWindowTextLengthW(logEdit);
        std::wstring whole(textLength + 1, L'\0');
        whole.resize(GetWindowTextW(logEdit, whole.data(), textLength + 1));
        std::wstring expected = whole.substr(120, 60);
        SendMessageW(logEdit, WM_COPY, 0, 0);
        std::wstring copied = ClipboardText();
        Check(expected.size() == 60 && copied == expected,
            "the selection is still what the menu's 复制 puts on the clipboard");

        // The drag that makes a selection in the first place holds the control's capture for its whole
        // length, and a write during it would move the caret out from under the pointer: those lines
        // wait for the button to come up.
        int before = (int)SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0);
        SetCapture(logEdit);
        AppendLog(L"queued while the button is down");
        Check((int)SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0) == before && logDeferred.size() == 1,
            "a line that arrives during a drag waits instead of moving the caret");
        ReleaseCapture();
        SendMessageW(logEdit, WM_LBUTTONUP, 0, 0);
        Pump();
        Check((int)SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0) == before + 1,
            "the button coming up writes what the drag held back");
        Check(logDeferred.empty(), "and nothing is left waiting after that");
    }

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

// ---------------------------------------------------------------- fold flicker probe
//
// Folding moves the window, and a move plus a repaint is where a compositor can present a frame that
// belongs to the state before. Numbers about our own painting cannot see that: what the reader sees is
// the composed screen. This mode drives the real launcher window with a real double-click on the whale
// and captures the screen as fast as it can, recording what every frame actually showed.
struct FoldFrame {
    double atMs = 0;
    int width = 0, left = 0;
    int cross = 0, plate = 0, borders = 0;
    int crossFrom = -1, crossTo = -1, plateFrom = -1, plateTo = -1;
    bool operator==(const FoldFrame& other) const {
        return width == other.width && left == other.left && cross == other.cross &&
            plate == other.plate && borders == other.borders;
    }
};

static void SaveScreenArea(const wchar_t* path, int screenLeft, int screenTop, int width, int height);

static int RunFoldProbe() {
    HWND target = FindWindowW(L"DeepSeekHarnessLauncherNative", nullptr);
    if (!target) {
        std::printf("the launcher is not running, so there is nothing to fold\n");
        return 0;
    }
    ShowWindow(target, SW_SHOW);
    SetForegroundWindow(target);
    RECT box{};
    GetWindowRect(target, &box);
    int width = box.right - box.left, height = box.bottom - box.top;
    if (width <= 0 || height <= 0) { std::printf("the launcher window has no size\n"); return 0; }
    // The whale, which is what the reader double-clicks to fold the chip.
    int whaleX = box.left + Scaled(iconX + iconWidth / 2);
    int whaleY = box.top + Scaled(iconTop + iconHeight / 2);

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    std::vector<unsigned char> pixels((size_t)width * height * 4);

    // Read the window's own region of the screen and describe what is in it: the close button's cross,
    // the plate the window buttons share, and button borders — none of which exist in a folded chip.
    auto grab = [&](double atMs) {
        FoldFrame frame;
        frame.atMs = atMs;
        RECT now{};
        GetWindowRect(target, &now);
        frame.width = now.right - now.left;
        frame.left = now.left;
        BitBlt(memory, 0, 0, width, height, screen, box.left, box.top, SRCCOPY);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;          // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (!GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS)) return frame;
        for (int y = 0; y < height; ++y) {
            const unsigned char* row = pixels.data() + (size_t)y * width * 4;
            int run = 0;
            for (int x = 0; x < width; ++x) {
                int b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
                if (std::abs(r - 185) <= 30 && std::abs(g - 28) <= 40 && std::abs(b - 28) <= 40) {
                    ++frame.cross;
                    if (frame.crossFrom < 0 || x < frame.crossFrom) frame.crossFrom = x;
                    if (x > frame.crossTo) frame.crossTo = x;
                } else if (std::abs(r - 203) <= 6 && std::abs(g - 213) <= 6 && std::abs(b - 225) <= 6) {
                    ++frame.borders;
                }
                bool plate = std::abs(r - 248) <= 3 && std::abs(g - 250) <= 3 && std::abs(b - 252) <= 3;
                run = plate ? run + 1 : 0;
                if (run >= 20) {
                    ++frame.plate;
                    if (frame.plateFrom < 0 || x - run + 1 < frame.plateFrom) frame.plateFrom = x - run + 1;
                    if (x > frame.plateTo) frame.plateTo = x;
                }
            }
        }
        return frame;
    };

    LARGE_INTEGER frequency{}, start{};
    QueryPerformanceFrequency(&frequency);
    auto nowMs = [&]() {
        LARGE_INTEGER at{};
        QueryPerformanceCounter(&at);
        return (double)(at.QuadPart - start.QuadPart) * 1000.0 / (double)frequency.QuadPart;
    };
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    auto click = [&]() {
        SetCursorPos(whaleX, whaleY);
        mouse_event(MOUSEEVENTF_MOVE, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        Sleep(60);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        double at = nowMs();                         // the click that folds or unfolds
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        return at;
    };

    for (int round = 0; round < 2; ++round) {
        std::vector<FoldFrame> frames;
        Pump();
        Sleep(400);                                  // settle, then start from a known shape
        Pump();
        double clickedAt = click();
        double until = nowMs() + 600;
        while (nowMs() < until) frames.push_back(grab(nowMs()));
        Pump();
        std::printf("\n%s: %d frames in %.0f ms (the double-click lands at t=%.1f ms)\n",
            round == 0 ? "fold" : "unfold", (int)frames.size(), 600.0, clickedAt);
        FoldFrame last{};
        bool first = true;
        for (const FoldFrame& frame : frames) {
            if (!first && frame == last) continue;
            std::printf("   %+7.2f ms  window %d wide at x=%d   cross=%d@[%d..%d] plate=%d@[%d..%d] borders=%d\n",
                frame.atMs - clickedAt, frame.width, frame.left, frame.cross, frame.crossFrom,
                frame.crossTo, frame.plate, frame.plateFrom, frame.plateTo, frame.borders);
            last = frame;
            first = false;
        }
        // Keep a picture of what the probe just looked at, so the shape can be checked with the eyes.
        RECT now{};
        GetWindowRect(target, &now);
        if (round == 0) {
            std::wstring chip = (fs::temp_directory_path() / L"dsh-probe-window-folded.png").wstring();
            SaveScreenArea(chip.c_str(), now.left, now.top, now.right - now.left, now.bottom - now.top);
            std::wstring crop = (fs::temp_directory_path() / L"dsh-probe-chip.png").wstring();
            SaveScreenArea(crop.c_str(), now.right - Scaled(W_MINI), now.top, Scaled(W_MINI), Scaled(H_COLLAPSED));
        } else {
            std::wstring whole = (fs::temp_directory_path() / L"dsh-probe-window-unfolded.png").wstring();
            SaveScreenArea(whole.c_str(), now.left, now.top, now.right - now.left, now.bottom - now.top);
        }
        Sleep(250);
    }

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    std::printf("\nleft the launcher as it was found\n");
    return 0;
}

// Save the part of the screen the test just looked at, so the shape can be looked at with the eyes as
// well. GDI+ is started here because the test process never ran the launcher's entry point.
static void SaveScreenArea(const wchar_t* path, int screenLeft, int screenTop, int width, int height) {
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, screen, screenLeft, screenTop, SRCCOPY);
    SelectObject(memory, previous);
    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, nullptr) == Ok) {
        Gdiplus::Bitmap image(bitmap, nullptr);
        CLSID png{};
        UINT count = 0, size = 0;
        if (GetImageEncodersSize(&count, &size) == Ok && size) {
            std::vector<BYTE> buffer(size);
            auto* encoders = (ImageCodecInfo*)buffer.data();
            if (GetImageEncoders(count, size, encoders) == Ok) {
                for (UINT i = 0; i < count; ++i)
                    if (std::wstring(encoders[i].MimeType) == L"image/png") { png = encoders[i].Clsid; break; }
            }
        }
        if (png != CLSID{}) {
            std::printf("saved %ls\n", path);
            image.Save(path, &png, nullptr);
        }
        GdiplusShutdown(token);
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

static void ReportScreen(const char* what, HWND barWindow) {
    Seen seen = LookAtScreen(barWindow);
    std::printf("      %-26s systemTrack=%-5d systemThumb=%-5d ourLine=%-5d white=%-5d other=%d\n",
        what, seen.systemTrack, seen.systemThumb, seen.ourLine, seen.white, seen.other);
}

// A window that paints itself with the launcher's own paint code, so the title bar can be looked at
// on the real screen. The swap the reader asked for is a question about pixels: whether the drawing
// followed the rects, or stayed behind where the whale used to be.
// The part of a window a region actually shows, in window coordinates. Folding is a region change, so
// this is what says where the chip is.
static RECT VisibleBox(HWND window) {
    RECT box{};
    if (HRGN region = CreateRectRgn(0, 0, 0, 0)) {
        if (GetWindowRgn(window, region) != ERROR) GetRgnBox(region, &box);
        DeleteObject(region);
    }
    return box;
}

static int probeTitleBarPaints = 0;   // how many times the probe window has painted itself

static LRESULT CALLBACK ProbeTitleBarProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: ++probeTitleBarPaints; Paint(); return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

static HWND MakeTitleBarWindow(int left, int top) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW info{sizeof(info)};
        info.lpfnWndProc = ProbeTitleBarProc;
        info.hInstance = GetModuleHandleW(nullptr);
        info.lpszClassName = L"ProbeTitleBar";
        info.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&info);
        registered = true;
    }
    return CreateWindowExW(WS_EX_TOOLWINDOW, L"ProbeTitleBar", L"", WS_POPUP,
        left, top, Scaled(W), Scaled(H_COLLAPSED), nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// A solid icon, so the pixels it leaves are known: whatever comes back in that colour is the whale's
// slot, wherever the layout has put it.
static HICON MakeSolidIcon(int size, COLORREF color) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP colour = CreateDIBSection(screen, (BITMAPINFO*)&header, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!colour || !bits) { if (colour) DeleteObject(colour); return nullptr; }
    DWORD* pixels = (DWORD*)bits;
    // 0xAARRGGBB, which is what a 32-bit BGRA DIB wants: packing it the other way round swaps red
    // and blue and the "blue" icon comes back red.
    DWORD value = 0xFF000000u | ((DWORD)GetRValue(color) << 16) | ((DWORD)GetGValue(color) << 8) | GetBValue(color);
    for (int i = 0; i < size * size; ++i) pixels[i] = value;
    std::vector<BYTE> zero((size_t)(((size + 15) / 16) * 2) * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zero.data());
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = colour;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(colour);
    if (mask) DeleteObject(mask);
    return icon;
}

struct TitleBarPixels { int icon = 0, firstX = -1, lastX = -1, red = 0, redLastX = -1,
                        dash = 0, dashFirstX = -1, lamp = 0, lampFirstX = -1, lampLastX = -1,
                        lampFirstY = -1, lampLastY = -1,
                        frame = 0, frameFirstX = -1, frameLastX = -1,
                        frameLeft = 0, frameRight = 0, frameTop = 0, frameBottom = 0,
                        midLeft = 0, midLeftFull = 0, midRight = 0, midRightFull = 0,
                        midTop = 0, midTopFull = 0, midBottom = 0, midBottomFull = 0,
                        cornerTL = 0, cornerTR = 0, cornerBL = 0, cornerBR = 0,
                        cornerTLDark = 255, cornerTRDark = 255, cornerBLDark = 255, cornerBRDark = 255,
                        edgeTop = 0, edgeBottom = 0, edgeLeft = 0, edgeRight = 0,
                        plate = 0, plateFirstX = -1, plateLastX = -1; };

// The exact pixel profile across each edge of the window, printed so an asymmetric hairline shows up in
// the output and not only as a failed check ("." marks a pixel that is not neutral grey).
static void PrintBorderProfile(HWND window, int width, int height) {
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    RECT box{};
    GetWindowRect(window, &box);
    BitBlt(memory, 0, 0, width, height, screen, box.left, box.top, SRCCOPY);
    auto show = [&](const char* what, int fixed, bool horizontal, int from, int to) {
        std::string line;
        for (int i = from; i < to; ++i) {
            COLORREF pixel = horizontal ? GetPixel(memory, i, fixed) : GetPixel(memory, fixed, i);
            int r = GetRValue(pixel), g = GetGValue(pixel), b = GetBValue(pixel);
            line += (r == g && g == b) ? std::to_string(r) : std::string(".");
            line += ' ';
        }
        std::printf("      %-7s %s\n", what, line.c_str());
    };
    show("left", height / 2, false, 0, 6);
    show("right", height / 2, false, width - 6, width);
    show("top", width / 2, true, 0, 6);
    show("bottom", width / 2, true, height - 6, height);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

static TitleBarPixels ScanTitleBar(HWND window, int originX, int originY, int width, int height) {
    TitleBarPixels seen;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    RECT box{};
    GetWindowRect(window, &box);
    BitBlt(memory, 0, 0, width, height, screen, box.left + originX, box.top + originY, SRCCOPY);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            COLORREF pixel = GetPixel(memory, x, y);
            // Close to the pen's own colour: ClearType puts coloured fringes on the button labels, and
            // a loose "reddish" test would read those as the close button.
            if (GetBValue(pixel) > 200 && GetRValue(pixel) < 40 && GetGValue(pixel) < 40) {
                ++seen.icon;
                if (seen.firstX < 0 || x < seen.firstX) seen.firstX = x;
                if (x > seen.lastX) seen.lastX = x;
            } else if (std::abs((int)GetRValue(pixel) - 185) <= 30 &&
                       std::abs((int)GetGValue(pixel) - 28) <= 40 &&
                       std::abs((int)GetBValue(pixel) - 28) <= 40) {
                ++seen.red;                       // the close button's cross is the only red up here
                if (x > seen.redLastX) seen.redLastX = x;
            } else if (std::abs((int)GetRValue(pixel) - 71) <= 20 &&
                       std::abs((int)GetGValue(pixel) - 85) <= 20 &&
                       std::abs((int)GetBValue(pixel) - 105) <= 20) {
                ++seen.dash;                      // the minimize glyph, a lone dark bar
                if (seen.dashFirstX < 0 || x < seen.dashFirstX) seen.dashFirstX = x;
            } else if (std::abs((int)GetRValue(pixel) - 234) <= 25 &&
                       std::abs((int)GetGValue(pixel) - 179) <= 30 &&
                       std::abs((int)GetBValue(pixel) - 8) <= 40) {
                ++seen.lamp;                      // the "not started" lamp, the one colour nothing else uses
                if (seen.lampFirstX < 0 || x < seen.lampFirstX) seen.lampFirstX = x;
                if (x > seen.lampLastX) seen.lampLastX = x;
                if (seen.lampFirstY < 0 || y < seen.lampFirstY) seen.lampFirstY = y;
                if (y > seen.lampLastY) seen.lampLastY = y;
            } else if (GetRValue(pixel) == GetGValue(pixel) && GetGValue(pixel) == GetBValue(pixel) &&
                       GetRValue(pixel) >= 180 && GetRValue(pixel) <= 250) {
                // The outermost border, #B8B8B8, or that colour blended with the white behind it. A
                // 1.5px antialiased stroke has almost no pixel of the pure colour, so the test is for
                // neutral grey: nothing else in the title bar has equal red, green and blue (the button
                // borders are blue-grey, the labels are navy, the plate is off-white).
                ++seen.frame;
                if (seen.frameFirstX < 0 || x < seen.frameFirstX) seen.frameFirstX = x;
                if (x > seen.frameLastX) seen.frameLastX = x;
                // Counted per side, so a stroke that is half outside the window region on one side shows
                // up as a side with fewer pixels rather than as a missing border nobody looks at.
                if (x <= 4) ++seen.frameLeft;
                else if (x >= width - 5) ++seen.frameRight;
                if (y <= 4) ++seen.frameTop;
                else if (y >= height - 5) ++seen.frameBottom;
                // The same, but with the corners left out: the antialiased corner arcs reach into the edge
                // bands, so only the straight part of each side is counted for thickness.
                const int straight = 8;
                if (y <= 1 && x >= straight && x < width - straight) ++seen.edgeTop;
                if (y >= height - 2 && x >= straight && x < width - straight) ++seen.edgeBottom;
                if (x <= 1 && y >= straight && y < height - straight) ++seen.edgeLeft;
                if (x >= width - 2 && y >= straight && y < height - straight) ++seen.edgeRight;
                // Along the middle line only: how many pixels of the border each side shows there, and how
                // many of them are the full colour rather than a partly covered neighbour.
                bool full = std::abs((int)GetRValue(pixel) - 184) <= 8;
                if (y == height / 2) {
                    if (x <= 5) { ++seen.midLeft; if (full) ++seen.midLeftFull; }
                    else if (x >= width - 6) { ++seen.midRight; if (full) ++seen.midRightFull; }
                }
                if (x == width / 2) {
                    if (y <= 5) { ++seen.midTop; if (full) ++seen.midTopFull; }
                    else if (y >= height - 6) { ++seen.midBottom; if (full) ++seen.midBottomFull; }
                }
                // The corner zones, where the border has to turn: drawn on the window region's own edge the
                // arcs were clipped away entirely and the corners had no border at all.
                const int zone = 5;
                int value = (int)GetRValue(pixel);
                if (x < zone && y < zone) { ++seen.cornerTL; if (value < seen.cornerTLDark) seen.cornerTLDark = value; }
                else if (x >= width - zone && y < zone) { ++seen.cornerTR; if (value < seen.cornerTRDark) seen.cornerTRDark = value; }
                else if (x < zone && y >= height - zone) { ++seen.cornerBL; if (value < seen.cornerBLDark) seen.cornerBLDark = value; }
                else if (x >= width - zone && y >= height - zone) { ++seen.cornerBR; if (value < seen.cornerBRDark) seen.cornerBRDark = value; }
            }
        }
    }
    // The plate the two window buttons share is the same shade as a disabled button, and single
    // antialiased pixels along the button borders land within tolerance of it too. Only a run as wide
    // as the plate counts, so what comes back is the plate itself and not edge noise.
    for (int y = Scaled(buttonTop) + 1; y <= Scaled(buttonTop + buttonHeight) - 1 && y < height; ++y) {
        int run = 0;
        for (int x = 0; x <= width; ++x) {
            bool plate = false;
            if (x < width) {
                COLORREF pixel = GetPixel(memory, x, y);
                plate = std::abs((int)GetRValue(pixel) - 248) <= 3 &&
                        std::abs((int)GetGValue(pixel) - 250) <= 3 &&
                        std::abs((int)GetBValue(pixel) - 252) <= 3;
            }
            if (plate) { ++run; continue; }
            if (run >= 20) {
                if (seen.plateFirstX < 0 || x - run < seen.plateFirstX) seen.plateFirstX = x - run;
                if (x - 1 > seen.plateLastX) seen.plateLastX = x - 1;
                seen.plate += run;
            }
            run = 0;
        }
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return seen;
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

    // A drag is how a selection is normally made, and it is the one gesture that needs the control to
    // receive the moves between button down and button up. Real input on the real screen, because the
    // control's own drag only behaves as it does for the reader when the button is really held.
    {
        RECT dragBox{};
        GetWindowRect(logEdit, &dragBox);
        int x = dragBox.left + 8, y = dragBox.top + 12;
        // The frame is not the foreground window, so the first click is the window's, not the control's.
        SetCursorPos(x, y);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        Settle((int)GetDoubleClickTime() + 100);   // out of double-click range: this is a drag, not a pair
        SendMessageW(logEdit, EM_SETSEL, 0, 0);
        SetCursorPos(x, y);
        Pump();
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        for (int step = 1; step <= 6; ++step) { SetCursorPos(x + step * 20, y + step * 4); Settle(30); }
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        Settle(150);
        LONG dragFrom = 0, dragTo = 0;
        Check(LogSelectionRange(dragFrom, dragTo) && dragTo - dragFrom >= 10,
            "on screen: dragging across the log selects the text under the pointer");
        std::printf("      drag selected [%ld,%ld): %ld characters\n", dragFrom, dragTo, dragTo - dragFrom);
    }

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

    // The title bar itself, painted by the launcher's paint code and read back off the screen. The
    // window buttons and the whale traded places, and the whale is drawn from a rect: this is what
    // shows that the drawing followed the rect instead of staying where the whale used to be.
    {
        HWND bar = MakeTitleBarWindow(left, top);
        if (!bar) {
            Check(false, "a title bar window for the on-screen check could be created");
        } else {
            HWND previousWindow = windowHandle;
            HICON previousIcon = appIcon;
            bool previousMini = mini, previousExpanded = expanded, previousTopmost = topmost;
            State previousStatus = status;
            // Paint() reads the launcher's globals, so point them at this window and at a state whose
            // colours are known: unfolded, nothing busy, lamp stopped, and no accent-blue button.
            windowHandle = bar;
            mini = false; expanded = false; serverRunning = false; busy = false;
            topmost = false; status = State::Stopped; autoRestart = false;
            SetWindowPos(bar, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ShowWindow(bar, SW_SHOW);

            // Control first: with no icon set there must be no icon-coloured pixel at all, or
            // "found the whale" would not mean anything.
            appIcon = nullptr;
            InvalidateRect(bar, nullptr, TRUE);
            UpdateWindow(bar);
            Settle(150);
            TitleBarPixels none = ScanTitleBar(bar, 0, 0, Scaled(W), Scaled(H_COLLAPSED));
            Check(none.icon == 0 && none.red > 0,
                "on screen: with no icon set only the window buttons are drawn, so the icon colour is meaningful");

            appIcon = MakeSolidIcon(22, RGB(0, 0, 255));
            InvalidateRect(bar, nullptr, TRUE);
            UpdateWindow(bar);
            Settle(200);
            TitleBarPixels seen = ScanTitleBar(bar, 0, 0, Scaled(W), Scaled(H_COLLAPSED));
            UiRect draw = IconDrawRect();
            std::printf("      title bar %d px wide: icon at x=%d..%d (%d px, rect %d..%d), close cross x=%d, "
                "minimize dash x=%d, lamp x=%d..%d\n",
                Scaled(W), seen.firstX, seen.lastX, seen.icon, Scaled(draw.x),
                Scaled(draw.x + draw.w), seen.redLastX, seen.dashFirstX, seen.lampFirstX, seen.lampLastX);
            Check(seen.icon > 200, "on screen: the whale really is drawn in the title bar");
            Check(seen.firstX >= Scaled(draw.x) - 2 && seen.lastX <= Scaled(draw.x + draw.w) + 2,
                "on screen: the whale is drawn where its rect is, not at the spot it used to occupy");
            Check(seen.firstX > Scaled(W) / 2, "on screen: the whale sits at the right end of the title bar");
            Check(seen.red > 0 && seen.redLastX < Scaled(W) / 2,
                "on screen: the close button sits in the left half of the title bar");
            Check(seen.dash > 0 && seen.dashFirstX > seen.redLastX,
                "on screen: the cross is left of the minimize dash, so the two did swap");
            Check(seen.lamp > 0 && seen.lampFirstX > Scaled(W) / 2 && seen.lampLastX < seen.firstX &&
                  seen.firstX - seen.lampLastX <= Scaled(indicatorWidth),
                "on screen: the lamp is drawn right beside the whale, both at the right end");
            // The lamp's lit part, measured off the screen: it has to be the size that was asked for, and
            // round rather than a blob, so both spans are checked.
            std::printf("      lamp dot on screen: %dx%d px (asked for %d)\n",
                seen.lampLastX - seen.lampFirstX + 1, seen.lampLastY - seen.lampFirstY + 1, Scaled(lampDotDiameter));
            Check(seen.lampLastX - seen.lampFirstX + 1 >= Scaled(lampDotDiameter) - 1 &&
                  seen.lampLastY - seen.lampFirstY + 1 >= Scaled(lampDotDiameter) - 1,
                "on screen: the lamp dot is drawn at the bigger size that was asked for");
            Check(seen.lampLastX - seen.lampFirstX <= seen.lampLastY - seen.lampFirstY + 1 &&
                  seen.lampLastY - seen.lampFirstY <= seen.lampLastX - seen.lampFirstX + 1,
                "on screen: the lamp dot is as wide as it is tall, so it is still a circle");
            // The outermost border, read off the screen: #B8B8B8 along both vertical edges of the window.
            std::printf("      window border pixels: %d, x=%d..%d (window is %d wide)\n",
                seen.frame, seen.frameFirstX, seen.frameLastX, Scaled(W));
            std::printf("      border per side: left %d, right %d, top %d, bottom %d\n",
                seen.frameLeft, seen.frameRight, seen.frameTop, seen.frameBottom);
            PrintBorderProfile(bar, Scaled(W), Scaled(H_COLLAPSED));
            Check(seen.frame > 0 && seen.frameFirstX <= 2 && seen.frameLastX >= Scaled(W) - 3,
                "on screen: the window's outermost border is drawn in #B8B8B8 along both edges");
            Check(seen.frameLeft > 0 && seen.frameTop > 0 && seen.frameBottom > 0,
                "on screen: the border is drawn on the left, the top and the bottom");
            // Both vertical edges have to carry the same amount of stroke, and the stroke has to be the
            // full 1.5px: with the path only 1px inside the window region the outward half of the stroke
            // was clipped away and the count halved — which is exactly what "the border looks cut off"
            // is. Measured: 1531 border pixels as drawn, 785 with the old 1px inset.
            Check(seen.frameRight > 0 && seen.frameRight * 2 >= seen.frameLeft &&
                  seen.frameLeft * 2 >= seen.frameRight,
                "on screen: the right edge carries as much border as the left");
            // Each side is exactly one pixel thick along its straight part: one border pixel per column of
            // the top and bottom, per row of the left and right, corners left out. Too many means the
            // hairline was drawn twice or blurred over two columns (a stroked path does that), too few
            // means part of it fell outside the window region — both are "the border looks wrong", just in
            // different ways. A blurred border would roughly double these counts.
            std::printf("      straight parts: top %d, bottom %d, left %d, right %d (expect %d and %d)\n",
                seen.edgeTop, seen.edgeBottom, seen.edgeLeft, seen.edgeRight,
                Scaled(W) - 16, Scaled(H_COLLAPSED) - 16);
            Check(seen.edgeTop >= Scaled(W) - 20 && seen.edgeTop <= Scaled(W) - 12 &&
                  seen.edgeBottom >= Scaled(W) - 20 && seen.edgeBottom <= Scaled(W) - 12,
                "on screen: the top and bottom borders are one pixel thick");
            Check(seen.edgeLeft >= Scaled(H_COLLAPSED) - 20 && seen.edgeLeft <= Scaled(H_COLLAPSED) - 12 &&
                  seen.edgeRight >= Scaled(H_COLLAPSED) - 20 && seen.edgeRight <= Scaled(H_COLLAPSED) - 12,
                "on screen: the left and right borders are one pixel thick");
            // And each side is exactly one pixel of the full border colour at its own edge. A stroked
            // hairline spread itself over two columns and did it differently on the two sides, which is
            // what "the border is not complete" looked like on screen.
            std::printf("      middle line per side (grey/full): left %d/%d, right %d/%d, top %d/%d, bottom %d/%d\n",
                seen.midLeft, seen.midLeftFull, seen.midRight, seen.midRightFull,
                seen.midTop, seen.midTopFull, seen.midBottom, seen.midBottomFull);
            Check(seen.midLeft == 1 && seen.midLeftFull == 1 && seen.midRight == 1 && seen.midRightFull == 1 &&
                  seen.midTop == 1 && seen.midTopFull == 1 && seen.midBottom == 1 && seen.midBottomFull == 1,
                "on screen: each side is exactly one full pixel of #B8B8B8, so all four sides match");
            // The lamp dot has to sit in the middle of its button. Measured against where the layout says the
            // button is, so what is judged is the drawn dot and not the arithmetic that placed it.
            {
                UiRect lamp = LampRect();
                int buttonCentreX = Scaled(lamp.x + indicatorWidth / 2) * 2;
                int buttonCentreY = Scaled(buttonTop + buttonHeight / 2) * 2;
                int dotCentreX = seen.lampFirstX + seen.lampLastX + 1;
                int dotCentreY = seen.lampFirstY + seen.lampLastY + 1;
                std::printf("      lamp dot centre (%d,%d) vs its button's (%d,%d), both doubled\n",
                    dotCentreX, dotCentreY, buttonCentreX, buttonCentreY);
                Check(std::abs(dotCentreX - buttonCentreX) <= 1 && std::abs(dotCentreY - buttonCentreY) <= 1,
                    "on screen: the lamp dot is centred in its button, both across and down");
            }
            // The same two margins, read off the screen: the plate starts at the close button's left edge,
            // the whale ends at its own right edge.
            {
                int leftGap = seen.plateFirstX;
                int rightGap = Scaled(W) - 1 - seen.lastX;
                std::printf("      margins on screen: %d px before the window buttons, %d px after the whale\n",
                    leftGap, rightGap);
                // The plate is a filled rounded shape, so its first fully covered pixel sits about a pixel
                // and a half inside its own edge; two pixels of slack keep that out of the judgement while
                // still catching the six pixels the right margin used to be out by.
                Check(std::abs(leftGap - rightGap) <= 2,
                    "on screen: the whale's right margin matches the window buttons' left margin");
            }
            // The four corners: the border has to turn there. Drawn on the region's own edge the arcs were
            // clipped away and the corners had no border colour at all — which is what the reader saw.
            std::printf("      border pixels in the corner zones: TL %d, TR %d, BL %d, BR %d\n",
                seen.cornerTL, seen.cornerTR, seen.cornerBL, seen.cornerBR);
            Check(seen.cornerTL >= 3 && seen.cornerTR >= 3 && seen.cornerBL >= 3 && seen.cornerBR >= 3,
                "on screen: all four corners carry border pixels, so the frame goes right round");
            // And each corner has at least one solidly covered pixel. Clipped by the window region the arcs
            // came out as a faint, broken line: every pixel in the zone stayed near white.
            std::printf("      darkest corner pixel: TL %d, TR %d, BL %d, BR %d (full border is 184)\n",
                seen.cornerTLDark, seen.cornerTRDark, seen.cornerBLDark, seen.cornerBRDark);
            Check(seen.cornerTLDark <= 205 && seen.cornerTRDark <= 205 &&
                  seen.cornerBLDark <= 205 && seen.cornerBRDark <= 205,
                "on screen: every corner has a solidly drawn pixel, not a faint clipped arc");
            // The bug this replaced: the pair's plate was derived from the minimize button, so once
            // close led the row the plate covered the left half of the start button and the divider
            // was drawn on the outside edge. Both are read off the screen here.
            std::printf("      plate x=%d..%d (first button starts at %d), divider at %d\n",
                seen.plateFirstX, seen.plateLastX, Scaled(firstButtonX), Scaled(titleDividerX));
            Check(seen.plate > 0 && seen.plateFirstX <= Scaled(titleClusterX) + 2,
                "on screen: the window buttons' plate starts at the pair's own left edge");
            Check(seen.plateLastX < Scaled(firstButtonX),
                "on screen: the plate stops before the start button, so it cannot cover its label");
            Check(seen.dash > 0 && seen.red > 0 && seen.redLastX < seen.dashFirstX &&
                  seen.dashFirstX < Scaled(firstButtonX),
                "on screen: close and minimize sit inside the plate, in that order");

            // Fold it the way the launcher folds it, and check what the reader cares about: the window
            // itself does not move — the chip is a region at its right end — so the lamp and the whale
            // under the pointer stay exactly where they were.
            RECT before{};
            GetWindowRect(bar, &before);
            int whaleBefore = before.left + seen.firstX;
            mini = true;
            Layout();
            Settle(200);
            RECT after{};
            GetWindowRect(bar, &after);
            RECT chip = VisibleBox(bar);
            TitleBarPixels folded = ScanTitleBar(bar, chip.left, 0, chip.right - chip.left + 1, Scaled(H_COLLAPSED));
            int whaleAfter = after.left + chip.left + folded.firstX;
            std::printf("      folded chip %d px wide, region x=%d..%d of the window at x=%d: "
                "lamp x=%d..%d, icon x=%d..%d (screen %d, was %d)\n",
                chip.right - chip.left + 1, (int)chip.left, (int)chip.right, (int)after.left,
                folded.lampFirstX, folded.lampLastX, folded.firstX, folded.lastX, whaleAfter, whaleBefore);
            Check(folded.lamp > 0 && folded.icon > 0 && folded.lampLastX < folded.firstX,
                "on screen: folded, the lamp is on the left and the whale on the right");
            Check(folded.lampFirstX >= 0 && folded.lastX < chip.right - chip.left + 1,
                "on screen: both of the folded chip's controls are inside the narrow chip");
            Check(after.left == before.left && after.right == before.right,
                "on screen: folding does not move the window, so there is no geometry change to compose");
            Check(whaleAfter == whaleBefore,
                "on screen: the whale does not move when the chip folds, which is the point of folding this way");
            mini = false;

            if (appIcon) DestroyIcon(appIcon);
            appIcon = previousIcon;
            windowHandle = previousWindow;
            mini = previousMini; expanded = previousExpanded; topmost = previousTopmost; status = previousStatus;
            autoRestart = false;
            DestroyWindow(bar);
        }
    }

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
    if (argc > 1 && std::wstring(argv[1]) == L"--fold") return RunFoldProbe();
    if (argc > 1 && std::wstring(argv[1]) == L"--probe") {
        // Live: the same probe the launcher runs every five seconds, pointed at whatever service
        // is listening on the launcher's port right now, printed as the log line it produces.
        HarnessProbe probe = ProbeHarness((USHORT)serverPort);
        DWORD owner = PortOwnerPid(serverPort);
        // Printed as UTF-8 rather than through wprintf: the CRT would convert to the console code
        // page and turn the Chinese into question marks.
        std::wstring line = HealthProbeMessage(probe, probe.recognised ? 0 : 1, owner != 0);
        std::string utf8((size_t)WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, utf8.data(), (int)utf8.size(), nullptr, nullptr);
        if (!utf8.empty() && utf8.back() == '\0') utf8.pop_back();
        std::printf("port %d owner pid %u\n%s\n", (int)serverPort, owner, utf8.c_str());
        return 0;
    }
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

    // The five-second health check now writes a line every time it runs, so the shapes that line
    // can take are pinned here. The detail matters: "the port is gone" and "the port is open and
    // silent" send the reader to different places.
    {
        HarnessProbe alive{true, 200, true, 3};
        std::wstring healthy = HealthProbeMessage(alive, 0, true);
        Check(healthy.find(L"正常") != std::wstring::npos && healthy.find(L"HTTP 200") != std::wstring::npos &&
              healthy.find(L"3 ms") != std::wstring::npos,
            "a healthy check line reports the status code and how long the round trip took");
        Check(HealthProbeMessage(alive, 2, true).find(L"恢复正常") != std::wstring::npos,
            "a check that answers again after failures says so");

        HarnessProbe silent{false, 0, false, 1200};
        std::wstring portGone = HealthProbeMessage(silent, 1, false);
        std::wstring portOpen = HealthProbeMessage(silent, 3, true);
        Check(portGone.find(L"端口已无监听") != std::wstring::npos &&
              portOpen.find(L"端口仍在监听但没有应答") != std::wstring::npos,
            "a check with no answer says whether the port is still open, which is what tells a hang from a crash");
        Check(portGone.find(L"连续 1/3 次") != std::wstring::npos &&
              portOpen.find(L"连续 3/3 次") != std::wstring::npos,
            "a failing check counts how many have failed in a row, against the threshold");
        Check(portGone.find(L"1200 ms") != std::wstring::npos,
            "a failing check reports the time the failure cost, so a timeout is recognisable");

        HarnessProbe stranger{true, 502, false, 5};
        std::wstring wrong = HealthProbeMessage(stranger, 1, true);
        Check(wrong.find(L"应答不是") != std::wstring::npos && wrong.find(L"HTTP 502") != std::wstring::npos,
            "a page that is not the harness is reported as such rather than as a healthy server");
    }

    // Auto restart, the tray toggle: both failure paths share one counter and one backoff, a give-up
    // rule stops a broken install from looping, and the lamp turns purple while it is armed and running.
    {
        Check(AutoRestartDelayMs(0) == 3000 && AutoRestartDelayMs(1) == 6000 && AutoRestartDelayMs(2) == 12000,
            "the automatic restart backs off: 3s, then 6s, then 12s");
        Check(AutoRestartDelayMs(9) == autoRestartMaxDelayMs,
            "the backoff is capped, so the launcher never waits minutes between tries");
        Check(AutoRestartSettled(100000, 100000 + autoRestartSettledMs) &&
              !AutoRestartSettled(100000, 100000 + autoRestartSettledMs - 1) &&
              !AutoRestartSettled(0, 500000),
            "a start that stayed up a minute clears the counter, and a service that never started cannot");

        autoRestart = false;
        HMENU menu = BuildTrayMenu();
        Check(menu && GetMenuItemCount(menu) == 5,
            "the tray menu carries the window, service, auto restart and exit items plus a separator");
        wchar_t label[32]{};
        GetMenuStringW(menu, 2, label, 32, MF_BYPOSITION);
        Check(std::wstring(label) == L"自动重启" && (GetMenuState(menu, 2, MF_BYPOSITION) & MF_CHECKED) == 0,
            "the tray menu offers an auto restart item, unticked while the feature is off");
        DestroyMenu(menu);
        autoRestart = true;
        menu = BuildTrayMenu();
        Check(menu && (GetMenuState(menu, 2, MF_BYPOSITION) & MF_CHECKED) != 0,
            "the auto restart item is ticked while the feature is on");
        DestroyMenu(menu);

        status = State::Running;
        Check(ColorForStatus().GetValue() == 0xFFA855F7u,
            "a running service with auto restart armed shows the purple lamp");
        autoRestart = false;
        Check(ColorForStatus().GetValue() == 0xFF22C55Eu, "without it the running lamp stays green");
        autoRestart = true;
        Check(StatusName().find(L"自动重启") != std::wstring::npos, "the status says auto restart is on");
        status = State::Unresponsive;
        Check(ColorForStatus().GetValue() == 0xFFF97316u,
            "an unresponsive service keeps its amber lamp even with auto restart armed");
        status = State::Update;
        Check(ColorForStatus().GetValue() == 0xFFA855F7u,
            "the update lamp shares that purple, which is safe because it needs a stopped service");
        autoRestart = false;
        status = State::Missing;
    }

    // Settings outlive a run through settings.ini. The file format is checked here, including the
    // rule that a missing key or an unreadable value leaves the current setting alone, plus a real
    // save/load round trip in a temporary folder.
    {
        std::string text = "topmost=0\r\nautoRestart=1\nunknown=7\n";
        Check(ParseSetting(text, "topmost") == std::optional<bool>(false), "a stored 0 is read back as off");
        Check(ParseSetting(text, "autoRestart") == std::optional<bool>(true), "a stored 1 is read back as on");
        Check(!ParseSetting(text, "unknown").has_value() && !ParseSetting(text, "missing").has_value(),
            "a key the launcher does not know leaves the current setting alone");
        Check(!ParseSetting("topmost=maybe\n", "topmost").has_value(), "an unreadable value is ignored");
        std::string written = FormatSettings(false, true);
        Check(ParseSetting(written, "topmost") == std::optional<bool>(false) &&
              ParseSetting(written, "autoRestart") == std::optional<bool>(true),
            "whatever the launcher writes, the launcher reads back");

        fs::path savedRuntime = runtimeDir;
        fs::path probeRuntime = fs::temp_directory_path() / L"dsh-settings-check" / L"runtime";
        fs::remove_all(probeRuntime.parent_path());
        runtimeDir = probeRuntime;
        topmost = true; autoRestart = false;
        SaveSettings();
        Check(fs::exists(SettingsFile()), "saving writes settings.ini next to the runtime folder");
        topmost = false; autoRestart = true;
        LoadSettings();
        Check(topmost && !autoRestart, "both toggles come back the way they were left");
        topmost = false; autoRestart = true;
        SaveSettings();
        topmost = true; autoRestart = false;
        LoadSettings();
        Check(!topmost && autoRestart, "saving again replaces the file instead of appending to it");
        fs::remove_all(probeRuntime.parent_path());
        runtimeDir = savedRuntime;
        topmost = true; autoRestart = false;
    }

    // The title bar, after the reader asked for three moves: the window buttons and the whale traded
    // places, close and minimize swapped with each other, and the lamp came over to sit by the whale.
    {
        Check(closeRect.x < minRect.x && minRect.x + minRect.w <= firstButtonX,
            "close and minimize lead the title bar now, close first");
        // The pair shares one plate and one divider; both must come from the pair's own edges, or the
        // plate lands on the first button and the divider on the outside.
        Check(titleClusterX == closeX && titleClusterX + 2 * titleButtonWidth <= firstButtonX,
            "the shared plate covers both window buttons and stops before the first button");
        Check(titleDividerX == minRect.x && titleDividerX == closeRect.x + closeRect.w,
            "the divider between the two window buttons sits on their shared edge, not on the outside");
        Check(iconRect.x >= lastButtonRight && iconRect.x + iconRect.w <= W,
            "the whale closes the title bar, at the far right");
        Check(statusX == lastButtonRight + titleClusterGap &&
              iconRect.x == statusX + indicatorWidth + buttonGap,
            "the lamp moved to the end of the row, right beside the whale");
        Check(statusX >= lastButtonRight && statusX + indicatorWidth <= iconRect.x,
            "the lamp and the whale both sit past the last button, and do not overlap");
        Check(minRect.x + 2 * titleButtonWidth <= iconRect.x,
            "the window buttons and the whale stay at opposite ends, with the buttons between them");
        // The middle buttons run 置顶、更新、日志、启动/停止 from the left, which puts the button that
        // starts and stops the service next to the lamp that reports it.
        Check(topmostX == firstButtonX && updateX > topmostX && logX > updateX && startX > logX &&
              startX + buttonWidth == lastButtonRight,
            "the buttons read 置顶、更新、日志、启动/停止, in that order");
        Check(lastButtonRight <= statusX && startX + buttonWidth <= statusX,
            "启动/停止 ends up next to the lamp, with the other two before it");
        UiRect draw = IconDrawRect();
        Check(draw.x >= iconRect.x && draw.x + draw.w <= iconRect.x + iconRect.w &&
              draw.y >= iconRect.y && draw.y + draw.h <= iconRect.y + iconRect.h,
            "the icon is drawn inside its slot, so the drawing follows the rect that moved");
        // The gap the reader sees on the right is from the drawn icon to the window edge, and it has to
        // match the gap from the window edge to the drawn close button on the left. The icon is drawn
        // smaller than its slot and centred, so the slot margin alone is not that gap.
        {
            int leftGap = closeRect.x;
            int rightGap = W - (draw.x + draw.w);
            std::printf("      window margins: %d px before the close button, %d px after the whale\n",
                leftGap, rightGap);
            Check(leftGap == rightGap,
                "the whale leaves the same margin on the right as the close button does on the left");
        }
        Check(miniLampX == 10 && miniIconX == 42 && miniLampX < miniIconX,
            "folded, the lamp is on the left and the whale on the right");
        Check(W_MINI - (miniIconX + iconPadding + iconDrawSize) == miniLampX,
            "the folded chip leaves the same margin on both sides of its two controls");
        Check(W_MINI == miniIconX + iconWidth + rightMargin && W_MINI < 82,
            "the folded chip is narrower than it was, now that its right margin is the drawn one");
        Check(miniLampRect.x + miniLampRect.w <= W_MINI && miniIconRect.x + miniIconRect.w <= W_MINI,
            "both of the folded chip's controls still fit inside it");
        Check(!mini && LampRect().x == statusRect.x && IconRect().x == iconRect.x &&
              (mini = true, LampRect().x == miniOffsetX + miniLampX && IconRect().x == miniOffsetX + miniIconX),
            "the lamp and the whale rects follow the chip state, which is what the hit test uses");
        mini = false;
    }

    // The corner radius is one number for the buttons, the menu highlight, the log box and the window
    // frame, and the window region has to curve by the same amount or the shape and the stroke that
    // traces it disagree.
    Check(cornerRadius == 3.0f, "one corner radius, tightened to 3px, is shared by everything round");
    Check(windowCornerEllipsePx == (int)(2 * cornerRadius),
        "the window region's corner ellipse is twice the painted radius, so shape and frame agree");
    // The lit part of the lamp: bigger than the 10px dot it used to be, and still inside its button with
    // a margin all round, or the lamp would look like a filled square of colour.
    Check(lampDotDiameter > 10 && lampDotDiameter <= indicatorWidth - 6,
        "the lamp dot is bigger than it was and still leaves a margin inside the lamp button");
    Check(lampDotDiameter <= buttonHeight - 6, "the lamp dot fits the button's height too");
    // Centred means the margins on the two sides are equal: with an odd diameter one side gets the extra
    // pixel and the dot reads as sitting off to the left, which is what the reader saw.
    Check((indicatorWidth - lampDotDiameter) % 2 == 0 && (buttonHeight - lampDotDiameter) % 2 == 0,
        "the lamp dot leaves the same margin on both sides, so it is centred in its button");
    // The outermost border, in the colour the reader asked for (#B8B8B8), used by the painted frame and
    // by the frame the menus ask DWM for so the two keep matching.
    Check(windowBorderArgb == 0xFFB8B8B8u && windowBorderColor == RGB(0xB8, 0xB8, 0xB8),
        "the outermost border is #B8B8B8, in both the painted frame and the menu frame");

    // Folding: the window itself neither moves nor resizes any more. That is the point — a geometry
    // change is what made the compositor show the surface it already had at the new shape for a frame,
    // which the reader saw as a flicker. The chip is a window region at the right edge instead, so
    // these checks read the region, and the window rectangle is checked to stay exactly as it was.
    {
        HWND probe = MakeTitleBarWindow(-3000, -3000);
        if (!probe) {
            Check(false, "a window to lay out could be created");
        } else {
            HWND previousWindow = windowHandle;
            bool previousMini = mini, previousExpanded = expanded;
            windowHandle = probe;
            mini = false; expanded = false;
            ShowWindow(probe, SW_SHOW);      // visible but off screen: a hidden window gets no WM_PAINT
            Settle(120);
            Layout();
            RECT open{};
            GetWindowRect(probe, &open);
            RECT openBox = VisibleBox(probe);
            // The paint has to happen inside Layout: a queued one leaves the compositor showing the
            // surface it already has for a frame. The queued case is measured first, as the control
            // that says the counter can tell the two apart.
            probeTitleBarPaints = 0;
            InvalidateRect(probe, nullptr, FALSE);
            Check(probeTitleBarPaints == 0,
                "a queued repaint does not paint inside the call, which is the frame the reader saw");
            probeTitleBarPaints = 0;
            mini = true;
            Layout();
            Check(probeTitleBarPaints > 0, "folding paints inside the same call, not one frame later");
            std::printf("      fold painted %d time(s) before returning\n", probeTitleBarPaints);
            RECT folded{};
            GetWindowRect(probe, &folded);
            RECT foldedBox = VisibleBox(probe);
            Check(folded.left == open.left && folded.top == open.top && folded.right == open.right,
                "folding leaves the window rectangle alone, so there is no geometry change to compose");
            // CreateRoundRectRgn covers the rectangle minus its right and bottom edges, so the region has
            // to be asked for one pixel more: asked for exactly the box it comes back 343x47 and the
            // window's last column and row are outside it — the frame drawn there is clipped away, which
            // is what made the border vanish on exactly two sides.
            Check(foldedBox.left == Scaled(miniOffsetX) && foldedBox.top == 0 &&
                  foldedBox.right >= Scaled(W) && foldedBox.bottom >= Scaled(H_COLLAPSED),
                "the chip's region covers the window's last column and row, not one pixel less");
            Check(openBox.left == 0 && openBox.right >= Scaled(W) && openBox.bottom >= Scaled(H_COLLAPSED),
                "unfolded, the region covers the whole window including its last column and row");
            probeTitleBarPaints = 0;
            mini = false;
            Layout();
            RECT again{};
            GetWindowRect(probe, &again);
            Check(again.left == open.left && again.right == open.right,
                "unfolding leaves the window where it was as well");
            Check(probeTitleBarPaints > 0, "unfolding paints inside the same call as well");
            RECT back = VisibleBox(probe);
            Check(back.left == 0 && back.right >= Scaled(W) && back.bottom >= Scaled(H_COLLAPSED),
                "unfolding covers the whole window again");
            windowHandle = previousWindow;
            mini = previousMini; expanded = previousExpanded;
            DestroyWindow(probe);
        }
    }

    CheckLogBars();

    CheckParentRepaint();
    CheckHoverRepaint();

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
