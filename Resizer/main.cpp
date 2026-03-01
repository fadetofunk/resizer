// main.cpp
// Win32 C++ app with FFmpeg: drag & drop a video, show middle-frame screenshot.
// Adds a simple video player with controls and correct paused-step preview.
// Playback thread no longer uses goto; avoids MSVC E0546.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <strsafe.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "ole32.lib")
#include <windowsx.h>
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
#include <libavutil/dict.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

// ------------------------------ Resource IDs ------------------------------
#define IDI_APPICON                  101

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

#define WM_APP_THUMBS_READY  (WM_APP + 3)

#define IDC_GRP_SETTINGS          2010
#define IDC_GRP_RANGE             2011
#define IDC_GRP_RESOLUTION        2012
#define IDC_GRP_PLAYER            2013
#define IDC_RES_DROPDOWN          2014
#define IDC_RES_STATIC            2015
#define IDC_AUDIO_STATIC          2016
#define IDC_AUDIO_DROPDOWN        2017
#define IDC_SUBS_STATIC           2018
#define IDC_SUBS_DROPDOWN         2019
#define IDC_GRP_MEDIA             2020
#define IDC_GRP_SAVELOC           2021
#define IDC_SAVE_SAME_RADIO       2022
#define IDC_SAVE_CUSTOM_RADIO     2023
#define IDC_SAVE_PATH_EDIT        2024
#define IDC_SAVE_BROWSE_BTN       2025
#define IDC_GRP_COLOR             2026
#define IDC_COLOR_DROP            2027

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

static HWND     g_hTimeline = nullptr;   // custom filmstrip seekbar
static int      g_tlPos     = 0;         // current position in ms
static int      g_tlMax     = 1000;      // duration in ms
static bool     g_tlEnabled = false;

static HBITMAP  g_thumbs[21] = {};       // filmstrip thumbnails (0%..100% in 5% steps)
static int      g_thumbW = 0, g_thumbH = 0;
static HANDLE   g_thumbThread   = nullptr;
static volatile bool g_thumbThreadStop = false;
static HWND     g_hBtnPlayPause = nullptr;
static HWND     g_hBtnFwd = nullptr;
static HWND     g_hBtnBack = nullptr;
static HWND     g_hBtnMarkIn = nullptr;
static HWND     g_hBtnMarkOut = nullptr;

static HWND     g_hGrpSettings   = nullptr;
static HWND     g_hGrpRange      = nullptr;
static HWND     g_hGrpResolution = nullptr;
static HWND     g_hGrpPlayer     = nullptr;
static HFONT    g_hFont          = nullptr;
static HFONT    g_hLabelFont     = nullptr;  // semibold variant for labels/radios
static HWND     g_hResDrop       = nullptr;  // owner-draw resolution dropdown button
static int      g_resSelection   = 0;        // 0=Full, 1=Half, 2=Quarter
static HWND     g_hResStatic     = nullptr;  // "Resolution:" label
static HWND     g_hGrpMedia      = nullptr;  // Audio & Subtitles group box
static HWND     g_hAudioStatic   = nullptr;  // "Audio track:" label
static HWND     g_hAudioDrop     = nullptr;  // audio track combo box
static HWND     g_hSubsStatic    = nullptr;  // "Burn in Subtitles:" label
static HWND     g_hSubsDrop      = nullptr;  // subtitles combo box
static ULONG_PTR g_gdiplusToken  = 0;
static HWND      g_hTooltip      = nullptr;

// Save Location
static HWND     g_hGrpSaveLoc      = nullptr;
static HWND     g_hSaveSameRadio   = nullptr;
static HWND     g_hSaveCustomRadio = nullptr;
static HWND     g_hSavePathEdit    = nullptr;
static HWND     g_hSaveBrowseBtn   = nullptr;
static wchar_t  g_saveFolder[MAX_PATH] = { 0 };
static bool     g_saveCustom       = false;

// Color Space / HDR
static HWND     g_hGrpColor        = nullptr;
static HWND     g_hColorDrop       = nullptr;
static bool     g_isHdr            = false;
static int      g_hdrTrc           = 0;   // AVColorTransferCharacteristic of source
static uint8_t  g_hdrDisplay8Lut[256] = {};
static bool     g_hdrLutValid      = false;

// ------------------------------ Theme ------------------------------
struct AppTheme {
    COLORREF bk;           // window / panel background
    COLORREF editBk;       // edit control background
    COLORREF text;         // primary text (labels, group captions)
    COLORREF editText;     // edit control text
    COLORREF pillNormal;   // owner-draw button pill — normal
    COLORREF pillPressed;  // owner-draw button pill — pressed
    COLORREF pillDisabled; // owner-draw button pill — disabled
    COLORREF pillBorder;   // pill border (as COLORREF; alpha applied in drawing)
};
static bool     g_darkMode  = false;
static AppTheme g_theme     = {};
static HBRUSH   g_hBkBrush  = nullptr;   // window background brush
static HBRUSH   g_hEditBrush = nullptr;  // edit control background brush

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
static bool         g_isDragging   = false; // true while user drags the seekbar thumb
static bool         g_isGenerating = false; // true while a seek-frame decode is in flight

// Forward declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK TimelineWndProc(HWND, UINT, WPARAM, LPARAM);
static bool IsSystemDarkMode();
static void ApplyTheme(HWND hwnd);
void HandleResize(HWND hwnd, int clientW, int clientH);
static HBITMAP ExtractFrameAtTime(const char* filepath, double timeSecs);
static unsigned __stdcall ThumbExtractThreadProc(void* param);
bool GetVideoInfo(const char* filepath, int& width, int& height, double& durationSeconds);
HBITMAP ExtractMiddleFrameBitmap(const char* filepath, int orig_w, int orig_h, double duration);
bool TranscodeWithSizeAndScale(const char* in_filename, const char* out_filename, double target_size_mb,
    int scale_factor, int orig_w, int orig_h, double start_seconds, double end_seconds,
    int audio_stream_index = -1, int subtitle_stream_index = -1, bool convert_hdr_to_sdr = false);

// Audio/subtitle track enumeration
static void PopulateAudioAndSubsDropdowns(const char* filepath);
static void DetectHdr(const char* filepath);

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
    const int M      = 10;  // outer margin
    const int P      = 8;   // inner padding inside group boxes
    const int GH     = 22;  // group box header height
    const int RH     = 22;  // standard row height
    const int totalW = clientW - M * 2;
    const int colGap = 6;
    const int lColW  = (totalW - colGap) * 3 / 5;   // ~60% — left column groups
    const int rColW  = totalW - colGap - lColW;       // ~40% — right column groups

    int y = M;

    // Info bar – always at top, full width
    MoveWindow(g_hInfoStatic, M, y, totalW, RH, TRUE);
    y += RH + M;

    // === Output Settings (left) + Save Location (right) ===
    int settingsH = GH + P + RH + 5 + RH + 5 + RH + P;
    MoveWindow(g_hGrpSettings, M,                  y, lColW, settingsH, TRUE);
    MoveWindow(g_hGrpSaveLoc,  M + lColW + colGap, y, rColW, settingsH, TRUE);
    {
        int ix = M + P, iy = y + GH;
        int lw = 130;
        int ew = max(40, lColW - P * 2 - lw - 6);
        MoveWindow(g_hSizeStatic,   ix,          iy, lw, RH, TRUE);
        MoveWindow(g_hSizeEdit,     ix + lw + 6, iy, ew, RH, TRUE);
        iy += RH + 5;
        MoveWindow(g_hSuffixStatic, ix,          iy, lw, RH, TRUE);
        MoveWindow(g_hSuffixEdit,   ix + lw + 6, iy, ew, RH, TRUE);
        iy += RH + 5;
        MoveWindow(g_hResStatic,    ix,          iy, lw, RH, TRUE);
        MoveWindow(g_hResDrop,      ix + lw + 6, iy, ew, RH, TRUE);
    }
    {
        int sx     = M + lColW + colGap + P;
        int iy     = y + GH;
        int innerW = rColW - P * 2;
        MoveWindow(g_hSaveSameRadio,   sx, iy, innerW, RH, TRUE);
        iy += RH + 5;
        MoveWindow(g_hSaveCustomRadio, sx, iy, innerW, RH, TRUE);
        iy += RH + 5;
        int browseW = 70;
        int editW   = max(40, innerW - browseW - 4);
        MoveWindow(g_hSavePathEdit,  sx,             iy, editW,   RH, TRUE);
        MoveWindow(g_hSaveBrowseBtn, sx + editW + 4, iy, browseW, RH, TRUE);
    }
    y += settingsH + M;

    // === Range (left) + Color Space (right) ===
    int rangeH = GH + P + RH + P;
    MoveWindow(g_hGrpRange, M,                  y, lColW, rangeH, TRUE);
    MoveWindow(g_hGrpColor, M + lColW + colGap, y, rColW, rangeH, TRUE);
    {
        int ix      = M + P, iy = y + GH;
        int rightIx = M + lColW - P;
        const int radioW1 = 105, radioW2 = 120, gapR = 10;
        MoveWindow(g_hRangeFullRadio,   ix,                  iy, radioW1, RH, TRUE);
        MoveWindow(g_hRangeCustomRadio, ix + radioW1 + gapR, iy, radioW2, RH, TRUE);

        int midLeft = ix + radioW1 + gapR + radioW2 + 12;
        int midW    = rightIx - midLeft;
        const int labelW = 90;
        int editW = max(40, (midW - labelW * 2 - 8) / 2);
        MoveWindow(g_hStartStatic, midLeft,                          iy, labelW, RH, TRUE);
        MoveWindow(g_hStartEdit,   midLeft + labelW,                 iy, editW,  RH, TRUE);
        MoveWindow(g_hEndStatic,   midLeft + labelW + editW + 8,     iy, labelW, RH, TRUE);
        MoveWindow(g_hEndEdit,     midLeft + labelW * 2 + editW + 8, iy, editW,  RH, TRUE);
    }
    {
        int cx     = M + lColW + colGap + P;
        int iy     = y + GH;
        int innerW = rColW - P * 2;
        MoveWindow(g_hColorDrop, cx, iy, innerW, 120, TRUE);
    }
    y += rangeH + M;

    // === Audio & Subtitles (2 rows) ===
    int mediaH = GH + P + RH + 5 + RH + P;
    MoveWindow(g_hGrpMedia, M, y, totalW, mediaH, TRUE);
    {
        int ix = M + P, iy = y + GH;
        int lw = 130;
        int ew = max(50, totalW - P * 2 - lw - 6);
        MoveWindow(g_hAudioStatic, ix,          iy, lw,  RH,  TRUE);
        MoveWindow(g_hAudioDrop,   ix + lw + 6, iy, ew,  120, TRUE);
        iy += RH + 5;
        MoveWindow(g_hSubsStatic,  ix,          iy, lw,  RH,  TRUE);
        MoveWindow(g_hSubsDrop,    ix + lw + 6, iy, ew,  120, TRUE);
    }
    y += mediaH + M;

    // === Player ===
    const int btnW = 90, btnH = 28, seekH = 72;
    int playerH = GH + P + btnH + 6 + seekH + P;
    MoveWindow(g_hGrpPlayer, M, y, totalW, playerH, TRUE);
    {
        int ix    = M + P, iy = y + GH;
        int avail = totalW - P * 2;
        MoveWindow(g_hBtnPlayPause, ix,                       iy, btnW, btnH, TRUE);
        MoveWindow(g_hBtnBack,      ix + btnW + 6,            iy, btnW, btnH, TRUE);
        MoveWindow(g_hBtnFwd,       ix + (btnW + 6) * 2,      iy, btnW, btnH, TRUE);
        MoveWindow(g_hBtnMarkIn,    ix + (btnW + 6) * 3 + 16, iy, btnW, btnH, TRUE);
        MoveWindow(g_hBtnMarkOut,   ix + (btnW + 6) * 4 + 16, iy, btnW, btnH, TRUE);
        iy += btnH + 6;
        MoveWindow(g_hTimeline, ix, iy, avail, seekH, TRUE);
    }
    y += playerH + M;

    // === Start Processing button — very bottom ===
    MoveWindow(g_hStartButton, M, y, totalW, 32, TRUE);

    // frame preview fills remaining client area below the start button
    InvalidateRect(hwnd, nullptr, TRUE);
}

// ------------------------------ App Entry ------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr);

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icex);

    // Register custom timeline window class
    {
        WNDCLASS wct = {};
        wct.lpfnWndProc   = TimelineWndProc;
        wct.hInstance     = hInstance;
        wct.lpszClassName = L"ResizerTimeline";
        wct.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        RegisterClass(&wct);
    }

    const wchar_t CLASS_NAME[] = L"FFmpegDragDropClass";
    WNDCLASS wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // WM_ERASEBKGND handled manually for theme support
    wc.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));

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

    ApplyTheme(g_mainHwnd);   // sets colours and dark title bar before first paint
    InitializeCriticalSection(&g_csState);

    ShowWindow(g_mainHwnd, nCmdShow);
    UpdateWindow(g_mainHwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DeleteCriticalSection(&g_csState);
    if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    return (int)msg.wParam;
}

