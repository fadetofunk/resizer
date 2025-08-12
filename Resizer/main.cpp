// main.cpp
// Win32 C++ app with FFmpeg: drag & drop a video, show middle-frame screenshot.
// Adds a simple video player with controls and correct paused-step preview.
// Playback thread no longer uses goto; avoids MSVC E0546.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>
#include <string>
#include <cmath>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <process.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

// ------------------------------ Control IDs ------------------------------
#define IDC_INFO_STATIC           1001
#define IDC_SIZE_STATIC           1002
#define IDC_SIZE_EDIT             1003
#define IDC_SUFFIX_STATIC         1008
#define IDC_SUFFIX_EDIT           1009
#define IDC_RANGE_FULL_RADIO      1010
#define IDC_RANGE_CUSTOM_RADIO    1011
#define IDC_START_STATIC          1012
#define IDC_START_EDIT            1013
#define IDC_END_STATIC            1014
#define IDC_END_EDIT              1015
#define IDC_SCALE_FULL_RADIO      1004
#define IDC_SCALE_HALF_RADIO      1005
#define IDC_SCALE_QUARTER_RADIO   1006
#define IDC_START_BUTTON          1007

#define IDC_SEEKBAR               2001
#define IDC_BTN_PLAYPAUSE         2002
#define IDC_BTN_FWD               2003
#define IDC_BTN_BACK              2004
#define IDC_BTN_MARKIN            2005
#define IDC_BTN_MARKOUT           2006

#define IDT_UI_REFRESH            3001

// ------------------------------ Globals ------------------------------
#define WM_APP_FRAME_READY (WM_APP + 1)
static HWND     g_mainHwnd = nullptr;

static char     g_inputPath[MAX_PATH] = { 0 };
static int      g_vidWidth = 0;
static int      g_vidHeight = 0;
static double   g_duration = 0.0; // seconds

static HWND     g_hInfoStatic = nullptr;
static HWND     g_hSizeStatic = nullptr;
static HWND     g_hSizeEdit = nullptr;
static HWND     g_hSuffixStatic = nullptr;
static HWND     g_hSuffixEdit = nullptr;
static HWND     g_hRangeFullRadio = nullptr;
static HWND     g_hRangeCustomRadio = nullptr;
static HWND     g_hStartStatic = nullptr;
static HWND     g_hStartEdit = nullptr;
static HWND     g_hEndStatic = nullptr;
static HWND     g_hEndEdit = nullptr;
static HWND     g_hFullRadio = nullptr;
static HWND     g_hHalfRadio = nullptr;
static HWND     g_hQuarterRadio = nullptr;
static HWND     g_hStartButton = nullptr;

static HWND     g_hSeekbar = nullptr;
static HWND     g_hBtnPlayPause = nullptr;
static HWND     g_hBtnFwd = nullptr;
static HWND     g_hBtnBack = nullptr;
static HWND     g_hBtnMarkIn = nullptr;
static HWND     g_hBtnMarkOut = nullptr;

static HBITMAP  g_hFrameBitmap = nullptr;
static int      g_frameWidth = 0;
static int      g_frameHeight = 0;

static HANDLE   g_hPlaybackThread = nullptr;
static CRITICAL_SECTION g_csState;
static volatile bool g_playThreadShouldExit = false;
static volatile bool g_isPlaying = false;
static volatile bool g_seekRequested = false;
static volatile int64_t g_seekTargetMs = 0;
static volatile int64_t g_currentPosMs = 0;
static volatile bool g_decodeSingleFrame = false;
static double   g_videoFPS = 30.0;
static bool     g_playerReady = false;
static volatile int g_stepDir = 0; // +1 forward, -1 backward

// Forward declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void HandleResize(HWND hwnd, int clientW, int clientH);
bool GetVideoInfo(const char* filepath, int& width, int& height, double& durationSeconds);
HBITMAP ExtractMiddleFrameBitmap(const char* filepath, int orig_w, int orig_h, double duration);
bool TranscodeWithSizeAndScale(const char* in_filename, const char* out_filename, double target_size_mb,
    int scale_factor, int orig_w, int orig_h, double start_seconds, double end_seconds);

// Playback helpers
unsigned __stdcall PlaybackThreadProc(void*);
void StartPlayback(HWND hwnd);
void StopPlayback();
void TogglePlayPause(HWND hwnd);
void EnsureThreadRunningPaused(HWND hwnd);
void SeekMs(int64_t ms, bool decodeSingle);
void StepForward(HWND hwnd);
void StepBackward(HWND hwnd);
void UpdateSeekbarFromPos();
void SetMarkInFromCurrent(HWND hwnd);
void SetMarkOutFromCurrent(HWND hwnd);

// ------------------------------ UI Layout ------------------------------
void HandleResize(HWND hwnd, int clientW, int clientH) {
    const int margin = 10;

    int labelHeightSize;
    {
        HDC hdc = GetDC(hwnd);
        HFONT hFont = (HFONT)SendMessage(g_hSizeStatic, WM_GETFONT, 0, 0);
        SelectObject(hdc, hFont);
        wchar_t text[] = L"Target size (MB):";
        SIZE sz;
        GetTextExtentPoint32W(hdc, text, lstrlenW(text), &sz);
        ReleaseDC(hwnd, hdc);
        int labelW = sz.cx + 4;
        labelHeightSize = sz.cy;
        MoveWindow(g_hSizeStatic, margin, margin, labelW, labelHeightSize, TRUE);

        int editX = margin + labelW + margin;
        int editW = max(50, clientW - editX - margin);
        MoveWindow(g_hSizeEdit, editX, margin, editW, labelHeightSize, TRUE);
    }

    int ySuffix = margin + labelHeightSize + margin;
    int labelHeightSuffix;
    {
        HDC hdc = GetDC(hwnd);
        HFONT hFont = (HFONT)SendMessage(g_hSuffixStatic, WM_GETFONT, 0, 0);
        SelectObject(hdc, hFont);
        wchar_t text[] = L"Suffix:";
        SIZE sz;
        GetTextExtentPoint32W(hdc, text, lstrlenW(text), &sz);
        ReleaseDC(hwnd, hdc);
        int labelW = sz.cx + 4;
        labelHeightSuffix = sz.cy;
        MoveWindow(g_hSuffixStatic, margin, ySuffix, labelW, labelHeightSuffix, TRUE);

        int editX = margin + labelW + margin;
        int editW = max(50, clientW - editX - margin);
        MoveWindow(g_hSuffixEdit, editX, ySuffix, editW, labelHeightSuffix, TRUE);
    }

    int yRange = ySuffix + labelHeightSuffix + margin;
    MoveWindow(g_hRangeFullRadio, margin, yRange, 120, 20, TRUE);
    MoveWindow(g_hRangeCustomRadio, margin + 140, yRange, 120, 20, TRUE);
    MoveWindow(g_hStartStatic, margin + 280, yRange, 100, 20, TRUE);
    MoveWindow(g_hStartEdit, margin + 380, yRange, 80, 20, TRUE);
    MoveWindow(g_hEndStatic, margin + 480, yRange, 100, 20, TRUE);
    MoveWindow(g_hEndEdit, margin + 580, yRange, 80, 20, TRUE);

    int yRes = yRange + 30;
    MoveWindow(g_hFullRadio, margin, yRes, 300, 20, TRUE);
    MoveWindow(g_hHalfRadio, margin, yRes + 25, 300, 20, TRUE);
    MoveWindow(g_hQuarterRadio, margin, yRes + 50, 300, 20, TRUE);

    int btnY = yRes + 75;
    MoveWindow(g_hStartButton, margin, btnY, 150, 30, TRUE);

    int yPlayer = btnY + 40;
    int btnW = 90, btnH = 26;
    MoveWindow(g_hBtnPlayPause, margin, yPlayer, btnW, btnH, TRUE);
    MoveWindow(g_hBtnBack, margin + btnW + 6, yPlayer, btnW, btnH, TRUE);
    MoveWindow(g_hBtnFwd, margin + (btnW + 6) * 2, yPlayer, btnW, btnH, TRUE);
    MoveWindow(g_hBtnMarkIn, margin + (btnW + 6) * 3 + 20, yPlayer, btnW, btnH, TRUE);
    MoveWindow(g_hBtnMarkOut, margin + (btnW + 6) * 4 + 20, yPlayer, btnW, btnH, TRUE);

    int sbX = margin;
    int sbY = yPlayer + btnH + 8;
    int sbW = clientW - margin * 2;
    MoveWindow(g_hSeekbar, sbX, sbY, sbW, 28, TRUE);

    if (g_hFrameBitmap) {
        HDC hdc = GetDC(hwnd);
        HFONT hFont = (HFONT)SendMessage(g_hInfoStatic, WM_GETFONT, 0, 0);
        SelectObject(hdc, hFont);
        wchar_t buf[512];
        GetWindowTextW(g_hInfoStatic, buf, ARRAYSIZE(buf));
        SIZE sz;
        GetTextExtentPoint32W(hdc, buf, lstrlenW(buf), &sz);
        ReleaseDC(hwnd, hdc);
        MoveWindow(g_hInfoStatic, margin, sbY + 28 + 6, sz.cx, sz.cy, TRUE);
    }
    else {
        int staticW = 240, staticH = 20;
        int x = clientW - staticW - margin;
        int y = clientH - staticH - margin;
        MoveWindow(g_hInfoStatic, x, y, staticW, staticH, TRUE);
    }

    InvalidateRect(hwnd, nullptr, TRUE);
}

// ------------------------------ App Entry ------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icex);

    const wchar_t CLASS_NAME[] = L"FFmpegDragDropClass";
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClass(&wc)) {
        MessageBox(nullptr, L"Failed to register window class.", L"Error", MB_ICONERROR);
        return 0;
    }

    g_mainHwnd = CreateWindowEx(
        0, CLASS_NAME, L"Resizer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 760,
        nullptr, nullptr, hInstance, nullptr
    );
    if (!g_mainHwnd) {
        MessageBox(nullptr, L"Failed to create main window.", L"Error", MB_ICONERROR);
        return 0;
    }

    InitializeCriticalSection(&g_csState);

    ShowWindow(g_mainHwnd, nCmdShow);
    UpdateWindow(g_mainHwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DeleteCriticalSection(&g_csState);
    return (int)msg.wParam;
}

// ------------------------------ Window Proc ------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        DragAcceptFiles(hwnd, TRUE);

        g_hInfoStatic = CreateWindowEx(0, L"STATIC", L"Drop a video file onto this window",
            WS_CHILD | WS_VISIBLE, 10, 10, 240, 20, hwnd,
            (HMENU)IDC_INFO_STATIC, GetModuleHandle(nullptr), nullptr);

        g_hSizeStatic = CreateWindowEx(0, L"STATIC", L"Target size (MB):",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 10, 40, 100, 20, hwnd,
            (HMENU)IDC_SIZE_STATIC, GetModuleHandle(nullptr), nullptr);
        g_hSizeEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | ES_NUMBER,
            120, 40, 200, 20, hwnd, (HMENU)IDC_SIZE_EDIT, GetModuleHandle(nullptr), nullptr);

        g_hSuffixStatic = CreateWindowEx(0, L"STATIC", L"Suffix:",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 10, 70, 60, 20, hwnd,
            (HMENU)IDC_SUFFIX_STATIC, GetModuleHandle(nullptr), nullptr);
        g_hSuffixEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"RESIZED",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 80, 70, 200, 20, hwnd,
            (HMENU)IDC_SUFFIX_EDIT, GetModuleHandle(nullptr), nullptr);

        g_hRangeFullRadio = CreateWindowEx(0, L"BUTTON", L"Full video",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_AUTORADIOBUTTON | WS_GROUP,
            10, 100, 120, 20, hwnd, (HMENU)IDC_RANGE_FULL_RADIO, GetModuleHandle(nullptr), nullptr);
        g_hRangeCustomRadio = CreateWindowEx(0, L"BUTTON", L"Custom range",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_AUTORADIOBUTTON,
            150, 100, 120, 20, hwnd, (HMENU)IDC_RANGE_CUSTOM_RADIO, GetModuleHandle(nullptr), nullptr);

        g_hStartStatic = CreateWindowEx(0, L"STATIC", L"Start time (s):",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 290, 100, 100, 20, hwnd,
            (HMENU)IDC_START_STATIC, GetModuleHandle(nullptr), nullptr);
        g_hStartEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"0",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | ES_NUMBER,
            390, 100, 80, 20, hwnd, (HMENU)IDC_START_EDIT, GetModuleHandle(nullptr), nullptr);
        g_hEndStatic = CreateWindowEx(0, L"STATIC", L"End time (s):",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 480, 100, 100, 20, hwnd,
            (HMENU)IDC_END_STATIC, GetModuleHandle(nullptr), nullptr);
        g_hEndEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | ES_NUMBER,
            580, 100, 80, 20, hwnd, (HMENU)IDC_END_EDIT, GetModuleHandle(nullptr), nullptr);

        g_hFullRadio = CreateWindowEx(0, L"BUTTON", L"Full resolution",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_AUTORADIOBUTTON | WS_GROUP,
            10, 130, 300, 20, hwnd, (HMENU)IDC_SCALE_FULL_RADIO, GetModuleHandle(nullptr), nullptr);
        g_hHalfRadio = CreateWindowEx(0, L"BUTTON", L"Half resolution",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_AUTORADIOBUTTON,
            10, 155, 300, 20, hwnd, (HMENU)IDC_SCALE_HALF_RADIO, GetModuleHandle(nullptr), nullptr);
        g_hQuarterRadio = CreateWindowEx(0, L"BUTTON", L"Quarter resolution",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_AUTORADIOBUTTON,
            10, 180, 300, 20, hwnd, (HMENU)IDC_SCALE_QUARTER_RADIO, GetModuleHandle(nullptr), nullptr);

        g_hStartButton = CreateWindowEx(0, L"BUTTON", L"Start Processing",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_DEFPUSHBUTTON,
            10, 210, 150, 30, hwnd, (HMENU)IDC_START_BUTTON, GetModuleHandle(nullptr), nullptr);

        g_hBtnPlayPause = CreateWindowEx(0, L"BUTTON", L"Play",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 10, 250, 90, 26, hwnd,
            (HMENU)IDC_BTN_PLAYPAUSE, GetModuleHandle(nullptr), nullptr);
        g_hBtnBack = CreateWindowEx(0, L"BUTTON", L"Frame Back",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 110, 250, 90, 26, hwnd,
            (HMENU)IDC_BTN_BACK, GetModuleHandle(nullptr), nullptr);
        g_hBtnFwd = CreateWindowEx(0, L"BUTTON", L"Frame Fwd",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 210, 250, 90, 26, hwnd,
            (HMENU)IDC_BTN_FWD, GetModuleHandle(nullptr), nullptr);
        g_hBtnMarkIn = CreateWindowEx(0, L"BUTTON", L"Mark In",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 330, 250, 90, 26, hwnd,
            (HMENU)IDC_BTN_MARKIN, GetModuleHandle(nullptr), nullptr);
        g_hBtnMarkOut = CreateWindowEx(0, L"BUTTON", L"Mark Out",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 430, 250, 90, 26, hwnd,
            (HMENU)IDC_BTN_MARKOUT, GetModuleHandle(nullptr), nullptr);
        g_hSeekbar = CreateWindowEx(0, TRACKBAR_CLASS, L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | TBS_AUTOTICKS,
            10, 285, 600, 28, hwnd, (HMENU)IDC_SEEKBAR, GetModuleHandle(nullptr), nullptr);
        SendMessage(g_hSeekbar, TBM_SETRANGEMIN, TRUE, 0);
        SendMessage(g_hSeekbar, TBM_SETRANGEMAX, TRUE, 1000);
        SendMessage(g_hSeekbar, TBM_SETPAGESIZE, 0, 100);

        break;
    }

    case WM_SIZE: {
        int clientW = LOWORD(lParam);
        int clientH = HIWORD(lParam);
        HandleResize(hwnd, clientW, clientH);
        break;
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        if (DragQueryFileA(hDrop, 0, g_inputPath, MAX_PATH)) {
            if (GetVideoInfo(g_inputPath, g_vidWidth, g_vidHeight, g_duration)) {
                wchar_t infoText[512];
                double minutes = floor(g_duration / 60.0);
                double seconds = g_duration - minutes * 60.0;
                StringCchPrintfW(infoText, 512, L"%S   (%.0f:%02.0f)   %dx%d",
                    g_inputPath, minutes, seconds, g_vidWidth, g_vidHeight);
                SetWindowTextW(g_hInfoStatic, infoText);

                int fullW = g_vidWidth, fullH = g_vidHeight;
                int halfW = g_vidWidth / 2, halfH = g_vidHeight / 2;
                int quarterW = g_vidWidth / 4, quarterH = g_vidHeight / 4;
                wchar_t fullText[64], halfText[64], quarterText[64];
                StringCchPrintfW(fullText, 64, L"Full resolution    (%dx%d)", fullW, fullH);
                StringCchPrintfW(halfText, 64, L"Half resolution    (%dx%d)", halfW, halfH);
                StringCchPrintfW(quarterText, 64, L"Quarter resolution (%dx%d)", quarterW, quarterH);
                SetWindowTextW(g_hFullRadio, fullText);
                SetWindowTextW(g_hHalfRadio, halfText);
                SetWindowTextW(g_hQuarterRadio, quarterText);

                EnableWindow(g_hSizeStatic, TRUE);
                EnableWindow(g_hSizeEdit, TRUE);
                EnableWindow(g_hSuffixStatic, TRUE);
                EnableWindow(g_hSuffixEdit, TRUE);

                EnableWindow(g_hRangeFullRadio, TRUE);
                EnableWindow(g_hRangeCustomRadio, TRUE);
                SendMessage(g_hRangeFullRadio, BM_SETCHECK, BST_CHECKED, 0);
                SendMessage(g_hRangeCustomRadio, BM_SETCHECK, BST_UNCHECKED, 0);
                EnableWindow(g_hStartStatic, FALSE);
                EnableWindow(g_hStartEdit, FALSE);
                EnableWindow(g_hEndStatic, FALSE);
                EnableWindow(g_hEndEdit, FALSE);

                EnableWindow(g_hFullRadio, TRUE);
                EnableWindow(g_hHalfRadio, TRUE);
                EnableWindow(g_hQuarterRadio, TRUE);
                SendMessage(g_hFullRadio, BM_SETCHECK, BST_CHECKED, 0);
                SendMessage(g_hHalfRadio, BM_SETCHECK, BST_UNCHECKED, 0);
                SendMessage(g_hQuarterRadio, BM_SETCHECK, BST_UNCHECKED, 0);

                EnableWindow(g_hStartButton, TRUE);

                wchar_t startBuf[32], endBuf[32];
                StringCchPrintfW(startBuf, 32, L"0");
                int durInt = (int)floor(g_duration);
                StringCchPrintfW(endBuf, 32, L"%d", durInt);
                SetWindowTextW(g_hStartEdit, startBuf);
                SetWindowTextW(g_hEndEdit, endBuf);

                SendMessage(g_hSeekbar, TBM_SETRANGEMIN, TRUE, 0);
                SendMessage(g_hSeekbar, TBM_SETRANGEMAX, TRUE, (LPARAM)(int)(g_duration * 1000));
                SendMessage(g_hSeekbar, TBM_SETPOS, TRUE, 0);
                EnableWindow(g_hSeekbar, TRUE);

                EnableWindow(g_hBtnPlayPause, TRUE);
                EnableWindow(g_hBtnFwd, TRUE);
                EnableWindow(g_hBtnBack, TRUE);
                EnableWindow(g_hBtnMarkIn, TRUE);
                EnableWindow(g_hBtnMarkOut, TRUE);
                SetWindowTextW(g_hBtnPlayPause, L"Play");

                if (g_hFrameBitmap) { DeleteObject(g_hFrameBitmap); g_hFrameBitmap = nullptr; }
                g_hFrameBitmap = ExtractMiddleFrameBitmap(g_inputPath, g_vidWidth, g_vidHeight, g_duration);
                if (g_hFrameBitmap) {
                    BITMAP bi; GetObject(g_hFrameBitmap, sizeof(bi), &bi);
                    g_frameWidth = bi.bmWidth; g_frameHeight = bi.bmHeight;
                }

                g_isPlaying = false;
                g_currentPosMs = 0;
                g_playerReady = true;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            else {
                MessageBox(hwnd, L"Failed to retrieve video information.", L"Error", MB_ICONERROR);
                SetWindowTextW(g_hInfoStatic, L"Drop a video file onto this window");
            }
        }
        DragFinish(hDrop);

        RECT rc; GetClientRect(hwnd, &rc);
        HandleResize(hwnd, rc.right, rc.bottom);
        break;
    }

    case WM_HSCROLL: {
        if ((HWND)lParam == g_hSeekbar && g_playerReady) {
            if (LOWORD(wParam) == TB_THUMBTRACK || LOWORD(wParam) == TB_ENDTRACK ||
                LOWORD(wParam) == SB_LINELEFT || LOWORD(wParam) == SB_LINERIGHT ||
                LOWORD(wParam) == SB_PAGELEFT || LOWORD(wParam) == SB_PAGERIGHT) {
                int pos = (int)SendMessage(g_hSeekbar, TBM_GETPOS, 0, 0);
                bool wantSingle = !g_isPlaying;
                EnsureThreadRunningPaused(hwnd);
                SeekMs(pos, wantSingle);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        if (id == IDC_RANGE_FULL_RADIO) {
            SendMessage(g_hRangeFullRadio, BM_SETCHECK, BST_CHECKED, 0);
            SendMessage(g_hRangeCustomRadio, BM_SETCHECK, BST_UNCHECKED, 0);
            EnableWindow(g_hStartStatic, FALSE);
            EnableWindow(g_hStartEdit, FALSE);
            EnableWindow(g_hEndStatic, FALSE);
            EnableWindow(g_hEndEdit, FALSE);
        }
        else if (id == IDC_RANGE_CUSTOM_RADIO) {
            SendMessage(g_hRangeFullRadio, BM_SETCHECK, BST_UNCHECKED, 0);
            SendMessage(g_hRangeCustomRadio, BM_SETCHECK, BST_CHECKED, 0);
            EnableWindow(g_hStartStatic, TRUE);
            EnableWindow(g_hStartEdit, TRUE);
            EnableWindow(g_hEndStatic, TRUE);
            EnableWindow(g_hEndEdit, TRUE);
        }

        if (id == IDC_START_BUTTON) {
            wchar_t sizeBuf[32] = { 0 };
            GetWindowTextW(g_hSizeEdit, sizeBuf, ARRAYSIZE(sizeBuf));
            double targetSizeMB = _wtof(sizeBuf);
            if (targetSizeMB <= 0.0) {
                MessageBox(hwnd, L"Please enter a valid target size in MB.", L"Input Error", MB_ICONWARNING);
                break;
            }

            wchar_t suffixW[128] = { 0 };
            GetWindowTextW(g_hSuffixEdit, suffixW, ARRAYSIZE(suffixW));
            if (wcslen(suffixW) == 0) wcscpy_s(suffixW, L"RESIZED");
            char suffixA[256] = { 0 };
            WideCharToMultiByte(CP_ACP, 0, suffixW, -1, suffixA, sizeof(suffixA), nullptr, nullptr);

            double startSecs = 0.0, endSecs = g_duration;
            if (SendMessage(g_hRangeCustomRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                wchar_t startBuf[32], endBuf[32];
                GetWindowTextW(g_hStartEdit, startBuf, ARRAYSIZE(startBuf));
                GetWindowTextW(g_hEndEdit, endBuf, ARRAYSIZE(endBuf));
                startSecs = _wtof(startBuf);
                endSecs = _wtof(endBuf);
                if (startSecs < 0.0 || endSecs <= startSecs || endSecs > g_duration) {
                    MessageBox(hwnd, L"Please enter a valid start/end range within video duration.", L"Input Error", MB_ICONWARNING);
                    break;
                }
            }

            int scaleFactor = 1;
            if (SendMessage(g_hFullRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) scaleFactor = 1;
            else if (SendMessage(g_hHalfRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) scaleFactor = 2;
            else if (SendMessage(g_hQuarterRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) scaleFactor = 4;
            else {
                MessageBox(hwnd, L"Please select a scale option.", L"Input Error", MB_ICONWARNING);
                break;
            }

            char outPath[MAX_PATH] = { 0 };
            {
                char drive[_MAX_DRIVE], dir[_MAX_DIR], fname[_MAX_FNAME], ext[_MAX_EXT];
                _splitpath_s(g_inputPath, drive, _MAX_DRIVE, dir, _MAX_DIR, fname, _MAX_FNAME, ext, _MAX_EXT);
                char baseName[MAX_PATH];
                StringCchPrintfA(baseName, MAX_PATH, "%s_%s%s", fname, suffixA, ext);
                char candidate[MAX_PATH];
                _makepath_s(candidate, MAX_PATH, drive, dir, baseName, nullptr);
                if (_access(candidate, 0) == 0) {
                    for (int i = 1;; i++) {
                        char numbered[MAX_PATH];
                        StringCchPrintfA(numbered, MAX_PATH, "%s_%s-%d%s", fname, suffixA, i, ext);
                        _makepath_s(candidate, MAX_PATH, drive, dir, numbered, nullptr);
                        if (_access(candidate, 0) != 0) break;
                    }
                }
                StringCchCopyA(outPath, MAX_PATH, candidate);
            }

            EnableWindow(g_hStartButton, FALSE);
            bool ok = TranscodeWithSizeAndScale(g_inputPath, outPath, targetSizeMB, scaleFactor,
                g_vidWidth, g_vidHeight, startSecs, endSecs);
            if (ok) {
                std::string msg = "Successfully created:\n"; msg += outPath;
                MessageBoxA(hwnd, msg.c_str(), "Success", MB_ICONINFORMATION);
            }
            else {
                MessageBox(hwnd, L"Transcoding failed. See debug output for details.", L"Error", MB_ICONERROR);
            }
            EnableWindow(g_hStartButton, TRUE);
        }

        if (id == IDC_BTN_PLAYPAUSE && g_playerReady) {
            TogglePlayPause(hwnd);
        }
        else if (id == IDC_BTN_FWD && g_playerReady) {
            StepForward(hwnd);
        }
        else if (id == IDC_BTN_BACK && g_playerReady) {
            StepBackward(hwnd);
        }
        else if (id == IDC_BTN_MARKIN && g_playerReady) {
            SetMarkInFromCurrent(hwnd);
        }
        else if (id == IDC_BTN_MARKOUT && g_playerReady) {
            SetMarkOutFromCurrent(hwnd);
        }
        break;
    }

    case WM_APP_FRAME_READY: {
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    }

    case WM_TIMER: {
        if (wParam == IDT_UI_REFRESH) {
            UpdateSeekbarFromPos();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        if (g_hFrameBitmap) {
            RECT clientRect; GetClientRect(hwnd, &clientRect);

            RECT sbRect;
            GetWindowRect(g_hSeekbar, &sbRect);
            ScreenToClient(hwnd, (POINT*)&sbRect.left);
            ScreenToClient(hwnd, (POINT*)&sbRect.right);
            int margin = 10;
            int topY = sbRect.bottom + 30;

            int availW = clientRect.right - margin * 2;
            int availH = clientRect.bottom - topY - margin;
            if (availW > 0 && availH > 0) {
                double imgAR = (double)g_frameWidth / (double)g_frameHeight;
                int destW = availW;
                int destH = (int)(availW / imgAR);
                if (destH > availH) { destH = availH; destW = (int)(availH * imgAR); }
                int destX = (clientRect.right - destW) / 2;
                int destY = topY;

                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, g_hFrameBitmap);
                SetStretchBltMode(hdc, HALFTONE);
                SetBrushOrgEx(hdc, 0, 0, nullptr);
                StretchBlt(hdc, destX, destY, destW, destH, memDC, 0, 0, g_frameWidth, g_frameHeight, SRCCOPY);
                SelectObject(memDC, oldBmp);
                DeleteDC(memDC);
            }
        }

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DESTROY:
        StopPlayback();
        if (g_hFrameBitmap) { DeleteObject(g_hFrameBitmap); g_hFrameBitmap = nullptr; }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ------------------------------ Video Info ------------------------------
bool GetVideoInfo(const char* filepath, int& width, int& height, double& durationSeconds) {
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filepath, nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) { avformat_close_input(&fmt_ctx); return false; }

    int videoStreamIndex = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { videoStreamIndex = i; break; }
    }
    if (videoStreamIndex < 0) { avformat_close_input(&fmt_ctx); return false; }

    AVStream* videoStream = fmt_ctx->streams[videoStreamIndex];
    width = videoStream->codecpar->width;
    height = videoStream->codecpar->height;
    if (fmt_ctx->duration != AV_NOPTS_VALUE) durationSeconds = fmt_ctx->duration / (double)AV_TIME_BASE;
    else durationSeconds = 0.0;

    if (videoStream->avg_frame_rate.num && videoStream->avg_frame_rate.den) {
        g_videoFPS = av_q2d(videoStream->avg_frame_rate);
    }
    else if (videoStream->r_frame_rate.num && videoStream->r_frame_rate.den) {
        g_videoFPS = av_q2d(videoStream->r_frame_rate);
    }
    else g_videoFPS = 30.0;

    avformat_close_input(&fmt_ctx);
    return true;
}

// ------------------------------ Extract Middle Frame ------------------------------
HBITMAP ExtractMiddleFrameBitmap(const char* filepath, int orig_w, int orig_h, double duration) {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* dec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* rgbFrame = nullptr;
    int64_t middle_ts = 0;
    uint8_t* rgbBuffer = nullptr;
    HBITMAP hBitmap = nullptr;
    int videoStreamIndex = -1;
    AVStream* videoStream = nullptr;
    const AVCodec* decoder = nullptr;
    bool gotFrame = false;
    int rgbBufSize = 0;
    BITMAPINFO bmi = {};
    HDC hdc = nullptr;
    void* dibBits = nullptr;

    if (avformat_open_input(&fmt_ctx, filepath, nullptr, nullptr) < 0) goto cleanup;
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) goto cleanup;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { videoStreamIndex = i; break; }
    }
    if (videoStreamIndex < 0) goto cleanup;
    videoStream = fmt_ctx->streams[videoStreamIndex];

    decoder = avcodec_find_decoder(videoStream->codecpar->codec_id);
    if (!decoder) goto cleanup;
    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) goto cleanup;
    if (avcodec_parameters_to_context(dec_ctx, videoStream->codecpar) < 0) goto cleanup;
    if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) goto cleanup;

    middle_ts = (int64_t)((duration / 2.0) * AV_TIME_BASE);
    av_seek_frame(fmt_ctx, -1, middle_ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec_ctx);

    frame = av_frame_alloc();
    rgbFrame = av_frame_alloc();
    if (!frame || !rgbFrame) goto cleanup;
    pkt = av_packet_alloc();
    if (!pkt) goto cleanup;

    sws_ctx = sws_getContext(dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
        dec_ctx->width, dec_ctx->height, AV_PIX_FMT_BGR24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx) goto cleanup;
    rgbBufSize = av_image_get_buffer_size(AV_PIX_FMT_BGR24, dec_ctx->width, dec_ctx->height, 1);
    rgbBuffer = (uint8_t*)av_malloc(rgbBufSize);
    if (!rgbBuffer) goto cleanup;
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer,
        AV_PIX_FMT_BGR24, dec_ctx->width, dec_ctx->height, 1);

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(dec_ctx, pkt) < 0) { av_packet_unref(pkt); break; }
            if (avcodec_receive_frame(dec_ctx, frame) == 0) {
                sws_scale(sws_ctx, frame->data, frame->linesize, 0, dec_ctx->height,
                    rgbFrame->data, rgbFrame->linesize);
                gotFrame = true;
                av_frame_unref(frame);
                av_packet_unref(pkt);
                break;
            }
        }
        av_packet_unref(pkt);
    }
    if (!gotFrame) goto cleanup;

    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = dec_ctx->width;
    bmi.bmiHeader.biHeight = -dec_ctx->height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    hdc = GetDC(nullptr);
    hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hBitmap) goto cleanup;

    {
        int rowBytes = dec_ctx->width * 3;
        for (int y = 0; y < dec_ctx->height; y++) {
            memcpy((uint8_t*)dibBits + y * rowBytes,
                rgbFrame->data[0] + y * rgbFrame->linesize[0], rowBytes);
        }
    }