// ------------------------------ Theme Implementation ------------------------------
static inline Gdiplus::Color GdipColor(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

static bool IsSystemDarkMode() {
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

static void ApplyTheme(HWND hwnd) {
    g_darkMode = IsSystemDarkMode();

    if (g_hBkBrush)   { DeleteObject(g_hBkBrush);   g_hBkBrush   = nullptr; }
    if (g_hEditBrush) { DeleteObject(g_hEditBrush); g_hEditBrush = nullptr; }

    if (g_darkMode) {
        g_theme = {
            RGB( 28,  28,  32),   // bk
            RGB( 44,  44,  50),   // editBk
            RGB(218, 220, 226),   // text
            RGB(208, 210, 218),   // editText
            RGB( 58,  60,  74),   // pillNormal
            RGB( 28,  30,  40),   // pillPressed
            RGB( 45,  47,  55),   // pillDisabled
            RGB(150, 152, 170),   // pillBorder
        };
    } else {
        g_theme = {
            RGB(235, 235, 240),   // bk
            RGB(255, 255, 255),   // editBk
            RGB( 22,  22,  28),   // text
            RGB( 15,  15,  20),   // editText
            RGB(155, 158, 178),   // pillNormal
            RGB(120, 123, 142),   // pillPressed
            RGB(195, 197, 210),   // pillDisabled
            RGB(100, 102, 122),   // pillBorder
        };
    }

    g_hBkBrush   = CreateSolidBrush(g_theme.bk);
    g_hEditBrush = CreateSolidBrush(g_theme.editBk);

    // Dark/light title bar (Windows 10 20H1+ attribute 20; fall back to 19)
    BOOL dm = g_darkMode ? TRUE : FALSE;
    if (DwmSetWindowAttribute(hwnd, 20, &dm, sizeof(dm)) != S_OK)
        DwmSetWindowAttribute(hwnd, 19, &dm, sizeof(dm));

    // Opt every control into the correct visual style so borders, radio dots
    // and scrollbars all render in the right mode — not just their text/fill.
    if (hwnd) {
        LPCWSTR editTheme   = g_darkMode ? L"DarkMode_CFD"      : L"";
        LPCWSTR ctrlTheme   = g_darkMode ? L"DarkMode_Explorer"  : L"";

        // Group boxes — border + caption in correct colour
        if (g_hGrpSettings)   SetWindowTheme(g_hGrpSettings,   ctrlTheme, nullptr);
        if (g_hGrpRange)      SetWindowTheme(g_hGrpRange,       ctrlTheme, nullptr);
        if (g_hGrpResolution) SetWindowTheme(g_hGrpResolution,  ctrlTheme, nullptr);
        if (g_hGrpPlayer)     SetWindowTheme(g_hGrpPlayer,      ctrlTheme, nullptr);
        if (g_hGrpMedia)      SetWindowTheme(g_hGrpMedia,       ctrlTheme, nullptr);
        if (g_hGrpSaveLoc)    SetWindowTheme(g_hGrpSaveLoc,     ctrlTheme, nullptr);

        // Edit controls — border + scrollbar rendered dark
        if (g_hSizeEdit)      SetWindowTheme(g_hSizeEdit,      editTheme, nullptr);
        if (g_hSuffixEdit)    SetWindowTheme(g_hSuffixEdit,    editTheme, nullptr);
        if (g_hStartEdit)     SetWindowTheme(g_hStartEdit,     editTheme, nullptr);
        if (g_hEndEdit)       SetWindowTheme(g_hEndEdit,       editTheme, nullptr);
        if (g_hSavePathEdit)  SetWindowTheme(g_hSavePathEdit,  editTheme, nullptr);

        // Combo boxes
        if (g_hAudioDrop) SetWindowTheme(g_hAudioDrop, editTheme, nullptr);
        if (g_hSubsDrop)  SetWindowTheme(g_hSubsDrop,  editTheme, nullptr);

        // Radio buttons — dot, circle and text rendered in correct mode
        if (g_hRangeFullRadio)   SetWindowTheme(g_hRangeFullRadio,   ctrlTheme, nullptr);
        if (g_hRangeCustomRadio) SetWindowTheme(g_hRangeCustomRadio, ctrlTheme, nullptr);
        if (g_hSaveSameRadio)    SetWindowTheme(g_hSaveSameRadio,    ctrlTheme, nullptr);
        if (g_hSaveCustomRadio)  SetWindowTheme(g_hSaveCustomRadio,  ctrlTheme, nullptr);
        if (g_hGrpColor)         SetWindowTheme(g_hGrpColor,         ctrlTheme, nullptr);
        if (g_hColorDrop)        SetWindowTheme(g_hColorDrop,        editTheme, nullptr);
    }

    // Repaint everything
    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

// ------------------------------ Player Button Icons ------------------------------
static void DrawPlayerButton(const DRAWITEMSTRUCT* dis) {
    bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED)  != 0;

    int W = dis->rcItem.right  - dis->rcItem.left;
    int H = dis->rcItem.bottom - dis->rcItem.top;
    float cx = W * 0.5f + (pressed ? 0.5f : 0.0f);
    float cy = H * 0.5f + (pressed ? 0.5f : 0.0f);

    UINT id = GetDlgCtrlID(dis->hwndItem);

    // Erase the full rect first (clears corners outside any shape)
    FillRect(dis->hDC, &dis->rcItem, g_hBkBrush ? g_hBkBrush : GetSysColorBrush(COLOR_WINDOW));

    Gdiplus::Graphics g(dis->hDC);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    float bx = 1.0f, by = 1.0f;
    float bw = (float)W - 2.0f, bh = (float)H - 2.0f;

    // ---- Start Processing button — teal gradient with text ----
    if (id == IDC_START_BUTTON) {
        float r = 8.0f;
        Gdiplus::GraphicsPath path;
        path.AddArc(bx,          by, r*2.0f, bh, 90.0f, 180.0f);
        path.AddArc(bx+bw-r*2.0f, by, r*2.0f, bh, 270.0f, 180.0f);
        path.CloseFigure();

        Gdiplus::Color c1, c2;
        if      (disabled) { c1 = Gdiplus::Color(255, 34, 50, 48); c2 = Gdiplus::Color(255, 26, 38, 36); }
        else if (pressed)  { c1 = Gdiplus::Color(255, 16, 108, 98); c2 = Gdiplus::Color(255, 12, 84, 76); }
        else               { c1 = Gdiplus::Color(255, 28, 152, 138); c2 = Gdiplus::Color(255, 20, 118, 106); }

        Gdiplus::LinearGradientBrush lgb(
            Gdiplus::PointF(0.0f, by), Gdiplus::PointF(0.0f, by + bh), c1, c2);
        g.FillPath(&lgb, &path);
        if (!disabled) {
            Gdiplus::Pen pen(Gdiplus::Color(90, 100, 255, 220), 1.0f);
            g.DrawPath(&pen, &path);
        }

        BYTE ta = disabled ? 85 : 235;
        Gdiplus::SolidBrush tb(Gdiplus::Color(ta, 220, 255, 248));
        HFONT hf = g_hFont ? g_hFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        Gdiplus::Font font(dis->hDC, hf);
        Gdiplus::RectF tr(bx, by, bw, bh);
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(L"\u25B6\u2006Start Processing", -1, &font, tr, &sf, &tb);
        return;
    }

    // ---- Resolution dropdown button ----
    if (id == IDC_RES_DROPDOWN) {
        float r  = H * 0.5f - 1.0f;
        Gdiplus::Color bgCol = disabled ? GdipColor(g_theme.pillDisabled)
                             : pressed  ? GdipColor(g_theme.pillPressed)
                                        : GdipColor(g_theme.pillNormal);
        {
            Gdiplus::GraphicsPath path;
            path.AddArc(bx, by, r*2.0f, bh, 90.0f, 180.0f);
            path.AddArc(bx+bw-r*2.0f, by, r*2.0f, bh, 270.0f, 180.0f);
            path.CloseFigure();
            Gdiplus::SolidBrush b(bgCol);
            g.FillPath(&b, &path);
            if (!disabled) {
                Gdiplus::Pen pen(GdipColor(g_theme.pillBorder, 80), 1.0f);
                g.DrawPath(&pen, &path);
            }
        }

        BYTE a = disabled ? 65 : (pressed ? 195 : 228);
        // Build label: "Full 1920×1080" etc.
        wchar_t resText[48];
        const wchar_t* labels[] = { L"Full", L"Half", L"Quarter" };
        if (g_vidWidth > 0) {
            int dw = g_vidWidth  >> g_resSelection;
            int dh = g_vidHeight >> g_resSelection;
            StringCchPrintfW(resText, 48, L"%s  %d\u00D7%d", labels[g_resSelection], dw, dh);
        } else {
            StringCchCopyW(resText, 48, labels[g_resSelection]);
        }

        float chevW = 18.0f;
        HFONT hf = g_hFont ? g_hFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        Gdiplus::Font font(dis->hDC, hf);
        Gdiplus::SolidBrush tb(Gdiplus::Color(a, 215, 220, 232));
        Gdiplus::RectF textRect(bx + 10.0f, by, bw - chevW - 10.0f, bh);
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentNear);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        g.DrawString(resText, -1, &font, textRect, &sf, &tb);

        // Chevron ▼
        float cy2 = H * 0.5f;
        float cvx = bx + bw - chevW * 0.5f - 2.0f;
        Gdiplus::SolidBrush cb(Gdiplus::Color(a, 150, 155, 175));
        Gdiplus::PointF chevPts[3] = { {cvx-4.5f, cy2-2.5f}, {cvx+4.5f, cy2-2.5f}, {cvx, cy2+3.0f} };
        g.FillPolygon(&cb, chevPts, 3);
        return;
    }

    // ---- Player icon buttons — pill ----
    float r  = H * 0.5f - 1.0f;

    {
        Gdiplus::Color bgCol = disabled ? GdipColor(g_theme.pillDisabled)
                             : pressed  ? GdipColor(g_theme.pillPressed)
                                        : GdipColor(g_theme.pillNormal);
        Gdiplus::GraphicsPath path;
        path.AddArc(bx,                  by, r * 2.0f, bh, 90.0f, 180.0f);
        path.AddArc(bx + bw - r * 2.0f, by, r * 2.0f, bh, 270.0f, 180.0f);
        path.CloseFigure();
        Gdiplus::SolidBrush b(bgCol);
        g.FillPath(&b, &path);
        if (!disabled) {
            Gdiplus::Pen pen(GdipColor(g_theme.pillBorder, 80), 1.0f);
            g.DrawPath(&pen, &path);
        }
    }

    BYTE a = disabled ? 65 : (pressed ? 195 : 228);

    if (id == IDC_BTN_PLAYPAUSE) {
        if (g_isPlaying) {
            // Pause — two amber bars
            Gdiplus::SolidBrush b(Gdiplus::Color(a, 255, 198, 55));
            float bw2 = 4.5f, bh2 = 13.0f, gap = 5.5f;
            g.FillRectangle(&b, cx - bw2 - gap * 0.5f, cy - bh2 * 0.5f, bw2, bh2);
            g.FillRectangle(&b, cx             + gap * 0.5f, cy - bh2 * 0.5f, bw2, bh2);
        }
        else {
            // Play — teal-green triangle
            Gdiplus::SolidBrush b(Gdiplus::Color(a, 72, 220, 155));
            Gdiplus::PointF pts[3] = { {cx-6.5f, cy-9.0f}, {cx-6.5f, cy+9.0f}, {cx+9.0f, cy} };
            g.FillPolygon(&b, pts, 3);
        }
    }
    else if (id == IDC_BTN_BACK) {
        // |◀  coral step-back
        Gdiplus::SolidBrush b(Gdiplus::Color(a, 255, 125, 75));
        g.FillRectangle(&b, cx - 9.5f, cy - 8.0f, 3.5f, 16.0f);
        Gdiplus::PointF pts[3] = { {cx-5.0f, cy}, {cx+7.5f, cy-8.0f}, {cx+7.5f, cy+8.0f} };
        g.FillPolygon(&b, pts, 3);
    }
    else if (id == IDC_BTN_FWD) {
        // ▶|  coral step-forward
        Gdiplus::SolidBrush b(Gdiplus::Color(a, 255, 125, 75));
        Gdiplus::PointF pts[3] = { {cx-7.5f, cy-8.0f}, {cx-7.5f, cy+8.0f}, {cx+5.0f, cy} };
        g.FillPolygon(&b, pts, 3);
        g.FillRectangle(&b, cx + 6.0f, cy - 8.0f, 3.5f, 16.0f);
    }
    else if (id == IDC_BTN_MARKIN) {
        // [→  violet bracket + right arrow (set start)
        Gdiplus::Color col(a, 155, 95, 255);
        Gdiplus::Pen pen(col, 2.5f);
        pen.SetStartCap(Gdiplus::LineCapSquare);
        pen.SetEndCap(Gdiplus::LineCapSquare);
        g.DrawLine(&pen, cx - 9.0f, cy - 8.0f, cx - 9.0f, cy + 8.0f);
        g.DrawLine(&pen, cx - 9.0f, cy - 8.0f, cx - 4.5f, cy - 8.0f);
        g.DrawLine(&pen, cx - 9.0f, cy + 8.0f, cx - 4.5f, cy + 8.0f);
        Gdiplus::SolidBrush b(col);
        Gdiplus::PointF pts[3] = { {cx-2.5f, cy-6.0f}, {cx-2.5f, cy+6.0f}, {cx+8.5f, cy} };
        g.FillPolygon(&b, pts, 3);
    }
    else if (id == IDC_BTN_MARKOUT) {
        // ←]  hot-pink left arrow + bracket (set end)
        Gdiplus::Color col(a, 255, 75, 135);
        Gdiplus::SolidBrush b(col);
        Gdiplus::PointF pts[3] = { {cx+2.5f, cy-6.0f}, {cx+2.5f, cy+6.0f}, {cx-8.5f, cy} };
        g.FillPolygon(&b, pts, 3);
        Gdiplus::Pen pen(col, 2.5f);
        pen.SetStartCap(Gdiplus::LineCapSquare);
        pen.SetEndCap(Gdiplus::LineCapSquare);
        g.DrawLine(&pen, cx + 9.0f, cy - 8.0f, cx + 9.0f, cy + 8.0f);
        g.DrawLine(&pen, cx + 9.0f, cy - 8.0f, cx + 4.5f, cy - 8.0f);
        g.DrawLine(&pen, cx + 9.0f, cy + 8.0f, cx + 4.5f, cy + 8.0f);
    }
}

// ------------------------------ Persistent Settings ------------------------------
static const wchar_t* kRegKey = L"Software\\Resizer";

static void LoadSettings() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return;

    DWORD type, sz;

    // Target size
    wchar_t sizeBuf[64] = {};
    sz = sizeof(sizeBuf);
    if (RegQueryValueExW(hk, L"TargetSizeMB", nullptr, &type,
                         (BYTE*)sizeBuf, &sz) == ERROR_SUCCESS && type == REG_SZ && sizeBuf[0])
        SetWindowTextW(g_hSizeEdit, sizeBuf);

    // Suffix
    wchar_t sfxBuf[256] = {};
    sz = sizeof(sfxBuf);
    if (RegQueryValueExW(hk, L"Suffix", nullptr, &type,
                         (BYTE*)sfxBuf, &sz) == ERROR_SUCCESS && type == REG_SZ)
        SetWindowTextW(g_hSuffixEdit, sfxBuf);

    // Custom save folder flag
    DWORD custom = 0;
    sz = sizeof(custom);
    if (RegQueryValueExW(hk, L"SaveCustom", nullptr, &type,
                         (BYTE*)&custom, &sz) == ERROR_SUCCESS && type == REG_DWORD)
        g_saveCustom = (custom != 0);

    // Custom folder path
    wchar_t folder[MAX_PATH] = {};
    sz = sizeof(folder);
    if (RegQueryValueExW(hk, L"SaveFolder", nullptr, &type,
                         (BYTE*)folder, &sz) == ERROR_SUCCESS && type == REG_SZ)
        wcscpy_s(g_saveFolder, folder);

    RegCloseKey(hk);

    // Apply to UI
    if (g_saveCustom && g_saveFolder[0]) {
        SendMessage(g_hSaveSameRadio,   BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessage(g_hSaveCustomRadio, BM_SETCHECK, BST_CHECKED,   0);
        SetWindowTextW(g_hSavePathEdit, g_saveFolder);
        EnableWindow(g_hSavePathEdit,   TRUE);
        EnableWindow(g_hSaveBrowseBtn,  TRUE);
    } else {
        g_saveCustom = false;
        SendMessage(g_hSaveSameRadio,   BM_SETCHECK, BST_CHECKED,   0);
        SendMessage(g_hSaveCustomRadio, BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(g_hSavePathEdit,   FALSE);
        EnableWindow(g_hSaveBrowseBtn,  FALSE);
    }
}

static void SaveSettings() {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return;

    // Target size
    wchar_t sizeBuf[64] = {};
    GetWindowTextW(g_hSizeEdit, sizeBuf, ARRAYSIZE(sizeBuf));
    RegSetValueExW(hk, L"TargetSizeMB", 0, REG_SZ,
                   (const BYTE*)sizeBuf, (DWORD)((wcslen(sizeBuf) + 1) * sizeof(wchar_t)));

    // Suffix
    wchar_t sfxBuf[256] = {};
    GetWindowTextW(g_hSuffixEdit, sfxBuf, ARRAYSIZE(sfxBuf));
    RegSetValueExW(hk, L"Suffix", 0, REG_SZ,
                   (const BYTE*)sfxBuf, (DWORD)((wcslen(sfxBuf) + 1) * sizeof(wchar_t)));

    // Custom flag
    DWORD custom = g_saveCustom ? 1 : 0;
    RegSetValueExW(hk, L"SaveCustom", 0, REG_DWORD, (const BYTE*)&custom, sizeof(custom));

    // Folder path
    RegSetValueExW(hk, L"SaveFolder", 0, REG_SZ,
                   (const BYTE*)g_saveFolder,
                   (DWORD)((wcslen(g_saveFolder) + 1) * sizeof(wchar_t)));

    RegCloseKey(hk);
}

// ------------------------------ Window Proc ------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        DragAcceptFiles(hwnd, TRUE);

        // Create UI font from system metrics (Segoe UI on modern Windows)
        {
            NONCLIENTMETRICSW ncm = {};
            ncm.cbSize = sizeof(ncm);
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);

            // Semibold label font — same face, slightly larger, heavier weight
            LOGFONTW lf = ncm.lfMessageFont;
            lf.lfWeight = FW_SEMIBOLD;
            if (lf.lfHeight > 0) lf.lfHeight += 1; else if (lf.lfHeight < 0) lf.lfHeight -= 1;
            g_hLabelFont = CreateFontIndirectW(&lf);
        }

        // Group boxes created first so child controls render on top of them
        g_hGrpSettings = CreateWindowEx(0, L"BUTTON", L"Output Settings",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hwnd, (HMENU)IDC_GRP_SETTINGS, GetModuleHandle(nullptr), nullptr);
        g_hGrpRange = CreateWindowEx(0, L"BUTTON", L"Range",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hwnd, (HMENU)IDC_GRP_RANGE, GetModuleHandle(nullptr), nullptr);
        g_hGrpResolution = CreateWindowEx(0, L"BUTTON", L"Resolution",
            WS_CHILD | BS_GROUPBOX,   // not WS_VISIBLE — replaced by dropdown in Settings group
            0, 0, 0, 0, hwnd, (HMENU)IDC_GRP_RESOLUTION, GetModuleHandle(nullptr), nullptr);
        g_hGrpMedia = CreateWindowEx(0, L"BUTTON", L"Audio & Subtitles",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hwnd, (HMENU)IDC_GRP_MEDIA, GetModuleHandle(nullptr), nullptr);
        g_hGrpSaveLoc = CreateWindowEx(0, L"BUTTON", L"Save Location",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hwnd, (HMENU)IDC_GRP_SAVELOC, GetModuleHandle(nullptr), nullptr);
        g_hGrpPlayer = CreateWindowEx(0, L"BUTTON", L"Player",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hwnd, (HMENU)IDC_GRP_PLAYER, GetModuleHandle(nullptr), nullptr);
        g_hGrpColor = CreateWindowEx(0, L"BUTTON", L"Color Space",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hwnd, (HMENU)IDC_GRP_COLOR, GetModuleHandle(nullptr), nullptr);

        g_hInfoStatic = CreateWindowEx(0, L"STATIC", L"Drop a video file onto this window",
            WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, 10, 10, 240, 20, hwnd,
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
            WS_CHILD | WS_DISABLED | BS_AUTORADIOBUTTON | WS_GROUP,
            0, 0, 0, 0, hwnd, (HMENU)IDC_SCALE_FULL_RADIO, GetModuleHandle(nullptr), nullptr);
        g_hHalfRadio = CreateWindowEx(0, L"BUTTON", L"Half resolution",
            WS_CHILD | WS_DISABLED | BS_AUTORADIOBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_SCALE_HALF_RADIO, GetModuleHandle(nullptr), nullptr);
        g_hQuarterRadio = CreateWindowEx(0, L"BUTTON", L"Quarter resolution",
            WS_CHILD | WS_DISABLED | BS_AUTORADIOBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_SCALE_QUARTER_RADIO, GetModuleHandle(nullptr), nullptr);
        // Hidden from here on — state is still maintained, replaced visually by g_hResDrop
        SendMessage(g_hFullRadio, BM_SETCHECK, BST_CHECKED, 0);

        g_hResDrop = CreateWindowEx(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_RES_DROPDOWN, GetModuleHandle(nullptr), nullptr);

        g_hResStatic = CreateWindowEx(0, L"STATIC", L"Resolution:",
            WS_CHILD | WS_VISIBLE | WS_DISABLED,
            0, 0, 0, 0, hwnd, (HMENU)IDC_RES_STATIC, GetModuleHandle(nullptr), nullptr);

        g_hAudioStatic = CreateWindowEx(0, L"STATIC", L"Audio track:",
            WS_CHILD | WS_VISIBLE | WS_DISABLED,
            0, 0, 0, 0, hwnd, (HMENU)IDC_AUDIO_STATIC, GetModuleHandle(nullptr), nullptr);
        g_hAudioDrop = CreateWindowEx(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 0, 120, hwnd, (HMENU)IDC_AUDIO_DROPDOWN, GetModuleHandle(nullptr), nullptr);

        g_hSubsStatic = CreateWindowEx(0, L"STATIC", L"Burn in Subtitles:",
            WS_CHILD | WS_VISIBLE | WS_DISABLED,
            0, 0, 0, 0, hwnd, (HMENU)IDC_SUBS_STATIC, GetModuleHandle(nullptr), nullptr);
        g_hSubsDrop = CreateWindowEx(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 0, 120, hwnd, (HMENU)IDC_SUBS_DROPDOWN, GetModuleHandle(nullptr), nullptr);

        g_hStartButton = CreateWindowEx(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW,
            10, 210, 150, 30, hwnd, (HMENU)IDC_START_BUTTON, GetModuleHandle(nullptr), nullptr);

        g_hBtnPlayPause = CreateWindowEx(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 10, 250, 90, 26, hwnd,
            (HMENU)IDC_BTN_PLAYPAUSE, GetModuleHandle(nullptr), nullptr);
        g_hBtnBack = CreateWindowEx(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 110, 250, 90, 26, hwnd,
            (HMENU)IDC_BTN_BACK, GetModuleHandle(nullptr), nullptr);
        g_hBtnFwd = CreateWindowEx(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 210, 250, 90, 26, hwnd,
            (HMENU)IDC_BTN_FWD, GetModuleHandle(nullptr), nullptr);
        g_hBtnMarkIn = CreateWindowEx(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 330, 250, 90, 26, hwnd,
            (HMENU)IDC_BTN_MARKIN, GetModuleHandle(nullptr), nullptr);
        g_hBtnMarkOut = CreateWindowEx(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 430, 250, 90, 26, hwnd,
            (HMENU)IDC_BTN_MARKOUT, GetModuleHandle(nullptr), nullptr);
        g_hTimeline = CreateWindowEx(0, L"ResizerTimeline", L"",
            WS_CHILD | WS_VISIBLE,
            10, 285, 600, 72, hwnd, (HMENU)IDC_SEEKBAR, GetModuleHandle(nullptr), nullptr);

        g_hSaveSameRadio = CreateWindowEx(0, L"BUTTON", L"Same folder as source",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            0, 0, 0, 0, hwnd, (HMENU)IDC_SAVE_SAME_RADIO, GetModuleHandle(nullptr), nullptr);
        g_hSaveCustomRadio = CreateWindowEx(0, L"BUTTON", L"Custom folder",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_SAVE_CUSTOM_RADIO, GetModuleHandle(nullptr), nullptr);
        g_hSavePathEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_SAVE_PATH_EDIT, GetModuleHandle(nullptr), nullptr);
        g_hSaveBrowseBtn = CreateWindowEx(0, L"BUTTON", L"Browse\u2026",
            WS_CHILD | WS_VISIBLE | WS_DISABLED,
            0, 0, 0, 0, hwnd, (HMENU)IDC_SAVE_BROWSE_BTN, GetModuleHandle(nullptr), nullptr);
        SendMessage(g_hSaveSameRadio, BM_SETCHECK, BST_CHECKED, 0);

        g_hColorDrop = CreateWindowEx(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 0, 120, hwnd, (HMENU)IDC_COLOR_DROP, GetModuleHandle(nullptr), nullptr);

        // Apply UI font to every control
        if (g_hFont) {
            auto applyFont = [&](HWND h) { SendMessage(h, WM_SETFONT, (WPARAM)g_hFont, FALSE); };
            HFONT lf = g_hLabelFont ? g_hLabelFont : g_hFont;
            auto applyLabelFont = [&](HWND h) { SendMessage(h, WM_SETFONT, (WPARAM)lf, FALSE); };

            applyFont(g_hGrpSettings);    applyFont(g_hGrpRange);
            applyFont(g_hGrpResolution);  applyFont(g_hGrpPlayer);
            applyFont(g_hGrpMedia);
            applyFont(g_hInfoStatic);
            applyLabelFont(g_hSizeStatic);     applyFont(g_hSizeEdit);
            applyLabelFont(g_hSuffixStatic);   applyFont(g_hSuffixEdit);
            applyLabelFont(g_hResStatic);      applyFont(g_hResDrop);
            applyLabelFont(g_hRangeFullRadio); applyLabelFont(g_hRangeCustomRadio);
            applyLabelFont(g_hStartStatic);    applyFont(g_hStartEdit);
            applyLabelFont(g_hEndStatic);      applyFont(g_hEndEdit);
            applyFont(g_hFullRadio);      applyFont(g_hHalfRadio);   applyFont(g_hQuarterRadio);
            applyLabelFont(g_hAudioStatic);    applyFont(g_hAudioDrop);
            applyLabelFont(g_hSubsStatic);     applyFont(g_hSubsDrop);
            applyFont(g_hBtnPlayPause);   applyFont(g_hBtnBack);
            applyFont(g_hBtnFwd);         applyFont(g_hBtnMarkIn);   applyFont(g_hBtnMarkOut);
            applyFont(g_hGrpSaveLoc);
            applyLabelFont(g_hSaveSameRadio);  applyLabelFont(g_hSaveCustomRadio);
            applyFont(g_hSavePathEdit);        applyFont(g_hSaveBrowseBtn);
            applyFont(g_hGrpColor);            applyFont(g_hColorDrop);
        }

        // Create tooltip control and register each button
        g_hTooltip = CreateWindowEx(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_BALLOON,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
        SendMessage(g_hTooltip, TTM_SETMAXTIPWIDTH, 0, 300);
        SendMessage(g_hTooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 8000);

        auto addTip = [&](HWND hCtrl, LPCWSTR text) {
            TOOLINFOW ti = {};
            ti.cbSize   = sizeof(ti);
            ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd     = hwnd;
            ti.uId      = (UINT_PTR)hCtrl;
            ti.lpszText = const_cast<LPWSTR>(text);
            SendMessageW(g_hTooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        };
        addTip(g_hStartButton,  L"Transcode video to the target file size");
        addTip(g_hResDrop,      L"Output resolution — click to change");
        addTip(g_hAudioDrop,    L"Select which audio track to include in the output");
        addTip(g_hSubsDrop,      L"Select a subtitle track to burn into the video, or None to skip");
        addTip(g_hSaveBrowseBtn, L"Choose the folder where the output file will be saved");
        addTip(g_hColorDrop,     L"Color space — for HDR content, choose to preserve HDR or convert to SDR on export");
        addTip(g_hBtnPlayPause,  L"Play / Pause  [Space]");
        addTip(g_hBtnBack,      L"Step back one frame  [\u2190]");
        addTip(g_hBtnFwd,       L"Step forward one frame  [\u2192]");
        addTip(g_hBtnMarkIn,    L"Set clip start to current position  [I]");
        addTip(g_hBtnMarkOut,   L"Set clip end to current position  [O]");

        // Restore saved settings from the registry
        LoadSettings();

        break;
    }

    case WM_ERASEBKGND: {
        if (!g_hBkBrush) break;
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, g_hBkBrush);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        // Labels, info bar, group-box caption text
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_theme.text);
        SetBkColor(hdc, g_theme.bk);
        return (LRESULT)g_hBkBrush;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_theme.editText);
        SetBkColor(hdc, g_theme.editBk);
        return (LRESULT)g_hEditBrush;
    }

    case WM_CTLCOLORBTN: {
        // Group boxes (non-owner-draw BUTTON controls)
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_theme.text);
        SetBkColor(hdc, g_theme.bk);
        return (LRESULT)g_hBkBrush;
    }

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED: {
        if (msg == WM_SETTINGCHANGE &&
            !(lParam && lstrcmpW((LPCWSTR)lParam, L"ImmersiveColorSet") == 0))
            break;
        ApplyTheme(hwnd);
        break;
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlType == ODT_BUTTON) { DrawPlayerButton(dis); return TRUE; }
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
        // Stop any running playback from a previous file before loading the new one.
        // Without this the old decode thread keeps writing stale frames into g_hFrameBitmap.
        g_isPlaying = false;
        g_playerReady = false;
        StopPlayback();
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
                g_resSelection = 0;
                EnableWindow(g_hResStatic, TRUE);
                EnableWindow(g_hResDrop, TRUE);
                InvalidateRect(g_hResDrop, nullptr, TRUE);

                EnableWindow(g_hAudioStatic, TRUE);
                EnableWindow(g_hAudioDrop,   TRUE);
                EnableWindow(g_hSubsStatic,  TRUE);
                EnableWindow(g_hSubsDrop,    TRUE);
                PopulateAudioAndSubsDropdowns(g_inputPath);

                DetectHdr(g_inputPath);
                SendMessage(g_hColorDrop, CB_RESETCONTENT, 0, 0);
                if (g_isHdr) {
                    SendMessageW(g_hColorDrop, CB_ADDSTRING, 0, (LPARAM)L"HDR (preserve)");
                    SendMessageW(g_hColorDrop, CB_ADDSTRING, 0, (LPARAM)L"Convert to SDR");
                    SendMessage(g_hColorDrop, CB_SETCURSEL, 0, 0);
                    EnableWindow(g_hColorDrop, TRUE);
                } else {
                    SendMessageW(g_hColorDrop, CB_ADDSTRING, 0, (LPARAM)L"SDR");
                    SendMessage(g_hColorDrop, CB_SETCURSEL, 0, 0);
                    EnableWindow(g_hColorDrop, FALSE);
                }

                EnableWindow(g_hStartButton, TRUE);

                wchar_t startBuf[32], endBuf[32];
                StringCchPrintfW(startBuf, 32, L"0");
                int durInt = (int)floor(g_duration);
                StringCchPrintfW(endBuf, 32, L"%d", durInt);
                SetWindowTextW(g_hStartEdit, startBuf);
                SetWindowTextW(g_hEndEdit, endBuf);

                g_tlMax = (int)(g_duration * 1000);
                g_tlPos = 0;
                g_tlEnabled = true;
                InvalidateRect(g_hTimeline, nullptr, TRUE);

                EnableWindow(g_hBtnPlayPause, TRUE);
                EnableWindow(g_hBtnFwd, TRUE);
                EnableWindow(g_hBtnBack, TRUE);
                EnableWindow(g_hBtnMarkIn, TRUE);
                EnableWindow(g_hBtnMarkOut, TRUE);
                InvalidateRect(g_hBtnPlayPause, nullptr, TRUE);

                // Start thumbnail extraction in background
                g_thumbThreadStop = true;
                if (g_thumbThread) {
                    WaitForSingleObject(g_thumbThread, 3000);
                    CloseHandle(g_thumbThread);
                    g_thumbThread = nullptr;
                }
                for (int i = 0; i < 21; i++) {
                    if (g_thumbs[i]) { DeleteObject(g_thumbs[i]); g_thumbs[i] = nullptr; }
                }
                g_thumbW = 0; g_thumbH = 0;
                g_thumbThreadStop = false;
                g_thumbThread = (HANDLE)_beginthreadex(nullptr, 0, ThumbExtractThreadProc, nullptr, 0, nullptr);

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

    case WM_APP_THUMBS_READY: {
        InvalidateRect(g_hTimeline, nullptr, FALSE);
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        if (id == IDC_RES_DROPDOWN) {
            // Show resolution popup menu below the button
            HMENU hMenu = CreatePopupMenu();
            wchar_t lbl0[64], lbl1[64], lbl2[64];
            if (g_vidWidth > 0) {
                StringCchPrintfW(lbl0, 64, L"Full      %d\u00D7%d", g_vidWidth, g_vidHeight);
                StringCchPrintfW(lbl1, 64, L"Half      %d\u00D7%d", g_vidWidth/2, g_vidHeight/2);
                StringCchPrintfW(lbl2, 64, L"Quarter  %d\u00D7%d",  g_vidWidth/4, g_vidHeight/4);
            } else {
                StringCchCopyW(lbl0, 64, L"Full resolution");
                StringCchCopyW(lbl1, 64, L"Half resolution");
                StringCchCopyW(lbl2, 64, L"Quarter resolution");
            }
            AppendMenuW(hMenu, MF_STRING | (g_resSelection==0 ? MF_CHECKED : 0), 1, lbl0);
            AppendMenuW(hMenu, MF_STRING | (g_resSelection==1 ? MF_CHECKED : 0), 2, lbl1);
            AppendMenuW(hMenu, MF_STRING | (g_resSelection==2 ? MF_CHECKED : 0), 3, lbl2);
            RECT btnRect; GetWindowRect(g_hResDrop, &btnRect);
            int sel = TrackPopupMenu(hMenu,
                TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
                btnRect.left, btnRect.bottom, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
            if (sel >= 1 && sel <= 3) {
                g_resSelection = sel - 1;
                // Keep hidden radios in sync so existing scale-factor logic works
                SendMessage(g_hFullRadio,    BM_SETCHECK, g_resSelection==0 ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessage(g_hHalfRadio,    BM_SETCHECK, g_resSelection==1 ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessage(g_hQuarterRadio, BM_SETCHECK, g_resSelection==2 ? BST_CHECKED : BST_UNCHECKED, 0);
                InvalidateRect(g_hResDrop, nullptr, TRUE);
            }
        }
        else if (id == IDC_RANGE_FULL_RADIO) {
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
        else if (id == IDC_SAVE_SAME_RADIO) {
            g_saveCustom = false;
            EnableWindow(g_hSavePathEdit,  FALSE);
            EnableWindow(g_hSaveBrowseBtn, FALSE);
        }
        else if (id == IDC_SAVE_CUSTOM_RADIO) {
            g_saveCustom = true;
            EnableWindow(g_hSavePathEdit,  TRUE);
            EnableWindow(g_hSaveBrowseBtn, TRUE);
        }
        else if (id == IDC_SAVE_BROWSE_BTN) {
            IFileOpenDialog* pDlg = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg)))) {
                DWORD dwOpts = 0;
                pDlg->GetOptions(&dwOpts);
                pDlg->SetOptions(dwOpts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                pDlg->SetTitle(L"Select save folder");
                if (g_saveFolder[0]) {
                    IShellItem* pFolder = nullptr;
                    if (SUCCEEDED(SHCreateItemFromParsingName(g_saveFolder, nullptr,
                                                               IID_PPV_ARGS(&pFolder)))) {
                        pDlg->SetFolder(pFolder);
                        pFolder->Release();
                    }
                }
                if (SUCCEEDED(pDlg->Show(hwnd))) {
                    IShellItem* pResult = nullptr;
                    if (SUCCEEDED(pDlg->GetResult(&pResult))) {
                        PWSTR pPath = nullptr;
                        if (SUCCEEDED(pResult->GetDisplayName(SIGDN_FILESYSPATH, &pPath))) {
                            wcscpy_s(g_saveFolder, pPath);
                            SetWindowTextW(g_hSavePathEdit, g_saveFolder);
                            CoTaskMemFree(pPath);
                        }
                        pResult->Release();
                    }
                }
                pDlg->Release();
            }
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

                // Determine output directory
                char outDir[MAX_PATH] = {};
                if (g_saveCustom && g_saveFolder[0]) {
                    WideCharToMultiByte(CP_ACP, 0, g_saveFolder, -1, outDir, MAX_PATH, nullptr, nullptr);
                    size_t dlen = strlen(outDir);
                    if (dlen > 0 && outDir[dlen - 1] != '\\' && outDir[dlen - 1] != '/')
                        strcat_s(outDir, "\\");
                } else {
                    StringCchPrintfA(outDir, MAX_PATH, "%s%s", drive, dir);
                }

                char candidate[MAX_PATH];
                StringCchPrintfA(candidate, MAX_PATH, "%s%s_%s%s", outDir, fname, suffixA, ext);
                if (_access(candidate, 0) == 0) {
                    for (int i = 1;; i++) {
                        StringCchPrintfA(candidate, MAX_PATH, "%s%s_%s-%d%s",
                                         outDir, fname, suffixA, i, ext);
                        if (_access(candidate, 0) != 0) break;
                    }
                }
                StringCchCopyA(outPath, MAX_PATH, candidate);
            }

            // Read user-selected audio and subtitle stream indices
            int selAudio = -1, selSubs = -1;
            {
                LRESULT a = SendMessage(g_hAudioDrop, CB_GETCURSEL, 0, 0);
                if (a != CB_ERR) selAudio = (int)(INT_PTR)SendMessage(g_hAudioDrop, CB_GETITEMDATA, a, 0);
                LRESULT s = SendMessage(g_hSubsDrop, CB_GETCURSEL, 0, 0);
                if (s != CB_ERR) selSubs  = (int)(INT_PTR)SendMessage(g_hSubsDrop,  CB_GETITEMDATA, s, 0);
            }

            bool convertHdrToSdr = false;
            if (g_isHdr) {
                LRESULT colorSel = SendMessage(g_hColorDrop, CB_GETCURSEL, 0, 0);
                convertHdrToSdr = (colorSel == 1);  // index 1 = "Convert to SDR"
            }

            EnableWindow(g_hStartButton, FALSE);
            bool ok = TranscodeWithSizeAndScale(g_inputPath, outPath, targetSizeMB, scaleFactor,
                g_vidWidth, g_vidHeight, startSecs, endSecs, selAudio, selSubs, convertHdrToSdr);
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
        g_isGenerating = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    }

    case WM_TIMER: {
        if (wParam == IDT_UI_REFRESH) {
            UpdateSeekbarFromPos();
            if (g_isPlaying) InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        if (g_hFrameBitmap || g_isGenerating) {
            RECT clientRect; GetClientRect(hwnd, &clientRect);

            RECT sbRect;
            GetWindowRect(g_hStartButton, &sbRect);
            ScreenToClient(hwnd, (POINT*)&sbRect.left);
            ScreenToClient(hwnd, (POINT*)&sbRect.right);
            int margin = 10;
            int topY = sbRect.bottom + margin;

            int availW = clientRect.right - margin * 2;
            int availH = clientRect.bottom - topY - margin;
            if (availW > 0 && availH > 0) {
                if (g_isGenerating) {
                    RECT msgRect = { margin, topY, clientRect.right - margin, clientRect.bottom - margin };
                    HFONT oldFont = g_hFont ? (HFONT)SelectObject(hdc, g_hFont) : nullptr;
                    SetTextColor(hdc, g_darkMode ? RGB(140,142,150) : RGB(110,112,120));
                    SetBkMode(hdc, TRANSPARENT);
                    DrawTextW(hdc, L"Screenshot generating...", -1, &msgRect,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    if (oldFont) SelectObject(hdc, oldFont);
                }
                else {
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
        }

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DESTROY:
        SaveSettings();
        StopPlayback();
        g_thumbThreadStop = true;
        if (g_thumbThread) {
            WaitForSingleObject(g_thumbThread, 3000);
            CloseHandle(g_thumbThread);
            g_thumbThread = nullptr;
        }
        for (int i = 0; i < 21; i++) {
            if (g_thumbs[i]) { DeleteObject(g_thumbs[i]); g_thumbs[i] = nullptr; }
        }
        if (g_hFrameBitmap) { DeleteObject(g_hFrameBitmap); g_hFrameBitmap = nullptr; }
        if (g_hFont)        { DeleteObject(g_hFont);        g_hFont        = nullptr; }
        if (g_hLabelFont)   { DeleteObject(g_hLabelFont);   g_hLabelFont   = nullptr; }
        if (g_hBkBrush)     { DeleteObject(g_hBkBrush);    g_hBkBrush     = nullptr; }
        if (g_hEditBrush)   { DeleteObject(g_hEditBrush);  g_hEditBrush   = nullptr; }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ------------------------------ Audio / Subtitle Track Enumeration ------------------------------
static void PopulateAudioAndSubsDropdowns(const char* filepath) {
    SendMessage(g_hAudioDrop, CB_RESETCONTENT, 0, 0);
    SendMessage(g_hSubsDrop,  CB_RESETCONTENT, 0, 0);
    // "None" is always the first subtitle option (item data = -1 means no subtitles)
    {
        LRESULT ni = SendMessage(g_hSubsDrop, CB_ADDSTRING, 0, (LPARAM)L"None");
        SendMessage(g_hSubsDrop, CB_SETITEMDATA, ni, (LPARAM)(INT_PTR)-1);
    }

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filepath, nullptr, nullptr) < 0 ||
        avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
        LRESULT ni = SendMessage(g_hAudioDrop, CB_ADDSTRING, 0, (LPARAM)L"Default");
        SendMessage(g_hAudioDrop, CB_SETITEMDATA, ni, (LPARAM)(INT_PTR)-1);
        SendMessage(g_hAudioDrop, CB_SETCURSEL, 0, 0);
        SendMessage(g_hSubsDrop,  CB_SETCURSEL, 0, 0);
        return;
    }

    int audioIdx = 0, subIdx = 0;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* s = fmt_ctx->streams[i];

        if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++audioIdx;
            AVDictionaryEntry* lang  = av_dict_get(s->metadata, "language", nullptr, 0);
            AVDictionaryEntry* title = av_dict_get(s->metadata, "title",    nullptr, 0);
            const char* codec = avcodec_get_name(s->codecpar->codec_id);
            char labelA[128];
            if (title && lang)
                StringCchPrintfA(labelA, 128, "Track %d \xe2\x80\x94 %s (%s)", audioIdx, title->value, lang->value);
            else if (title)
                StringCchPrintfA(labelA, 128, "Track %d \xe2\x80\x94 %s", audioIdx, title->value);
            else if (lang)
                StringCchPrintfA(labelA, 128, "Track %d \xe2\x80\x94 %s [%s]", audioIdx, lang->value, codec);
            else
                StringCchPrintfA(labelA, 128, "Track %d \xe2\x80\x94 %s", audioIdx, codec);
            wchar_t labelW[128];
            MultiByteToWideChar(CP_UTF8, 0, labelA, -1, labelW, 128);
            LRESULT ni = SendMessage(g_hAudioDrop, CB_ADDSTRING, 0, (LPARAM)labelW);
            SendMessage(g_hAudioDrop, CB_SETITEMDATA, ni, (LPARAM)(INT_PTR)i); // global stream index
        }
        else if (s->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            ++subIdx;
            AVCodecID cid = s->codecpar->codec_id;
            AVDictionaryEntry* lang  = av_dict_get(s->metadata, "language", nullptr, 0);
            AVDictionaryEntry* title = av_dict_get(s->metadata, "title",    nullptr, 0);
            const char* codec = avcodec_get_name(cid);
            char labelA[128];
            if (title && lang)
                StringCchPrintfA(labelA, 128, "Track %d \xe2\x80\x94 %s (%s)", subIdx, title->value, lang->value);
            else if (title)
                StringCchPrintfA(labelA, 128, "Track %d \xe2\x80\x94 %s [%s]", subIdx, title->value, codec);
            else if (lang)
                StringCchPrintfA(labelA, 128, "Track %d \xe2\x80\x94 %s [%s]", subIdx, lang->value, codec);
            else
                StringCchPrintfA(labelA, 128, "Track %d \xe2\x80\x94 %s", subIdx, codec);
            wchar_t labelW[128];
            MultiByteToWideChar(CP_UTF8, 0, labelA, -1, labelW, 128);
            LRESULT ni = SendMessage(g_hSubsDrop, CB_ADDSTRING, 0, (LPARAM)labelW);
            SendMessage(g_hSubsDrop, CB_SETITEMDATA, ni, (LPARAM)(INT_PTR)i); // global stream index
        }
    }
    avformat_close_input(&fmt_ctx);

    if (audioIdx == 0) {
        LRESULT ni = SendMessage(g_hAudioDrop, CB_ADDSTRING, 0, (LPARAM)L"No audio tracks");
        SendMessage(g_hAudioDrop, CB_SETITEMDATA, ni, (LPARAM)(INT_PTR)-1);
    }
    SendMessage(g_hAudioDrop, CB_SETCURSEL, 0, 0);
    SendMessage(g_hSubsDrop,  CB_SETCURSEL, 0, 0);
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

// ------------------------------ HDR Helpers (shared by preview, playback & encode) ------
// Guard for older FFmpeg builds that don't define SWS_CS_BT2020
#ifndef SWS_CS_BT2020
#define SWS_CS_BT2020 9
#endif

// PQ (SMPTE ST 2084) EOTF: normalised [0,1] → linear light [0,1] (1.0 = 10 000 nits)
static double pq_eotf(double N) {
    if (N <= 0.0) return 0.0;
    const double m1 = 0.1593017578125, m2 = 78.84375;
    const double c1 = 0.8359375,       c2 = 18.8515625, c3 = 18.6875;
    double p   = pow(N, 1.0 / m2);
    double num = p - c1; if (num < 0.0) num = 0.0;
    double den = c2 - c3 * p;
    return den > 0.0 ? pow(num / den, 1.0 / m1) : 0.0;
}
// HLG (ARIB STD-B67) OETF inverse: normalised [0,1] → scene-linear [0,1]
static double hlg_eotf(double E) {
    if (E < 0.0) return 0.0;
    if (E <= 0.5) return (E * E) / 3.0;
    const double a = 0.17883277, b = 0.28466892, c = 0.55991073;
    return (exp((E - c) / a) + b) / 12.0;
}
// Reinhard tone mapping in linear light; white = reference white level.
// Maps v=0→0, v=white→1.0, v>white→clamped. Uses 2v/(white+v) so the
// slope at the origin is 2/white (linear), avoiding the quadratic collapse
// of the "extended" formula when v << white (common with PQ linear values).
static inline double reinhard_tm(double v, double white) {
    return 2.0 * v / (white + v);
}
// sRGB OETF: linear [0,1] → gamma-encoded uint8
static inline uint8_t srgb_pack(double v) {
    double g = (v <= 0.0031308) ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
    int i = (int)(g * 255.0 + 0.5);
    return (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
}
// BT.2020 → BT.709 primaries matrix (linear RGB)
static const double k_bt2020_to_bt709[3][3] = {
    { 1.6605, -0.5876, -0.0728 },
    {-0.1246,  1.1329, -0.0083 },
    {-0.0182, -0.1006,  1.1187 }
};

// Build an 8-bit LUT for fast display tone-mapping in the playback thread.
// sws_scale to BGR24 from an HDR source outputs PQ/HLG-encoded values linearly
// mapped to [0,255]; this LUT applies the inverse EOTF + Reinhard + sRGB.
static void BuildHdrDisplayLut() {
    bool   isPQ = (g_hdrTrc == AVCOL_TRC_SMPTE2084);
    double refW = isPQ ? 0.0203 : 0.25;
    for (int i = 0; i < 256; i++) {
        double v   = i / 255.0;
        double lin = isPQ ? pq_eotf(v) : hlg_eotf(v);
        double tm  = reinhard_tm(lin, refW);
        g_hdrDisplay8Lut[i] = srgb_pack(tm);
    }
    g_hdrLutValid = true;
}

// Detect HDR from container metadata; sets g_isHdr, g_hdrTrc, and rebuilds the LUT.
static void DetectHdr(const char* filepath) {
    g_isHdr       = false;
    g_hdrTrc      = AVCOL_TRC_UNSPECIFIED;
    g_hdrLutValid = false;
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filepath, nullptr, nullptr) < 0) return;
    if (avformat_find_stream_info(fmt_ctx, nullptr) >= 0) {
        for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                AVColorTransferCharacteristic trc =
                    fmt_ctx->streams[i]->codecpar->color_trc;
                g_hdrTrc = (int)trc;
                if (trc == AVCOL_TRC_SMPTE2084 || trc == AVCOL_TRC_ARIB_STD_B67)
                    g_isHdr = true;
                else if (fmt_ctx->streams[i]->codecpar->color_primaries == AVCOL_PRI_BT2020)
                    g_isHdr = true;  // BT.2020 primaries without explicit TRC tag
                break;
            }
        }
    }
    avformat_close_input(&fmt_ctx);
    if (g_isHdr) BuildHdrDisplayLut();
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
                bool isPQ  = (frame->color_trc == AVCOL_TRC_SMPTE2084);
                bool isHLG = (frame->color_trc == AVCOL_TRC_ARIB_STD_B67);
                if (isPQ || isHLG) {
                    // Full-quality HDR→SDR: decode → RGB48LE → EOTF+matrix+TM → sRGB
                    int srcCs = (frame->colorspace == AVCOL_SPC_BT2020_NCL ||
                                 frame->colorspace == AVCOL_SPC_BT2020_CL)
                                ? SWS_CS_BT2020 : SWS_CS_ITU709;
                    int srcRange = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
                    SwsContext* hdr_sws = sws_getContext(
                        dec_ctx->width, dec_ctx->height, (AVPixelFormat)frame->format,
                        dec_ctx->width, dec_ctx->height, AV_PIX_FMT_RGB48LE,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (hdr_sws) {
                        sws_setColorspaceDetails(hdr_sws,
                            sws_getCoefficients(srcCs), srcRange,
                            sws_getCoefficients(SWS_CS_ITU709), 1,
                            0, 1 << 16, 1 << 16);
                        int stride48 = dec_ctx->width * 6;
                        uint8_t* rgb48 = (uint8_t*)av_malloc((size_t)dec_ctx->height * stride48);
                        if (rgb48) {
                            uint8_t* d[1] = { rgb48 }; int s[1] = { stride48 };
                            sws_scale(hdr_sws, frame->data, frame->linesize,
                                      0, dec_ctx->height, d, s);
                            double refW = isPQ ? 0.0203 : 0.25;
                            for (int row = 0; row < dec_ctx->height; row++) {
                                const uint16_t* src16 =
                                    (const uint16_t*)(rgb48 + row * stride48);
                                uint8_t* dst =
                                    rgbFrame->data[0] + row * rgbFrame->linesize[0];
                                for (int x = 0; x < dec_ctx->width; x++) {
                                    double R = src16[x*3+0] / 65535.0;
                                    double G = src16[x*3+1] / 65535.0;
                                    double B = src16[x*3+2] / 65535.0;
                                    if (isPQ){ R=pq_eotf(R); G=pq_eotf(G); B=pq_eotf(B); }
                                    else     { R=hlg_eotf(R);G=hlg_eotf(G);B=hlg_eotf(B); }
                                    double Ro=k_bt2020_to_bt709[0][0]*R+k_bt2020_to_bt709[0][1]*G+k_bt2020_to_bt709[0][2]*B;
                                    double Go=k_bt2020_to_bt709[1][0]*R+k_bt2020_to_bt709[1][1]*G+k_bt2020_to_bt709[1][2]*B;
                                    double Bo=k_bt2020_to_bt709[2][0]*R+k_bt2020_to_bt709[2][1]*G+k_bt2020_to_bt709[2][2]*B;
                                    if (Ro<0.0) Ro=0.0; if (Go<0.0) Go=0.0; if (Bo<0.0) Bo=0.0;
                                    Ro=reinhard_tm(Ro,refW); Go=reinhard_tm(Go,refW); Bo=reinhard_tm(Bo,refW);
                                    dst[x*3+0] = srgb_pack(Bo);
                                    dst[x*3+1] = srgb_pack(Go);
                                    dst[x*3+2] = srgb_pack(Ro);
                                }
                            }
                            av_free(rgb48);
                            gotFrame = true;
                        }
                        sws_freeContext(hdr_sws);
                    }
                } else {
                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, dec_ctx->height,
                        rgbFrame->data, rgbFrame->linesize);
                    gotFrame = true;
                }
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
                    if (g_isHdr && g_hdrLutValid) {
                        for (int row = 0; row < dec_ctx->height; row++) {
                            uint8_t* p = rgbFrame->data[0] + row * rgbFrame->linesize[0];
                            for (int col = 0; col < dec_ctx->width * 3; col++)
                                p[col] = g_hdrDisplay8Lut[p[col]];
                        }
                    }

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
            if (g_isHdr && g_hdrLutValid) {
                for (int row = 0; row < dec_ctx->height; row++) {
                    uint8_t* p = rgbFrame->data[0] + row * rgbFrame->linesize[0];
                    for (int col = 0; col < dec_ctx->width * 3; col++)
                        p[col] = g_hdrDisplay8Lut[p[col]];
                }
            }

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
    int64_t sliderPos = (int64_t)g_tlPos;
    if (!g_hPlaybackThread) {
        // Sync g_currentPosMs to the slider before the thread starts so the
        // first timer tick never sees a stale value and snaps the slider back.
        g_currentPosMs = sliderPos;
        g_seekTargetMs = sliderPos;
        g_seekRequested = true;
        StartPlayback(hwnd);
        InvalidateRect(g_hBtnPlayPause, nullptr, TRUE);
        return;
    }
    if (!g_isPlaying) {
        // Resuming from pause — play from wherever the slider currently sits,
        // not from the last decoded frame position.
        g_currentPosMs = sliderPos;
        SeekMs(sliderPos, false);
    }
    g_isPlaying = !g_isPlaying;
    InvalidateRect(g_hBtnPlayPause, nullptr, TRUE);
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
    int64_t target = g_currentPosMs + frameMs;
    SeekMs(target, true);
    int64_t durMs = (int64_t)(g_duration * 1000.0);
    g_tlPos = (int)min(target, durMs);
    InvalidateRect(g_hTimeline, nullptr, FALSE);
}