cleanup:
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (frame) av_frame_free(&frame);
    if (rgbFrame) { if (rgbBuffer) av_free(rgbBuffer); av_frame_free(&rgbFrame); }
    if (pkt) av_packet_free(&pkt);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);
    return hBitmap;
}

// ------------------------------ Playback Thread (no goto) ------------------------------
struct PlaybackCtx { std::string path; };

unsigned __stdcall PlaybackThreadProc(void* p) {
    PlaybackCtx* ctx = (PlaybackCtx*)p;
    const char* filepath = ctx->path.c_str();

    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* dec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* rgbFrame = nullptr;
    int              videoStreamIndex = -1;
    AVStream* videoStream = nullptr;
    const AVCodec* decoder = nullptr;
    uint8_t* rgbBuffer = nullptr;
    int              rgbBufSize = 0;

    bool init_ok = false;
    do {
        if (avformat_open_input(&fmt_ctx, filepath, nullptr, nullptr) < 0) break;
        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) break;
        for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { videoStreamIndex = i; break; }
        }
        if (videoStreamIndex < 0) break;
        videoStream = fmt_ctx->streams[videoStreamIndex];

        decoder = avcodec_find_decoder(videoStream->codecpar->codec_id);
        if (!decoder) break;
        dec_ctx = avcodec_alloc_context3(decoder);
        if (!dec_ctx) break;
        if (avcodec_parameters_to_context(dec_ctx, videoStream->codecpar) < 0) break;
        if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) break;

        sws_ctx = sws_getContext(dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
            dec_ctx->width, dec_ctx->height, AV_PIX_FMT_BGR24,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx) break;

        frame = av_frame_alloc();
        rgbFrame = av_frame_alloc();
        if (!frame || !rgbFrame) break;

        rgbBufSize = av_image_get_buffer_size(AV_PIX_FMT_BGR24, dec_ctx->width, dec_ctx->height, 1);
        rgbBuffer = (uint8_t*)av_malloc(rgbBufSize);
        if (!rgbBuffer) break;
        av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer,
            AV_PIX_FMT_BGR24, dec_ctx->width, dec_ctx->height, 1);

        pkt = av_packet_alloc();
        if (!pkt) break;

        init_ok = true;
    } while (false);

    if (!init_ok) {
        // cleanup and exit
        if (pkt) av_packet_free(&pkt);
        if (rgbFrame) { if (rgbBuffer) av_free(rgbBuffer); av_frame_free(&rgbFrame); }
        if (frame) av_frame_free(&frame);
        if (sws_ctx) sws_freeContext(sws_ctx);
        if (dec_ctx) avcodec_free_context(&dec_ctx);
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
        delete ctx;
        _endthreadex(0);
        return 0;
    }

    auto doSeek = [&](int64_t toMs) {
        int64_t ts = av_rescale_q(toMs, AVRational{ 1,1000 }, videoStream->time_base);
        av_seek_frame(fmt_ctx, videoStreamIndex, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec_ctx);
        };

    if (g_seekRequested) { doSeek(g_seekTargetMs); g_seekRequested = false; }
    else { doSeek(g_currentPosMs); }

    while (!g_playThreadShouldExit) {
        if (g_seekRequested) {
            int64_t target = g_seekTargetMs;
            g_seekRequested = false;
            doSeek(target);
        }

        // Paused single-frame preview with proper step logic
        if (g_decodeSingleFrame && !g_isPlaying) {
            bool produced = false;
            int64_t targetMs = g_seekTargetMs;
            int64_t bestMs = -1;
            HBITMAP bestBmp = nullptr;

            while (av_read_frame(fmt_ctx, pkt) >= 0) {
                if (pkt->stream_index != videoStreamIndex) { av_packet_unref(pkt); continue; }
                if (avcodec_send_packet(dec_ctx, pkt) < 0) { av_packet_unref(pkt); break; }

                while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, dec_ctx->height,
                        rgbFrame->data, rgbFrame->linesize);

                    // Create DIB
                    BITMAPINFO bmi = {};
                    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    bmi.bmiHeader.biWidth = dec_ctx->width;
                    bmi.bmiHeader.biHeight = -dec_ctx->height;
                    bmi.bmiHeader.biPlanes = 1;
                    bmi.bmiHeader.biBitCount = 24;
                    bmi.bmiHeader.biCompression = BI_RGB;

                    HDC hdc = GetDC(nullptr);
                    void* dibBits = nullptr;
                    HBITMAP hNew = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
                    ReleaseDC(nullptr, hdc);

                    if (hNew) {
                        int rowBytes = dec_ctx->width * 3;
                        for (int y = 0; y < dec_ctx->height; y++) {
                            memcpy((uint8_t*)dibBits + y * rowBytes,
                                rgbFrame->data[0] + y * rgbFrame->linesize[0], rowBytes);
                        }
                    }

                    int64_t ms = (int64_t)(frame->best_effort_timestamp *
                        av_q2d(videoStream->time_base) * 1000.0);

                    if (g_stepDir >= 0) {
                        // Forward step: stop at first frame >= target
                        if (ms >= targetMs) {
                            EnterCriticalSection(&g_csState);
                            if (g_hFrameBitmap) DeleteObject(g_hFrameBitmap);
                            g_hFrameBitmap = hNew; hNew = nullptr;
                            g_frameWidth = dec_ctx->width;
                            g_frameHeight = dec_ctx->height;
                            g_currentPosMs = ms;
                            LeaveCriticalSection(&g_csState);
                            produced = true;
                        }
                    }
                    else {
                        // Backward step: remember last frame strictly before target
                        if (ms < targetMs) {
                            if (bestBmp) DeleteObject(bestBmp);
                            bestBmp = hNew; hNew = nullptr;
                            bestMs = ms;
                        }
                        else {
                            // Crossed target, present best previous if exists
                            if (bestBmp) {
                                EnterCriticalSection(&g_csState);
                                if (g_hFrameBitmap) DeleteObject(g_hFrameBitmap);
                                g_hFrameBitmap = bestBmp; bestBmp = nullptr;
                                g_frameWidth = dec_ctx->width;
                                g_frameHeight = dec_ctx->height;
                                g_currentPosMs = (bestMs >= 0) ? bestMs : ms;
                                LeaveCriticalSection(&g_csState);
                                produced = true;
                            }
                            else {
                                EnterCriticalSection(&g_csState);
                                if (g_hFrameBitmap) DeleteObject(g_hFrameBitmap);
                                g_hFrameBitmap = hNew; hNew = nullptr;
                                g_frameWidth = dec_ctx->width;
                                g_frameHeight = dec_ctx->height;
                                g_currentPosMs = ms;
                                LeaveCriticalSection(&g_csState);
                                produced = true;
                            }
                        }
                    }

                    if (hNew) DeleteObject(hNew);
                    av_frame_unref(frame);
                    if (produced) break;
                }
                av_packet_unref(pkt);
                if (produced) break;
            }

            g_decodeSingleFrame = false;
            g_stepDir = 0;
            if (produced) {
                PostMessage(g_mainHwnd, WM_APP_FRAME_READY, 0, 0);
            }
            Sleep(5);
            continue;
        }

        if (!g_isPlaying) { Sleep(10); continue; }

        if (av_read_frame(fmt_ctx, pkt) < 0) {
            g_isPlaying = false;
            continue;
        }
        if (pkt->stream_index != videoStreamIndex) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(dec_ctx, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        while (avcodec_receive_frame(dec_ctx, frame) == 0) {
            sws_scale(sws_ctx, frame->data, frame->linesize, 0, dec_ctx->height,
                rgbFrame->data, rgbFrame->linesize);

            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = dec_ctx->width;
            bmi.bmiHeader.biHeight = -dec_ctx->height;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 24;
            bmi.bmiHeader.biCompression = BI_RGB;

            HDC hdc = GetDC(nullptr);
            void* dibBits = nullptr;
            HBITMAP hNew = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
            ReleaseDC(nullptr, hdc);
            if (hNew) {
                int rowBytes = dec_ctx->width * 3;
                for (int y = 0; y < dec_ctx->height; y++) {
                    memcpy((uint8_t*)dibBits + y * rowBytes,
                        rgbFrame->data[0] + y * rgbFrame->linesize[0], rowBytes);
                }
                EnterCriticalSection(&g_csState);
                if (g_hFrameBitmap) DeleteObject(g_hFrameBitmap);
                g_hFrameBitmap = hNew;
                g_frameWidth = dec_ctx->width;
                g_frameHeight = dec_ctx->height;
                int64_t ms = (int64_t)(frame->best_effort_timestamp *
                    av_q2d(videoStream->time_base) * 1000.0);
                g_currentPosMs = ms;
                LeaveCriticalSection(&g_csState);
            }
            PostMessage(g_mainHwnd, WM_APP_FRAME_READY, 0, 0);
            av_frame_unref(frame);
        }
        av_packet_unref(pkt);

        Sleep((DWORD)max(1.0, 1000.0 / g_videoFPS));
    }

    // cleanup
    if (pkt) av_packet_free(&pkt);
    if (rgbFrame) { if (rgbBuffer) av_free(rgbBuffer); av_frame_free(&rgbFrame); }
    if (frame) av_frame_free(&frame);
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);

    delete ctx;
    _endthreadex(0);
    return 0;
}

void StartPlayback(HWND hwnd) {
    EnterCriticalSection(&g_csState);
    if (g_hFrameBitmap) { DeleteObject(g_hFrameBitmap); g_hFrameBitmap = nullptr; }
    LeaveCriticalSection(&g_csState);

    if (g_hPlaybackThread) return;
    g_playThreadShouldExit = false;
    g_isPlaying = true;

    PlaybackCtx* ctx = new PlaybackCtx();
    ctx->path = g_inputPath;
    uintptr_t th = _beginthreadex(nullptr, 0, PlaybackThreadProc, ctx, 0, nullptr);
    g_hPlaybackThread = (HANDLE)th;

    SetTimer(hwnd, IDT_UI_REFRESH, 33, nullptr);
}

void EnsureThreadRunningPaused(HWND hwnd) {
    if (!g_hPlaybackThread) {
        g_playThreadShouldExit = false;
        g_isPlaying = false;
        PlaybackCtx* ctx = new PlaybackCtx();
        ctx->path = g_inputPath;
        uintptr_t th = _beginthreadex(nullptr, 0, PlaybackThreadProc, ctx, 0, nullptr);
        g_hPlaybackThread = (HANDLE)th;
        SetTimer(hwnd, IDT_UI_REFRESH, 33, nullptr);
    }
}