void StepBackward(HWND hwnd) {
    EnsureThreadRunningPaused(hwnd);
    g_isPlaying = false;
    int frameMs = (int)max(1.0, 1000.0 / g_videoFPS);
    g_stepDir = -1;
    // Seek slightly earlier than one frame to ensure we land before target and then walk forward
    int64_t target = (g_currentPosMs > frameMs * 2) ? (g_currentPosMs - frameMs * 2) : 0;
    SeekMs(target, true);
    g_tlPos = (int)max((int64_t)0, target);
    InvalidateRect(g_hTimeline, nullptr, FALSE);
}


void UpdateSeekbarFromPos() {
    // Only auto-advance the slider during active playback.
    // When paused the slider belongs to the user and is never moved by the timer.
    if (!g_playerReady || !g_isPlaying || g_isDragging) return;
    g_tlPos = (int)g_currentPosMs;
    InvalidateRect(g_hTimeline, nullptr, FALSE);
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

// Bitmap subtitle types (PGS, VOBSUB) are pre-decoded to these structs and
// alpha-blended directly onto YUV frames; text-based subs go through libavfilter/libass.
struct PgsRect  { int x, y, w, h; std::vector<uint8_t> bgra; };
struct PgsEvent { int64_t pts_ms;  std::vector<PgsRect> rects; }; // rects.empty() = clear screen

bool TranscodeWithSizeAndScale(const char* in_filename, const char* out_filename, double target_size_mb,
    int scale_factor, int orig_w, int orig_h, double start_seconds, double end_seconds,
    int audio_stream_index, int subtitle_stream_index, bool convert_hdr_to_sdr) {
    int64_t           target_bitrate   = 0;
    AVFormatContext*  in_fmt_ctx       = nullptr;
    AVFormatContext*  out_fmt_ctx      = nullptr;
    AVCodecContext*   dec_ctx          = nullptr;
    AVCodecContext*   enc_ctx          = nullptr;
    AVStream*         video_in_stream  = nullptr;
    AVStream*         video_out_stream = nullptr;
    AVStream*         audio_in_stream  = nullptr;
    AVStream*         audio_out_stream = nullptr;
    const AVCodec*    video_decoder    = nullptr;
    const AVCodec*    video_encoder    = nullptr;
    SwsContext*       sws_ctx          = nullptr;
    SwsContext*       sws_hdr2rgb      = nullptr;   // HDR→SDR: native → RGB48LE
    SwsContext*       sws_rgb2yuv      = nullptr;   // HDR→SDR: BGR24 → encoder YUV
    uint8_t*          hdr_rgb48_buf    = nullptr;   // intermediate RGB48LE plane
    uint8_t*          hdr_bgr24_buf    = nullptr;   // intermediate BGR24 plane
    AVFilterGraph*    filter_graph     = nullptr;
    AVFilterContext*  buffersrc_ctx    = nullptr;
    AVFilterContext*  buffersink_ctx   = nullptr;
    bool              use_filter       = false;
    bool              use_bitmap_subs  = false;
    int               pgs_plane_w      = 0;   // subtitle coordinate space (may differ from video for 4K)
    int               pgs_plane_h      = 0;
    std::vector<PgsEvent> pgs_events;
    int               videoStreamIndex = -1;
    int               audioStreamIndex = -1;
    AVFrame*          frame            = nullptr;
    AVFrame*          filt_frame       = nullptr;
    AVPacket*         pkt              = nullptr;
    AVPacket*         enc_pkt          = nullptr;
    bool              success          = false;
    int64_t           video_start_pts  = 0;
    int64_t           audio_start_pts  = 0;

    if (end_seconds <= start_seconds) { OutputDebugStringA("End time must be greater than start time.\n"); return false; }
    double segment_duration = end_seconds - start_seconds;
    if (segment_duration <= 0.0) { OutputDebugStringA("Invalid segment duration.\n"); return false; }

    if (avformat_open_input(&in_fmt_ctx, in_filename, nullptr, nullptr) < 0) { OutputDebugStringA("Could not open input file.\n"); goto cleanup; }
    if (avformat_find_stream_info(in_fmt_ctx, nullptr) < 0) { OutputDebugStringA("Could not find stream info.\n"); goto cleanup; }
    for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; i++) {
        AVStream* st = in_fmt_ctx->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStreamIndex < 0) { videoStreamIndex = (int)i; }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStreamIndex < 0) { audioStreamIndex = (int)i; }
    }
    // Use user-selected audio stream if valid
    if (audio_stream_index >= 0 && (unsigned int)audio_stream_index < in_fmt_ctx->nb_streams &&
        in_fmt_ctx->streams[audio_stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        audioStreamIndex = audio_stream_index;

    if (videoStreamIndex < 0) { OutputDebugStringA("No video stream found.\n"); goto cleanup; }
    video_in_stream = in_fmt_ctx->streams[videoStreamIndex];
    if (audioStreamIndex >= 0) audio_in_stream = in_fmt_ctx->streams[audioStreamIndex];

    // Bitrate calculation: subtract the actual audio stream bitrate so video uses the remaining budget
    {
        int64_t total_bits    = (int64_t)(target_size_mb * 8.0 * 1024.0 * 1024.0);
        int64_t audio_bitrate = 0;
        if (audio_in_stream) {
            audio_bitrate = audio_in_stream->codecpar->bit_rate;
            if (audio_bitrate <= 0) audio_bitrate = 192000; // safe fallback if container doesn't report it
        }
        int64_t audio_bits  = (int64_t)(audio_bitrate * segment_duration);
        int64_t overhead    = (int64_t)(total_bits * 0.01); // ~1% for container overhead
        int64_t video_bits  = total_bits - audio_bits - overhead;
        if (video_bits <= 0) video_bits = total_bits / 2;  // audio alone exceeds budget; give video 50%
        target_bitrate = (int64_t)(video_bits / segment_duration);
    }
    if (target_bitrate <= 0) { OutputDebugStringA("Invalid target bitrate calculated.\n"); goto cleanup; }

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
    {
        // Prefer codec framerate, fall back to stream's avg_frame_rate, then 30 fps
        AVRational fps = dec_ctx->framerate;
        if (fps.num <= 0 || fps.den <= 0) fps = video_in_stream->avg_frame_rate;
        if (fps.num <= 0 || fps.den <= 0) fps = { 30, 1 };
        enc_ctx->time_base    = av_inv_q(fps);
        enc_ctx->max_b_frames = 0;  // no B-frames → DTS always == PTS → clean seeking
        enc_ctx->gop_size     = max(1, (int)(av_q2d(fps) * 2.0)); // keyframe every ~2 s
    }
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
    {
        AVDictionary* mux_opts = nullptr;
        av_dict_set(&mux_opts, "movflags", "faststart", 0); // moov atom at front → instant seeking
        int wh = avformat_write_header(out_fmt_ctx, &mux_opts);
        av_dict_free(&mux_opts);
        if (wh < 0) { OutputDebugStringA("Error writing header to output.\n"); goto cleanup; }
    }

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

    if (convert_hdr_to_sdr) {
        // Stage 1: decode native → RGB48LE with BT.2020 input colorspace
        int srcCs    = (dec_ctx->colorspace == AVCOL_SPC_BT2020_NCL ||
                        dec_ctx->colorspace == AVCOL_SPC_BT2020_CL)
                       ? SWS_CS_BT2020 : SWS_CS_ITU709;
        int srcRange = (dec_ctx->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
        sws_hdr2rgb = sws_getContext(dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
            enc_ctx->width, enc_ctx->height, AV_PIX_FMT_RGB48LE,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_hdr2rgb) {
            sws_setColorspaceDetails(sws_hdr2rgb,
                sws_getCoefficients(srcCs),        srcRange,
                sws_getCoefficients(SWS_CS_ITU709), 1,
                0, 1 << 16, 1 << 16);
        }
        // Stage 3: BGR24 (BT.709 full-range) → encoder YUV (BT.709 limited-range)
        sws_rgb2yuv = sws_getContext(enc_ctx->width, enc_ctx->height, AV_PIX_FMT_BGR24,
            enc_ctx->width, enc_ctx->height, enc_ctx->pix_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_rgb2yuv) {
            sws_setColorspaceDetails(sws_rgb2yuv,
                sws_getCoefficients(SWS_CS_ITU709), 1,   // src: BT.709, full range (sRGB 0-255)
                sws_getCoefficients(SWS_CS_ITU709), 0,   // dst: BT.709, limited range (H.264)
                0, 1 << 16, 1 << 16);
        }
        hdr_rgb48_buf = (uint8_t*)av_malloc((size_t)enc_ctx->height * enc_ctx->width * 6);
        hdr_bgr24_buf = (uint8_t*)av_malloc((size_t)enc_ctx->height * enc_ctx->width * 3);
        if (!sws_hdr2rgb || !sws_rgb2yuv || !hdr_rgb48_buf || !hdr_bgr24_buf) {
            OutputDebugStringA("HDR→SDR setup failed; falling back to direct encode.\n");
            convert_hdr_to_sdr = false;
        } else {
            // Tag output as BT.709 so players know it's been tone-mapped
            video_out_stream->codecpar->color_primaries = AVCOL_PRI_BT709;
            video_out_stream->codecpar->color_trc       = AVCOL_TRC_BT709;
            video_out_stream->codecpar->color_space     = AVCOL_SPC_BT709;
        }
    }

    // Pre-load bitmap subtitle events (PGS/VOBSUB) using a separate format context.
    // Text-based subtitles fall through to the libavfilter/libass path below.
    if (subtitle_stream_index >= 0 &&
        (unsigned)subtitle_stream_index < in_fmt_ctx->nb_streams) {
        AVCodecID cid = in_fmt_ctx->streams[subtitle_stream_index]->codecpar->codec_id;
        bool is_bitmap = (cid == AV_CODEC_ID_HDMV_PGS_SUBTITLE ||
                          cid == AV_CODEC_ID_DVD_SUBTITLE ||
                          cid == AV_CODEC_ID_XSUB);
        if (is_bitmap) {
            const AVCodec* sub_codec = avcodec_find_decoder(cid);
            AVCodecContext* sub_dec = sub_codec ? avcodec_alloc_context3(sub_codec) : nullptr;
            if (sub_dec) {
                avcodec_parameters_to_context(sub_dec, in_fmt_ctx->streams[subtitle_stream_index]->codecpar);
                if (avcodec_open2(sub_dec, sub_codec, nullptr) >= 0) {
                    // Record the subtitle plane dimensions so we can scale coords correctly.
                    // 4K Blu-ray PGS subtitles are typically authored at 1920×1080 even
                    // when the video track is 3840×2160.
                    pgs_plane_w = sub_dec->width;
                    pgs_plane_h = sub_dec->height;
                    AVFormatContext* sub_fmt = nullptr;
                    if (avformat_open_input(&sub_fmt, in_filename, nullptr, nullptr) >= 0 &&
                        avformat_find_stream_info(sub_fmt, nullptr) >= 0) {
                        AVPacket* sub_pkt = av_packet_alloc();
                        while (sub_pkt && av_read_frame(sub_fmt, sub_pkt) >= 0) {
                            if (sub_pkt->stream_index == subtitle_stream_index) {
                                AVSubtitle sub = {};
                                int got_sub = 0;
                                avcodec_decode_subtitle2(sub_dec, &sub, &got_sub, sub_pkt);
                                if (got_sub) {
                                    // AVSubtitle.pts is in microseconds (AV_TIME_BASE)
                                    int64_t s_ms = (sub.pts != AV_NOPTS_VALUE)
                                        ? sub.pts / 1000
                                        : (int64_t)(sub_pkt->pts * av_q2d(in_fmt_ctx->streams[subtitle_stream_index]->time_base) * 1000.0);
                                    PgsEvent ev;
                                    ev.pts_ms = s_ms;
                                    for (unsigned ri = 0; ri < sub.num_rects; ri++) {
                                        AVSubtitleRect* rect = sub.rects[ri];
                                        if (rect->type == SUBTITLE_BITMAP && rect->w > 0 && rect->h > 0) {
                                            PgsRect pr;
                                            pr.x = rect->x; pr.y = rect->y;
                                            pr.w = rect->w; pr.h = rect->h;
                                            pr.bgra.resize((size_t)pr.w * pr.h * 4);
                                            uint8_t* pal = rect->data[1]; // BGRA palette (256 × 4 bytes)
                                            for (int py = 0; py < pr.h; py++) {
                                                for (int px = 0; px < pr.w; px++) {
                                                    uint8_t idx = rect->data[0][py * rect->linesize[0] + px];
                                                    uint8_t* dst = pr.bgra.data() + ((size_t)py * pr.w + px) * 4;
                                                    dst[0] = pal[idx * 4 + 0]; // B
                                                    dst[1] = pal[idx * 4 + 1]; // G
                                                    dst[2] = pal[idx * 4 + 2]; // R
                                                    dst[3] = pal[idx * 4 + 3]; // A
                                                }
                                            }
                                            ev.rects.push_back(std::move(pr));
                                        }
                                    }
                                    pgs_events.push_back(std::move(ev));
                                    avsubtitle_free(&sub);
                                }
                            }
                            av_packet_unref(sub_pkt);
                        }
                        av_packet_free(&sub_pkt);
                        avformat_close_input(&sub_fmt);
                        std::sort(pgs_events.begin(), pgs_events.end(),
                            [](const PgsEvent& a, const PgsEvent& b){ return a.pts_ms < b.pts_ms; });
                        use_bitmap_subs = !pgs_events.empty();
                    }
                }
                avcodec_free_context(&sub_dec);
            }
        }
    }

    // Set up subtitle burn-in filter graph if a subtitle stream was selected
    // (text-based subtitles only — bitmap types handled by use_bitmap_subs above)
    if (subtitle_stream_index >= 0 && !use_bitmap_subs) {
        const AVFilter* buffersrc  = avfilter_get_by_name("buffer");
        const AVFilter* buffersink = avfilter_get_by_name("buffersink");
        if (buffersrc && buffersink) {
            filter_graph = avfilter_graph_alloc();
            if (filter_graph) {
                // Describe the input video stream for the buffer source
                char src_args[256];
                AVRational tb  = video_in_stream->time_base;
                AVRational sar = dec_ctx->sample_aspect_ratio;
                snprintf(src_args, sizeof(src_args),
                    "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                    dec_ctx->width, dec_ctx->height, (int)dec_ctx->pix_fmt,
                    tb.num, tb.den, sar.num ? sar.num : 1, sar.den ? sar.den : 1);

                int ret = avfilter_graph_create_filter(&buffersrc_ctx,  buffersrc,  "in",  src_args, nullptr, filter_graph);
                if (ret >= 0)
                    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", nullptr,  nullptr, filter_graph);

                if (ret >= 0) {
                    // Build the subtitles filter string.
                    // Path is wrapped in single quotes so colons don't need escaping;
                    // just convert backslashes to forward slashes (both work on Windows).
                    char esc[MAX_PATH * 2]; int k = 0;
                    for (int j = 0; in_filename[j] && k < (int)sizeof(esc) - 2; j++) {
                        char c = in_filename[j];
                        esc[k++] = (c == '\\') ? '/' : c;
                    }
                    esc[k] = '\0';
                    // The subtitles filter si= takes 0-based index among subtitle streams,
                    // not the global stream index stored in the combo box item data.
                    int subtitle_si = 0;
                    for (unsigned int j = 0; j < in_fmt_ctx->nb_streams && (int)j < subtitle_stream_index; j++)
                        if (in_fmt_ctx->streams[j]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
                            subtitle_si++;

                    // Get Windows Fonts dir so libass can initialize its font engine
                    char windir[MAX_PATH] = {};
                    GetWindowsDirectoryA(windir, MAX_PATH);
                    char fontspath[MAX_PATH + 8];
                    snprintf(fontspath, sizeof(fontspath), "%s/Fonts", windir);
                    for (int j = 0; fontspath[j]; j++)
                        if (fontspath[j] == '\\') fontspath[j] = '/';

                    char filter_descr[MAX_PATH * 2 + 512];
                    snprintf(filter_descr, sizeof(filter_descr),
                        "subtitles='%s':si=%d:fontsdir='%s'",
                        esc, subtitle_si, fontspath);

                    AVFilterInOut* outputs = avfilter_inout_alloc();
                    AVFilterInOut* inputs  = avfilter_inout_alloc();
                    if (outputs && inputs) {
                        outputs->name = av_strdup("in");  outputs->filter_ctx = buffersrc_ctx;  outputs->pad_idx = 0; outputs->next = nullptr;
                        inputs->name  = av_strdup("out"); inputs->filter_ctx  = buffersink_ctx; inputs->pad_idx  = 0; inputs->next  = nullptr;
                        ret = avfilter_graph_parse_ptr(filter_graph, filter_descr, &inputs, &outputs, nullptr);
                        if (ret >= 0) ret = avfilter_graph_config(filter_graph, nullptr);
                    }
                    avfilter_inout_free(&outputs);
                    avfilter_inout_free(&inputs);
                    if (ret >= 0) {
                        use_filter = true;
                    } else {
                        char errbuf[256]; av_strerror(ret, errbuf, sizeof(errbuf));
                        char msg[MAX_PATH * 2 + 512];
                        snprintf(msg, sizeof(msg),
                            "Subtitle filter setup failed.\n\nError: %s\n\nFilter string:\n%s",
                            errbuf, filter_descr);
                        MessageBoxA(nullptr, msg, "Subtitle Error", MB_OK | MB_ICONWARNING);
                        avfilter_graph_free(&filter_graph); filter_graph = nullptr;
                    }
                } else {
                    avfilter_graph_free(&filter_graph); filter_graph = nullptr;
                }
            }
        }
    }

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

                // Route through subtitle filter graph if active, otherwise scale directly
                AVFrame* src_frame = frame;
                AVFrame* filter_out = nullptr;
                if (use_filter) {
                    frame->pts = in_pts; // filter needs original pts for subtitle timing
                    if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) >= 0) {
                        filter_out = av_frame_alloc();
                        int fret = filter_out ? av_buffersink_get_frame(buffersink_ctx, filter_out) : AVERROR(ENOMEM);
                        // EAGAIN means the filter needs more frames — not an error, just use original
                        if (fret < 0) { av_frame_free(&filter_out); filter_out = nullptr; }
                        else { src_frame = filter_out; }
                    }
                }
                if (convert_hdr_to_sdr && sws_hdr2rgb && sws_rgb2yuv && hdr_rgb48_buf && hdr_bgr24_buf) {
                    // Stage 1: native → RGB48LE
                    int rgb48W     = enc_ctx->width;
                    int rgb48H     = enc_ctx->height;
                    int rgb48Stride = rgb48W * 6;
                    uint8_t* r48data[8] = { hdr_rgb48_buf, nullptr };
                    int      r48stride[8] = { rgb48Stride, 0 };
                    sws_scale(sws_hdr2rgb, src_frame->data, src_frame->linesize, 0, src_frame->height,
                        r48data, r48stride);
                    // Stage 2: per-pixel EOTF + BT.2020→BT.709 + Reinhard TM + sRGB
                    bool   isPQ = (g_hdrTrc == AVCOL_TRC_SMPTE2084);
                    double refW = isPQ ? 0.0203 : 0.25;
                    for (int row = 0; row < rgb48H; row++) {
                        const uint16_t* src16 = (const uint16_t*)(hdr_rgb48_buf + row * rgb48Stride);
                        uint8_t* dst8         = hdr_bgr24_buf + row * rgb48W * 3;
                        for (int x = 0; x < rgb48W; x++) {
                            double R = src16[x*3+0] / 65535.0;
                            double G = src16[x*3+1] / 65535.0;
                            double B = src16[x*3+2] / 65535.0;
                            if (isPQ) { R=pq_eotf(R); G=pq_eotf(G); B=pq_eotf(B); }
                            else      { R=hlg_eotf(R);G=hlg_eotf(G);B=hlg_eotf(B); }
                            double Ro = k_bt2020_to_bt709[0][0]*R+k_bt2020_to_bt709[0][1]*G+k_bt2020_to_bt709[0][2]*B;
                            double Go = k_bt2020_to_bt709[1][0]*R+k_bt2020_to_bt709[1][1]*G+k_bt2020_to_bt709[1][2]*B;
                            double Bo = k_bt2020_to_bt709[2][0]*R+k_bt2020_to_bt709[2][1]*G+k_bt2020_to_bt709[2][2]*B;
                            if (Ro<0.0) Ro=0.0; if (Go<0.0) Go=0.0; if (Bo<0.0) Bo=0.0;
                            Ro=reinhard_tm(Ro,refW); Go=reinhard_tm(Go,refW); Bo=reinhard_tm(Bo,refW);
                            dst8[x*3+0] = srgb_pack(Bo);
                            dst8[x*3+1] = srgb_pack(Go);
                            dst8[x*3+2] = srgb_pack(Ro);
                        }
                    }
                    // Stage 3: BGR24 → encoder YUV
                    uint8_t* b24data[8]  = { hdr_bgr24_buf, nullptr };
                    int      b24stride[8] = { rgb48W * 3, 0 };
                    sws_scale(sws_rgb2yuv, (const uint8_t* const*)b24data, b24stride, 0, rgb48H,
                        filt_frame->data, filt_frame->linesize);
                } else {
                    sws_scale(sws_ctx, src_frame->data, src_frame->linesize, 0, src_frame->height,
                        filt_frame->data, filt_frame->linesize);
                }
                if (filter_out) av_frame_free(&filter_out);

                // Alpha-blend PGS bitmap subtitle onto the scaled YUV output frame
                if (use_bitmap_subs) {
                    int64_t cur_ms = (int64_t)(in_time * 1000.0);
                    // Binary-search for the last event at or before cur_ms
                    const PgsEvent* active = nullptr;
                    for (const auto& ev : pgs_events) {
                        if (ev.pts_ms <= cur_ms) active = &ev;
                        else break;
                    }
                    if (active && !active->rects.empty()) {
                        // Scale from subtitle coordinate space → output frame.
                        // Use the subtitle plane's own dimensions if available; fall back to
                        // the decoded video size (handles non-4K sources where they match).
                        int sub_ref_w = (pgs_plane_w > 0) ? pgs_plane_w : dec_ctx->width;
                        int sub_ref_h = (pgs_plane_h > 0) ? pgs_plane_h : dec_ctx->height;
                        double sx = (double)enc_ctx->width  / sub_ref_w;
                        double sy = (double)enc_ctx->height / sub_ref_h;
                        for (const auto& rect : active->rects) {
                            for (int py = 0; py < rect.h; py++) {
                                for (int px = 0; px < rect.w; px++) {
                                    const uint8_t* bgra = rect.bgra.data() + ((size_t)py * rect.w + px) * 4;
                                    uint8_t a = bgra[3];
                                    if (a == 0) continue;
                                    int fx = (int)((rect.x + px) * sx);
                                    int fy = (int)((rect.y + py) * sy);
                                    if (fx < 0 || fy < 0 || fx >= enc_ctx->width || fy >= enc_ctx->height) continue;
                                    uint8_t r = bgra[2], g = bgra[1], b = bgra[0];
                                    int inv_a = 255 - a;
                                    // BT.601 studio-swing RGB → YCbCr
                                    int Y  = (( 66*r + 129*g +  25*b + 128) >> 8) + 16;
                                    int Cb = ((-38*r -  74*g + 112*b + 128) >> 8) + 128;
                                    int Cr = ((112*r -  94*g -  18*b + 128) >> 8) + 128;
                                    Y  = max(16, min(235, Y));
                                    Cb = max(16, min(240, Cb));
                                    Cr = max(16, min(240, Cr));
                                    // Blend luma
                                    uint8_t* Yp = filt_frame->data[0] + fy * filt_frame->linesize[0] + fx;
                                    *Yp = (uint8_t)((Y * a + *Yp * inv_a) >> 8);
                                    // Blend chroma at 4:2:0 sub-sample positions
                                    if ((fx & 1) == 0 && (fy & 1) == 0) {
                                        int cy = fy >> 1, cx = fx >> 1;
                                        if (enc_ctx->pix_fmt == AV_PIX_FMT_YUV420P) {
                                            uint8_t* Up = filt_frame->data[1] + cy * filt_frame->linesize[1] + cx;
                                            uint8_t* Vp = filt_frame->data[2] + cy * filt_frame->linesize[2] + cx;
                                            *Up = (uint8_t)((Cb * a + *Up * inv_a) >> 8);
                                            *Vp = (uint8_t)((Cr * a + *Vp * inv_a) >> 8);
                                        } else if (enc_ctx->pix_fmt == AV_PIX_FMT_NV12) {
                                            uint8_t* UVp = filt_frame->data[1] + cy * filt_frame->linesize[1] + cx * 2;
                                            *UVp       = (uint8_t)((Cb * a + *UVp       * inv_a) >> 8);
                                            *(UVp + 1) = (uint8_t)((Cr * a + *(UVp + 1) * inv_a) >> 8);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

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
    if (filter_graph) avfilter_graph_free(&filter_graph);
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (sws_hdr2rgb) sws_freeContext(sws_hdr2rgb);
    if (sws_rgb2yuv) sws_freeContext(sws_rgb2yuv);
    if (hdr_rgb48_buf) av_free(hdr_rgb48_buf);
    if (hdr_bgr24_buf) av_free(hdr_bgr24_buf);
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

// ------------------------------ Filmstrip Thumbnail Extraction ------------------------------

static HBITMAP ExtractFrameAtTime(const char* filepath, double timeSecs) {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext*  dec_ctx = nullptr;
    SwsContext*      sws_ctx = nullptr;
    AVPacket*        pkt     = nullptr;
    AVFrame*         frame   = nullptr;
    AVFrame*         rgbFrame = nullptr;
    uint8_t*         rgbBuffer = nullptr;
    HBITMAP          hBitmap = nullptr;
    int              videoIdx = -1;
    bool             gotFrame = false;

    if (avformat_open_input(&fmt_ctx, filepath, nullptr, nullptr) < 0) goto tf_cleanup;
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) goto tf_cleanup;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { videoIdx = i; break; }
    }
    if (videoIdx < 0) goto tf_cleanup;
    {
        AVStream* vs = fmt_ctx->streams[videoIdx];
        const AVCodec* dec = avcodec_find_decoder(vs->codecpar->codec_id);
        if (!dec) goto tf_cleanup;
        dec_ctx = avcodec_alloc_context3(dec);
        if (!dec_ctx) goto tf_cleanup;
        if (avcodec_parameters_to_context(dec_ctx, vs->codecpar) < 0) goto tf_cleanup;
        if (avcodec_open2(dec_ctx, dec, nullptr) < 0) goto tf_cleanup;

        // Compute thumbnail size preserving aspect ratio, capped at 160×90
        int srcW = dec_ctx->width, srcH = dec_ctx->height;
        int dstH = 90;
        int dstW = (srcH > 0) ? (int)((double)srcW / srcH * dstH) : 160;
        if (dstW < 1) dstW = 1;
        // Store thumb dimensions on first call (all frames same source size)
        if (g_thumbW == 0) { g_thumbW = dstW; g_thumbH = dstH; }

        // sws_ctx is created after decoding the first frame so we can use
        // frame->format (the decoder's actual output format) rather than
        // dec_ctx->pix_fmt, which may be wrong for 10-bit HEVC before decoding.
        int bufSize = av_image_get_buffer_size(AV_PIX_FMT_BGR24, dstW, dstH, 1);
        rgbBuffer = (uint8_t*)av_malloc(bufSize);
        frame    = av_frame_alloc();
        rgbFrame = av_frame_alloc();
        pkt      = av_packet_alloc();
        if (!rgbBuffer || !frame || !rgbFrame || !pkt) goto tf_cleanup;
        av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer,
            AV_PIX_FMT_BGR24, dstW, dstH, 1);

        int64_t seek_ts = (int64_t)(timeSecs * AV_TIME_BASE);
        av_seek_frame(fmt_ctx, -1, seek_ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec_ctx);

        // Seek lands on the nearest keyframe *before* timeSecs.  If we take the
        // very first decoded frame we always get that keyframe, so every thumbnail
        // within the same GOP (often 2-10 s) looks identical.  Instead, decode
        // forward and skip frames until we reach the requested timestamp.
        double   tbase     = av_q2d(vs->time_base);
        bool     frameDone = false;

        while (!g_thumbThreadStop && !frameDone && av_read_frame(fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index == videoIdx &&
                avcodec_send_packet(dec_ctx, pkt) == 0) {

                while (!frameDone && avcodec_receive_frame(dec_ctx, frame) == 0) {
                    // Convert frame PTS to seconds; prefer best_effort_timestamp.
                    int64_t pts = frame->best_effort_timestamp;
                    if (pts == AV_NOPTS_VALUE) pts = frame->pts;
                    double fSecs = (pts != AV_NOPTS_VALUE)
                        ? (double)pts * tbase
                        : timeSecs; // PTS unknown — accept whatever we have

                    if (fSecs + 0.001 >= timeSecs) {
                        // This frame is at or past the target time — process it.
                        frameDone = true;

                        AVPixelFormat srcFmt = (AVPixelFormat)frame->format;
                        bool isPQ  = (frame->color_trc == AVCOL_TRC_SMPTE2084);
                        bool isHLG = (frame->color_trc == AVCOL_TRC_ARIB_STD_B67);

                        if (isPQ || isHLG) {
                            // --- HDR path: decode → RGB48LE → EOTF → TM → sRGB → BGR24 ---
                            int srcCs = (frame->colorspace == AVCOL_SPC_BT2020_NCL ||
                                         frame->colorspace == AVCOL_SPC_BT2020_CL)
                                        ? SWS_CS_BT2020 : SWS_CS_ITU709;
                            int srcRange = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;

                            SwsContext* hdr_sws = sws_getContext(
                                srcW, srcH, srcFmt,
                                dstW, dstH, AV_PIX_FMT_RGB48LE,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
                            if (hdr_sws) {
                                sws_setColorspaceDetails(hdr_sws,
                                    sws_getCoefficients(srcCs),    srcRange,
                                    sws_getCoefficients(SWS_CS_ITU709), 1,
                                    0, 1 << 16, 1 << 16);

                                int rgb48Stride = dstW * 6;
                                uint8_t* rgb48 = (uint8_t*)av_malloc(dstH * rgb48Stride);
                                if (rgb48) {
                                    uint8_t* d[1] = { rgb48 };
                                    int      s[1] = { rgb48Stride };
                                    sws_scale(hdr_sws, frame->data, frame->linesize,
                                              0, srcH, d, s);

                                    const double refW = isPQ ? 0.0203 : 0.25;
                                    for (int y = 0; y < dstH; y++) {
                                        const uint16_t* src16 =
                                            (const uint16_t*)(rgb48 + y * rgb48Stride);
                                        uint8_t* dst =
                                            rgbFrame->data[0] + y * rgbFrame->linesize[0];
                                        for (int x = 0; x < dstW; x++) {
                                            double R = src16[x*3+0] / 65535.0;
                                            double G = src16[x*3+1] / 65535.0;
                                            double B = src16[x*3+2] / 65535.0;
                                            if (isPQ) { R = pq_eotf(R); G = pq_eotf(G); B = pq_eotf(B); }
                                            else       { R = hlg_eotf(R); G = hlg_eotf(G); B = hlg_eotf(B); }
                                            double Ro = k_bt2020_to_bt709[0][0]*R + k_bt2020_to_bt709[0][1]*G + k_bt2020_to_bt709[0][2]*B;
                                            double Go = k_bt2020_to_bt709[1][0]*R + k_bt2020_to_bt709[1][1]*G + k_bt2020_to_bt709[1][2]*B;
                                            double Bo = k_bt2020_to_bt709[2][0]*R + k_bt2020_to_bt709[2][1]*G + k_bt2020_to_bt709[2][2]*B;
                                            if (Ro < 0.0) Ro = 0.0; if (Go < 0.0) Go = 0.0; if (Bo < 0.0) Bo = 0.0;
                                            Ro = reinhard_tm(Ro, refW); Go = reinhard_tm(Go, refW); Bo = reinhard_tm(Bo, refW);
                                            dst[x*3+0] = srgb_pack(Bo);
                                            dst[x*3+1] = srgb_pack(Go);
                                            dst[x*3+2] = srgb_pack(Ro);
                                        }
                                    }
                                    av_free(rgb48);
                                    gotFrame = true;
                                }
                                sws_freeContext(hdr_sws);
                            }
                        } else {
                            // --- SDR path ---
                            sws_ctx = sws_getContext(
                                srcW, srcH, srcFmt,
                                dstW, dstH, AV_PIX_FMT_BGR24,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
                            if (sws_ctx) {
                                sws_scale(sws_ctx, frame->data, frame->linesize, 0, srcH,
                                    rgbFrame->data, rgbFrame->linesize);
                                gotFrame = true;
                            }
                        }
                    }
                    av_frame_unref(frame); // always unref; processing already copied data
                }
            }
            av_packet_unref(pkt);
        }
        if (!gotFrame) goto tf_cleanup;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = dstW;
        bmi.bmiHeader.biHeight      = -dstH;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 24;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* dibBits = nullptr;
        HDC hdc = GetDC(nullptr);
        hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        ReleaseDC(nullptr, hdc);
        if (hBitmap && dibBits) {
            // DIB rows are DWORD-aligned; srcFrame rows may not be.
            // Using dstW*3 as the destination stride corrupts any video
            // whose thumbnail width is not a multiple of 4 (e.g. 2.35:1 → dstW=211).
            int srcRowBytes = dstW * 3;
            int dibStride   = (srcRowBytes + 3) & ~3;  // round up to 4-byte boundary
            for (int y = 0; y < dstH; y++)
                memcpy((uint8_t*)dibBits + y * dibStride,
                    rgbFrame->data[0] + y * rgbFrame->linesize[0], srcRowBytes);
        }
    }
tf_cleanup:
    if (sws_ctx)  sws_freeContext(sws_ctx);
    if (frame)    av_frame_free(&frame);
    if (rgbFrame) { if (rgbBuffer) av_free(rgbBuffer); av_frame_free(&rgbFrame); }
    if (pkt)      av_packet_free(&pkt);
    if (dec_ctx)  avcodec_free_context(&dec_ctx);
    if (fmt_ctx)  avformat_close_input(&fmt_ctx);
    return hBitmap;
}

static unsigned __stdcall ThumbExtractThreadProc(void* /*param*/) {
    // Determine the timeline's pixel width so that each thumbnail is captured at
    // the exact time corresponding to the centre of its display slot.
    // Slot i spans [i*W/21, (i+1)*W/21]; its centre is at (i+0.5)*W/21 pixels,
    // which maps to time (i+0.5)/21 * duration — matching the playhead formula.
    int tlW = 0;
    if (g_hTimeline) {
        RECT tlrc; GetClientRect(g_hTimeline, &tlrc);
        tlW = tlrc.right;
    }
    const int N = 21;
    for (int i = 0; i < N && !g_thumbThreadStop; i++) {
        // Centre of slot i in pixels → time
        double centerPx = (i + 0.5) * (tlW > 0 ? tlW : 1000) / (double)N;
        double t = centerPx / (tlW > 0 ? tlW : 1000) * g_duration;
        // Simplifies to: t = (i + 0.5) / N * duration, independent of width.
        // The width query is kept so the intent is explicit and survives resize logic.
        HBITMAP bmp = ExtractFrameAtTime(g_inputPath, t);
        if (!g_thumbThreadStop) {
            g_thumbs[i] = bmp;
            PostMessage(g_mainHwnd, WM_APP_THUMBS_READY, 0, 0);
        } else {
            if (bmp) DeleteObject(bmp);
        }
    }
    return 0;
}

// ------------------------------ Timeline Window Proc ------------------------------
LRESULT CALLBACK TimelineWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right, H = rc.bottom;

        // Dark base
        HBRUSH bgBrush = CreateSolidBrush(RGB(18, 18, 22));
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);

        // Draw filmstrip thumbnails
        if (g_thumbW > 0 && g_thumbH > 0 && W > 0) {
            float slotW = (float)W / 21.0f;
            for (int i = 0; i <= 20; i++) {
                if (!g_thumbs[i]) continue;
                int x0 = (int)(i * slotW);
                int x1 = (int)((i + 1) * slotW);
                int sw = max(1, x1 - x0);
                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP old = (HBITMAP)SelectObject(memDC, g_thumbs[i]);
                SetStretchBltMode(hdc, HALFTONE);
                SetBrushOrgEx(hdc, 0, 0, nullptr);
                StretchBlt(hdc, x0, 0, sw, H, memDC, 0, 0, g_thumbW, g_thumbH, SRCCOPY);
                SelectObject(memDC, old);
                DeleteDC(memDC);
            }
        }

        // Thin separator lines between thumbnail slots
        if (g_thumbW > 0 && W > 0) {
            HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
            HPEN oldPen = (HPEN)SelectObject(hdc, sepPen);
            float slotW = (float)W / 21.0f;
            for (int i = 1; i <= 20; i++) {
                int x = (int)(i * slotW);
                MoveToEx(hdc, x, 0, nullptr);
                LineTo(hdc, x, H);
            }
            SelectObject(hdc, oldPen);
            DeleteObject(sepPen);
        }

        // White playhead line
        if (g_tlMax > 0 && W > 0) {
            int px = (int)((double)g_tlPos / g_tlMax * W);
            if (px < 0) px = 0; if (px >= W) px = W - 1;
            HPEN headPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HPEN oldPen  = (HPEN)SelectObject(hdc, headPen);
            MoveToEx(hdc, px, 0, nullptr);
            LineTo(hdc, px, H);
            SelectObject(hdc, oldPen);
            DeleteObject(headPen);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!g_tlEnabled || !g_playerReady) break;
        SetCapture(hwnd);
        g_isDragging = true;
        RECT rc; GetClientRect(hwnd, &rc);
        int x = GET_X_LPARAM(lParam);
        g_tlPos = (int)((double)x / max(1, rc.right) * g_tlMax);
        g_tlPos = max(0, min(g_tlPos, g_tlMax));
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    }

    case WM_MOUSEMOVE: {
        if (!g_isDragging) break;
        RECT rc; GetClientRect(hwnd, &rc);
        int x = GET_X_LPARAM(lParam);
        g_tlPos = (int)((double)x / max(1, rc.right) * g_tlMax);
        g_tlPos = max(0, min(g_tlPos, g_tlMax));
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    }

    case WM_LBUTTONUP: {
        if (!g_isDragging) break;
        ReleaseCapture();
        g_isDragging = false;
        RECT rc; GetClientRect(hwnd, &rc);
        int x = GET_X_LPARAM(lParam);
        g_tlPos = (int)((double)x / max(1, rc.right) * g_tlMax);
        g_tlPos = max(0, min(g_tlPos, g_tlMax));
        InvalidateRect(hwnd, nullptr, FALSE);
        // Seek to final position
        if (g_playerReady) {
            g_isGenerating = true;
            EnsureThreadRunningPaused(g_mainHwnd);
            SeekMs(g_tlPos, !g_isPlaying);
            InvalidateRect(g_mainHwnd, nullptr, FALSE);
        }
        break;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