void StopPlayback() {
    g_isPlaying = false;
    g_playThreadShouldExit = true;
    if (g_hPlaybackThread) {
        WaitForSingleObject(g_hPlaybackThread, 5000);
        CloseHandle(g_hPlaybackThread);
        g_hPlaybackThread = nullptr;
    }
}

void TogglePlayPause(HWND hwnd) {
    if (!g_hPlaybackThread) {
        StartPlayback(hwnd);
        SetWindowTextW(g_hBtnPlayPause, L"Pause");
        return;
    }
    g_isPlaying = !g_isPlaying;
    SetWindowTextW(g_hBtnPlayPause, g_isPlaying ? L"Pause" : L"Play");
}

void SeekMs(int64_t ms, bool decodeSingle) {
    if (ms < 0) ms = 0;
    int64_t durMs = (int64_t)(g_duration * 1000.0);
    if (ms > durMs) ms = durMs;

    g_seekTargetMs = ms;
    g_seekRequested = true;
    if (decodeSingle) g_decodeSingleFrame = true;
}


void StepForward(HWND hwnd) {
    EnsureThreadRunningPaused(hwnd);
    g_isPlaying = false;
    int frameMs = (int)max(1.0, 1000.0 / g_videoFPS);
    g_stepDir = +1;
    SeekMs(g_currentPosMs + frameMs, true);
}


void StepBackward(HWND hwnd) {
    EnsureThreadRunningPaused(hwnd);
    g_isPlaying = false;
    int frameMs = (int)max(1.0, 1000.0 / g_videoFPS);
    g_stepDir = -1;
    // Seek slightly earlier than one frame to ensure we land before target and then walk forward
    int64_t target = (g_currentPosMs > frameMs * 2) ? (g_currentPosMs - frameMs * 2) : 0;
    SeekMs(target, true);
}


void UpdateSeekbarFromPos() {
    if (!g_playerReady) return;
    int pos = (int)g_currentPosMs;
    SendMessage(g_hSeekbar, TBM_SETPOS, TRUE, pos);
}

void SetMarkInFromCurrent(HWND hwnd) {
    SendMessage(g_hRangeFullRadio, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessage(g_hRangeCustomRadio, BM_SETCHECK, BST_CHECKED, 0);
    EnableWindow(g_hStartStatic, TRUE);
    EnableWindow(g_hStartEdit, TRUE);
    EnableWindow(g_hEndStatic, TRUE);
    EnableWindow(g_hEndEdit, TRUE);

    double secs = g_currentPosMs / 1000.0;
    wchar_t buf[32]; StringCchPrintfW(buf, 32, L"%.3f", secs);
    SetWindowTextW(g_hStartEdit, buf);
}

void SetMarkOutFromCurrent(HWND hwnd) {
    SendMessage(g_hRangeFullRadio, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessage(g_hRangeCustomRadio, BM_SETCHECK, BST_CHECKED, 0);
    EnableWindow(g_hStartStatic, TRUE);
    EnableWindow(g_hStartEdit, TRUE);
    EnableWindow(g_hEndStatic, TRUE);
    EnableWindow(g_hEndEdit, TRUE);

    double secs = g_currentPosMs / 1000.0;
    wchar_t buf[32]; StringCchPrintfW(buf, 32, L"%.3f", secs);
    SetWindowTextW(g_hEndEdit, buf);
}

// ------------------------------ Transcode (unchanged) ------------------------------
bool TranscodeWithSizeAndScale(const char* in_filename, const char* out_filename, double target_size_mb,
    int scale_factor, int orig_w, int orig_h, double start_seconds, double end_seconds) {
    int64_t           target_bitrate;
    AVFormatContext* in_fmt_ctx = nullptr;
    AVFormatContext* out_fmt_ctx = nullptr;
    AVCodecContext* dec_ctx = nullptr;
    AVCodecContext* enc_ctx = nullptr;
    AVStream* video_in_stream = nullptr;
    AVStream* video_out_stream = nullptr;
    AVStream* audio_in_stream = nullptr;
    AVStream* audio_out_stream = nullptr;
    const AVCodec* video_decoder = nullptr;
    const AVCodec* video_encoder = nullptr;
    SwsContext* sws_ctx = nullptr;
    int               videoStreamIndex = -1;
    int               audioStreamIndex = -1;
    AVFrame* frame = nullptr;
    AVFrame* filt_frame = nullptr;
    AVPacket* pkt = nullptr;
    AVPacket* enc_pkt = nullptr;
    bool              success = false;
    int64_t           video_start_pts = 0;
    int64_t           audio_start_pts = 0;

    if (end_seconds <= start_seconds) { OutputDebugStringA("End time must be greater than start time.\n"); return false; }
    double segment_duration = end_seconds - start_seconds;
    if (segment_duration <= 0.0) { OutputDebugStringA("Invalid segment duration.\n"); return false; }
    {
        int64_t total_bits = (int64_t)(target_size_mb * 8.0 * 1024.0 * 1024.0);
        int64_t video_bits = (int64_t)(total_bits * 0.95);
        target_bitrate = video_bits / (int64_t)segment_duration;
    }
    if (target_bitrate <= 0) { OutputDebugStringA("Invalid target bitrate calculated.\n"); return false; }

    if (avformat_open_input(&in_fmt_ctx, in_filename, nullptr, nullptr) < 0) { OutputDebugStringA("Could not open input file.\n"); goto cleanup; }
    if (avformat_find_stream_info(in_fmt_ctx, nullptr) < 0) { OutputDebugStringA("Could not find stream info.\n"); goto cleanup; }
    for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; i++) {
        AVStream* st = in_fmt_ctx->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStreamIndex < 0) { videoStreamIndex = i; }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStreamIndex < 0) { audioStreamIndex = i; }
    }
    if (videoStreamIndex < 0) { OutputDebugStringA("No video stream found.\n"); goto cleanup; }
    video_in_stream = in_fmt_ctx->streams[videoStreamIndex];
    if (audioStreamIndex >= 0) audio_in_stream = in_fmt_ctx->streams[audioStreamIndex];

    video_decoder = avcodec_find_decoder(video_in_stream->codecpar->codec_id);
    if (!video_decoder) { OutputDebugStringA("Video decoder not found.\n"); goto cleanup; }
    dec_ctx = avcodec_alloc_context3(video_decoder);
    if (!dec_ctx) { OutputDebugStringA("Failed to allocate video decoder context.\n"); goto cleanup; }
    if (avcodec_parameters_to_context(dec_ctx, video_in_stream->codecpar) < 0) { OutputDebugStringA("Failed to copy video params to decoder.\n"); goto cleanup; }
    if (avcodec_open2(dec_ctx, video_decoder, nullptr) < 0) { OutputDebugStringA("Failed to open video decoder.\n"); goto cleanup; }

    avformat_alloc_output_context2(&out_fmt_ctx, nullptr, nullptr, out_filename);
    if (!out_fmt_ctx) { OutputDebugStringA("Could not create output format context.\n"); goto cleanup; }

    video_encoder = avcodec_find_encoder_by_name("h264_nvenc");
    if (!video_encoder) { video_encoder = avcodec_find_encoder(AV_CODEC_ID_H264); if (!video_encoder) { OutputDebugStringA("H.264 encoder not found.\n"); goto cleanup; } }

    video_out_stream = avformat_new_stream(out_fmt_ctx, video_encoder);
    if (!video_out_stream) { OutputDebugStringA("Could not create video output stream.\n"); goto cleanup; }
    enc_ctx = avcodec_alloc_context3(video_encoder);
    if (!enc_ctx) { OutputDebugStringA("Failed to allocate video encoder context.\n"); goto cleanup; }
    enc_ctx->height = orig_h / scale_factor;
    enc_ctx->width = orig_w / scale_factor;
    enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
    if (strcmp(video_encoder->name, "h264_nvenc") == 0) enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    else { enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P; av_opt_set(enc_ctx->priv_data, "preset", "medium", 0); av_opt_set(enc_ctx->priv_data, "nal-hrd", "cbr", 0); }
    enc_ctx->time_base = av_inv_q(dec_ctx->framerate);
    enc_ctx->bit_rate = target_bitrate;
    enc_ctx->rc_max_rate = target_bitrate;
    enc_ctx->rc_buffer_size = target_bitrate;
    if (out_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(enc_ctx, video_encoder, nullptr) < 0) { OutputDebugStringA("Could not open video encoder.\n"); goto cleanup; }
    if (avcodec_parameters_from_context(video_out_stream->codecpar, enc_ctx) < 0) { OutputDebugStringA("Failed to copy encoder params to output.\n"); goto cleanup; }
    video_out_stream->time_base = enc_ctx->time_base;

    if (audio_in_stream) {
        audio_out_stream = avformat_new_stream(out_fmt_ctx, nullptr);
        if (!audio_out_stream) { OutputDebugStringA("Could not create audio output stream. Disabling audio.\n"); audio_in_stream = nullptr; }
        else {
            if (avcodec_parameters_copy(audio_out_stream->codecpar, audio_in_stream->codecpar) < 0) { OutputDebugStringA("Failed to copy audio params. Disabling audio.\n"); audio_in_stream = nullptr; audio_out_stream = nullptr; }
            else audio_out_stream->time_base = audio_in_stream->time_base;
        }
    }

    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt_ctx->pb, out_filename, AVIO_FLAG_WRITE) < 0) { OutputDebugStringA("Could not open output file.\n"); goto cleanup; }
    }
    if (avformat_write_header(out_fmt_ctx, nullptr) < 0) { OutputDebugStringA("Error writing header to output.\n"); goto cleanup; }

    {
        AVRational video_tb = video_in_stream->time_base;
        // Convert start_seconds (in AV_TIME_BASE) to the video stream's time_base.
        int64_t start_av = (int64_t)llround(start_seconds * AV_TIME_BASE);
        video_start_pts = av_rescale_q(start_av, AV_TIME_BASE_Q, video_tb);

        // Seek the demuxer to (or just before) the requested start on the video stream.
        if (av_seek_frame(in_fmt_ctx, videoStreamIndex, video_start_pts, AVSEEK_FLAG_BACKWARD) < 0) {
            OutputDebugStringA("Warning: could not seek exactly to start time (video).\n");
        }

        // Prepare audio timeline anchor too (if present).
        if (audio_in_stream) {
            AVRational audio_tb = audio_in_stream->time_base;
            audio_start_pts = av_rescale_q(start_av, AV_TIME_BASE_Q, audio_tb);
        }
    }
    avcodec_flush_buffers(dec_ctx);


    frame = av_frame_alloc();
    filt_frame = av_frame_alloc();
    pkt = av_packet_alloc();
    enc_pkt = av_packet_alloc();
    if (!frame || !filt_frame || !pkt || !enc_pkt) { OutputDebugStringA("Could not allocate frame/packet.\n"); goto cleanup; }
    sws_ctx = sws_getContext(dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
        enc_ctx->width, enc_ctx->height, enc_ctx->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx) { OutputDebugStringA("Could not initialize SwsContext.\n"); goto cleanup; }
    filt_frame->format = enc_ctx->pix_fmt;
    filt_frame->width = enc_ctx->width;
    filt_frame->height = enc_ctx->height;
    if (av_frame_get_buffer(filt_frame, 32) < 0) { OutputDebugStringA("Could not allocate buffer for scaled frame.\n"); goto cleanup; }

    while (av_read_frame(in_fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(dec_ctx, pkt) < 0) { av_packet_unref(pkt); break; }
            while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                int64_t in_pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE) ? frame->best_effort_timestamp : frame->pts;
                double in_time = in_pts * av_q2d(video_in_stream->time_base);
                if (in_time > end_seconds) { av_frame_unref(frame); goto flush_encoder; }

                // Drop frames that still decode before the requested start
                if (in_time < start_seconds) { av_frame_unref(frame); continue; }
                if (in_time > end_seconds) { av_frame_unref(frame); goto flush_encoder; }

                // Rebase video PTS to 0 at start_seconds, then convert to encoder time_base
                int64_t rel_vid_pts = in_pts - video_start_pts;
                if (rel_vid_pts < 0) rel_vid_pts = 0;
                filt_frame->pts = av_rescale_q(rel_vid_pts, video_in_stream->time_base, enc_ctx->time_base);

                sws_scale(sws_ctx, frame->data, frame->linesize, 0, dec_ctx->height, filt_frame->data, filt_frame->linesize);
                if (avcodec_send_frame(enc_ctx, filt_frame) < 0) { OutputDebugStringA("Error sending frame to video encoder.\n"); break; }
                while (avcodec_receive_packet(enc_ctx, enc_pkt) == 0) {
                    enc_pkt->stream_index = video_out_stream->index;
                    av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, video_out_stream->time_base);
                    av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
                    av_packet_unref(enc_pkt);
                }
                av_frame_unref(frame);
            }
        }

        else if (audio_in_stream && pkt->stream_index == audioStreamIndex) {
            AVRational in_tb = audio_in_stream->time_base;
            AVRational out_tb = audio_out_stream->time_base;
            // Normalize missing PTS/DTS
            if (pkt->pts == AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE) pkt->pts = pkt->dts;
            if (pkt->dts == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE) pkt->dts = pkt->pts;

            int64_t aud_in_pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
            double aud_time = aud_in_pts * av_q2d(in_tb);
            if (aud_time < start_seconds || aud_time > end_seconds) { av_packet_unref(pkt); continue; }

            // Shift audio PTS/DTS so segment starts at t=0 in the audio TB
            if (pkt->pts != AV_NOPTS_VALUE) { pkt->pts -= audio_start_pts; if (pkt->pts < 0) pkt->pts = 0; }
            if (pkt->dts != AV_NOPTS_VALUE) { pkt->dts -= audio_start_pts; if (pkt->dts < 0) pkt->dts = 0; }

            // Rescale into output TB and write
            av_packet_rescale_ts(pkt, in_tb, out_tb);
            pkt->stream_index = audio_out_stream->index;
            pkt->pos = -1;
            av_interleaved_write_frame(out_fmt_ctx, pkt);
        }

        av_packet_unref(pkt);
    }

flush_encoder:
    avcodec_send_frame(enc_ctx, nullptr);
    while (avcodec_receive_packet(enc_ctx, enc_pkt) == 0) {
        enc_pkt->stream_index = video_out_stream->index;
        av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, video_out_stream->time_base);
        av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
        av_packet_unref(enc_pkt);
    }
    av_packet_unref(enc_pkt);

    av_write_trailer(out_fmt_ctx);
    success = true;

cleanup:
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (frame) av_frame_free(&frame);
    if (filt_frame) av_frame_free(&filt_frame);
    if (pkt) av_packet_free(&pkt);
    if (enc_pkt) av_packet_free(&enc_pkt);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (enc_ctx) avcodec_free_context(&enc_ctx);
    if (in_fmt_ctx) avformat_close_input(&in_fmt_ctx);
    if (out_fmt_ctx) {
        if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_fmt_ctx->pb);
        avformat_free_context(out_fmt_ctx);
    }
    return success;
}
