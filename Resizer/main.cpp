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
#include <future>
#include <thread>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "msimg32.lib")
#include <windowsx.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <process.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <deque>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
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
#define IDC_BTN_GOTO_MARKIN       2007
#define IDC_BTN_GOTO_MARKOUT      2008

#define IDT_UI_REFRESH            3001
#define IDT_ENCODE_PROGRESS       3002

#define WM_APP_THUMBS_READY  (WM_APP + 3)
#define WM_APP_ENCODE_DONE   (WM_APP + 4)

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
static char     g_extSubPaths[8][MAX_PATH] = {};  // external subtitle files found next to video
static int      g_extSubCount = 0;
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
static HWND     g_hBtnGotoMarkIn  = nullptr;
static HWND     g_hBtnGotoMarkOut = nullptr;
static int64_t  g_markInMs  = -1;   // -1 = not set
static int64_t  g_markOutMs = -1;

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

// Hardware acceleration (NVDEC/NVENC)
static AVBufferRef* g_hwDeviceCtx   = nullptr;  // CUDA device; null = no NVDEC available
static bool         g_nvdecAvailable = false;

// Encode progress (background thread)
static volatile float  g_encodeProgress = 0.0f;  // 0.0..1.0
static volatile bool   g_encodeRunning  = false;
static HANDLE          g_encodeThread   = nullptr;
static char            g_encodeOutPath[MAX_PATH] = {};
static int             g_videoTop       = 0;   // client-y where video preview starts (below start button)

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
static CRITICAL_SECTION g_csMarkCache;
static volatile bool g_playThreadShouldExit = false;
static volatile bool g_isPlaying = false;
static volatile bool g_seekRequested = false;
static volatile int64_t g_seekTargetMs = 0;
static volatile int64_t g_currentPosMs = 0;
static volatile bool g_decodeSingleFrame = false;
static double   g_videoFPS = 30.0;
static bool     g_playerReady = false;
static volatile int  g_stepDir = 0; // +1 forward, -1 backward
static volatile bool g_stepFileReady = false; // thread file ptr is right after last step-decoded frame
static bool          g_isDragging   = false; // true while user drags the seekbar thumb
static bool          g_isGenerating = false; // true while a seek-frame decode is in flight

// Forward declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK TimelineWndProc(HWND, UINT, WPARAM, LPARAM);
static bool IsSystemDarkMode();
static void ApplyTheme(HWND hwnd);
void HandleResize(HWND hwnd, int clientW, int clientH);
static unsigned __stdcall ThumbExtractThreadProc(void* param);
bool GetVideoInfo(const char* filepath, int& width, int& height, double& durationSeconds);
HBITMAP ExtractMiddleFrameBitmap(const char* filepath, int orig_w, int orig_h, double duration);
bool TranscodeWithSizeAndScale(const char* in_filename, const char* out_filename, double target_size_mb,
    int scale_factor, int orig_w, int orig_h, double start_seconds, double end_seconds,
    int audio_stream_index = -1, int subtitle_stream_index = -1, bool convert_hdr_to_sdr = false,
    const char* ext_subtitle_path = nullptr);

// Audio/subtitle track enumeration
static void PopulateAudioAndSubsDropdowns(const char* filepath);
static void ScanExternalSubtitles(const char* videoPath);
static void DetectHdr(const char* filepath);

// Playback helpers
unsigned __stdcall PlaybackThreadProc(void*);
void StartPlayback(HWND hwnd);
void StopPlayback();
void TogglePlayPause(HWND hwnd);
void EnsureThreadRunningPaused(HWND hwnd);
void SeekMs(int64_t ms, bool decodeSingle);
static bool IsXvidAvi(const char* filepath);
static bool RemuxXvidToMp4(const char* in_path, const char* out_path);
void StepForward(HWND hwnd);
void StepBackward(HWND hwnd);
void UpdateSeekbarFromPos();
void SetMarkInFromCurrent(HWND hwnd);
void SetMarkOutFromCurrent(HWND hwnd);

// ------------------------------ NVIDIA Hardware Acceleration ------------------------------
static const char* get_cuvid_name(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264:       return "h264_cuvid";
        case AV_CODEC_ID_HEVC:       return "hevc_cuvid";
        case AV_CODEC_ID_AV1:        return "av1_cuvid";
        case AV_CODEC_ID_VP9:        return "vp9_cuvid";
        case AV_CODEC_ID_VP8:        return "vp8_cuvid";
        case AV_CODEC_ID_MPEG2VIDEO: return "mpeg2_cuvid";
        case AV_CODEC_ID_MPEG4:      return "mpeg4_cuvid";
        case AV_CODEC_ID_VC1:        return "vc1_cuvid";
        default: return nullptr;
    }
}

static AVPixelFormat get_hw_format(AVCodecContext*, const AVPixelFormat* pix_fmts) {
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_CUDA) return AV_PIX_FMT_CUDA;
    return pix_fmts[0];
}

static void TryInitHWDevice() {
    if (av_hwdevice_ctx_create(&g_hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA,
                               nullptr, nullptr, 0) >= 0)
        g_nvdecAvailable = true;
}

// Returns best decoder for codec_id: cuvid (NVDEC) if available, else software.
// Sets using_hw=true when a hardware decoder was found.
static const AVCodec* find_best_decoder(AVCodecID id, bool& using_hw) {
    using_hw = false;
    if (g_nvdecAvailable) {
        const char* name = get_cuvid_name(id);
        if (name) {
            const AVCodec* hw = avcodec_find_decoder_by_name(name);
            if (hw) { using_hw = true; return hw; }
        }
    }
    return avcodec_find_decoder(id);
}

// ------------------------------ Encode Thread ------------------------------
struct EncodeArgs {
    char   inPath[MAX_PATH];
    char   outPath[MAX_PATH];
    double targetSizeMB;
    int    scaleFactor;
    int    origW, origH;
    double startSecs, endSecs;
    int    selAudio, selSubs;
    bool   convertHdrToSdr;
    HWND   hwnd;
    char   extSubPath[MAX_PATH]; // external subtitle file (empty = none)
};

static unsigned __stdcall EncodeThreadProc(void* param) {
    EncodeArgs* args = (EncodeArgs*)param;
    g_encodeProgress = 0.0f;
    StringCchCopyA(g_encodeOutPath, MAX_PATH, args->outPath);
    bool ok = TranscodeWithSizeAndScale(
        args->inPath, args->outPath, args->targetSizeMB,
        args->scaleFactor, args->origW, args->origH,
        args->startSecs, args->endSecs,
        args->selAudio, args->selSubs, args->convertHdrToSdr,
        args->extSubPath[0] ? args->extSubPath : nullptr);
    g_encodeProgress = ok ? 1.0f : 0.0f;
    g_encodeRunning  = false;
    PostMessage(args->hwnd, WM_APP_ENCODE_DONE, ok ? 1 : 0, 0);
    delete args;
    return 0;
}

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
        const int gotoW = 38;
        int bx = ix;
        MoveWindow(g_hBtnPlayPause,   bx, iy, btnW,  btnH, TRUE); bx += btnW + 6;
        MoveWindow(g_hBtnBack,        bx, iy, btnW,  btnH, TRUE); bx += btnW + 6;
        MoveWindow(g_hBtnFwd,         bx, iy, btnW,  btnH, TRUE); bx += btnW + 22;
        MoveWindow(g_hBtnMarkIn,      bx, iy, btnW,  btnH, TRUE); bx += btnW + 4;
        MoveWindow(g_hBtnGotoMarkIn,  bx, iy, gotoW, btnH, TRUE); bx += gotoW + 10;
        MoveWindow(g_hBtnMarkOut,     bx, iy, btnW,  btnH, TRUE); bx += btnW + 4;
        MoveWindow(g_hBtnGotoMarkOut, bx, iy, gotoW, btnH, TRUE);
        iy += btnH + 6;
        MoveWindow(g_hTimeline, ix, iy, avail, seekH, TRUE);
    }
    y += playerH + M;

    // === Start Processing button — very bottom ===
    MoveWindow(g_hStartButton, M, y, totalW, 32, TRUE);
    g_videoTop = y + 32 + M;  // video preview starts below the start button

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
    InitializeCriticalSection(&g_csMarkCache);

    ShowWindow(g_mainHwnd, nCmdShow);
    UpdateWindow(g_mainHwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DeleteCriticalSection(&g_csState);
    DeleteCriticalSection(&g_csMarkCache);
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

    // Off-screen double buffer: all drawing goes to memDC; a single BitBlt
    // at the end copies the finished image to the screen with no visible
    // intermediate state, eliminating the erase→redraw flicker.
    HDC     memDC  = CreateCompatibleDC(dis->hDC);
    HBITMAP memBmp = CreateCompatibleBitmap(dis->hDC, W, H);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    // Erase background on the off-screen surface (corners outside pill shapes)
    RECT rc = { 0, 0, W, H };
    FillRect(memDC, &rc, g_hBkBrush ? g_hBkBrush : GetSysColorBrush(COLOR_WINDOW));

    {   // Scope so Graphics is destroyed (flushing to memDC) before the blit
        Gdiplus::Graphics g(memDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        float bx = 1.0f, by = 1.0f;
        float bw = (float)W - 2.0f, bh = (float)H - 2.0f;

    // ---- Start Processing button — teal idle / red+green progress ----
    if (id == IDC_START_BUTTON) {
        float r = 8.0f;
        Gdiplus::GraphicsPath path;
        path.AddArc(bx,           by, r*2.0f, bh, 90.0f, 180.0f);
        path.AddArc(bx+bw-r*2.0f, by, r*2.0f, bh, 270.0f, 180.0f);
        path.CloseFigure();

        float prog = g_encodeProgress;  // 0.0..1.0
        bool inProgress = g_encodeRunning || (prog > 0.0f && prog < 1.0f);
        bool done       = (!g_encodeRunning && prog >= 1.0f);

        if (inProgress || done) {
            // Red background pill
            Gdiplus::Color bgTop(255, 130, 20, 20);
            Gdiplus::Color bgBot(255, 100, 14, 14);
            Gdiplus::LinearGradientBrush bgBrush(
                Gdiplus::PointF(0.0f, by), Gdiplus::PointF(0.0f, by + bh), bgTop, bgBot);
            g.FillPath(&bgBrush, &path);

            // Green fill — clip to left portion of the pill
            float fillW = bw * prog;
            if (fillW > 0.5f) {
                Gdiplus::Region oldClip;
                g.GetClip(&oldClip);
                Gdiplus::RectF fillRect(bx, by, fillW, bh);
                g.SetClip(fillRect);
                Gdiplus::Color gTop(255, 30, 160, 55);
                Gdiplus::Color gBot(255, 20, 125, 42);
                Gdiplus::LinearGradientBrush greenBrush(
                    Gdiplus::PointF(0.0f, by), Gdiplus::PointF(0.0f, by + bh), gTop, gBot);
                g.FillPath(&greenBrush, &path);
                g.SetClip(&oldClip);
            }

            // Subtle border
            Gdiplus::Pen pen(Gdiplus::Color(70, 255, 80, 80), 1.0f);
            g.DrawPath(&pen, &path);

            // Label text
            HFONT hf = g_hFont ? g_hFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            Gdiplus::Font font(memDC, hf);
            Gdiplus::RectF tr(bx, by, bw, bh);
            Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentCenter);
            sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::SolidBrush tb(Gdiplus::Color(235, 255, 255, 255));
            if (done) {
                g.DrawString(L"\u2713\u2006Done", -1, &font, tr, &sf, &tb);
            } else {
                wchar_t label[32];
                StringCchPrintfW(label, 32, L"Processing\u2026 %d%%", (int)(prog * 100.0f));
                g.DrawString(label, -1, &font, tr, &sf, &tb);
            }
        } else {
            // Idle / normal teal button
            Gdiplus::Color c1, c2;
            if      (disabled) { c1 = Gdiplus::Color(255, 34, 50, 48);   c2 = Gdiplus::Color(255, 26, 38, 36); }
            else if (pressed)  { c1 = Gdiplus::Color(255, 16, 108, 98);  c2 = Gdiplus::Color(255, 12, 84, 76); }
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
            Gdiplus::Font font(memDC, hf);
            Gdiplus::RectF tr(bx, by, bw, bh);
            Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentCenter);
            sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            g.DrawString(L"\u25B6\u2006Start Processing", -1, &font, tr, &sf, &tb);
        }
    }

    // ---- Resolution dropdown button ----
    else if (id == IDC_RES_DROPDOWN) {
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
        Gdiplus::Font font(memDC, hf);
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
    }

    // ---- Player icon buttons — pill ----
    else {
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
    else if (id == IDC_BTN_GOTO_MARKIN) {
        // ←| jump to mark-in: left-pointing arrow → vertical bar (violet)
        Gdiplus::Color col(a, 155, 95, 255);
        Gdiplus::SolidBrush b(col);
        Gdiplus::Pen pen(col, 2.5f);
        pen.SetStartCap(Gdiplus::LineCapSquare);
        pen.SetEndCap(Gdiplus::LineCapSquare);
        // vertical bar on right
        g.DrawLine(&pen, cx + 7.0f, cy - 8.0f, cx + 7.0f, cy + 8.0f);
        // left-pointing arrow pointing at the bar
        Gdiplus::PointF pts[3] = { {cx + 3.0f, cy - 6.0f}, {cx + 3.0f, cy + 6.0f}, {cx - 7.0f, cy} };
        g.FillPolygon(&b, pts, 3);
    }
    else if (id == IDC_BTN_GOTO_MARKOUT) {
        // |→ jump to mark-out: vertical bar → right-pointing arrow (hot-pink)
        Gdiplus::Color col(a, 255, 75, 135);
        Gdiplus::SolidBrush b(col);
        Gdiplus::Pen pen(col, 2.5f);
        pen.SetStartCap(Gdiplus::LineCapSquare);
        pen.SetEndCap(Gdiplus::LineCapSquare);
        // vertical bar on left
        g.DrawLine(&pen, cx - 7.0f, cy - 8.0f, cx - 7.0f, cy + 8.0f);
        // right-pointing arrow pointing away from the bar
        Gdiplus::PointF pts[3] = { {cx - 3.0f, cy - 6.0f}, {cx - 3.0f, cy + 6.0f}, {cx + 7.0f, cy} };
        g.FillPolygon(&b, pts, 3);
    }
    }   // end else (player buttons)
    }   // end Graphics scope — GDI+ content flushed to memDC

    // Atomic blit to screen: single operation, no visible intermediate state
    BitBlt(dis->hDC, 0, 0, W, H, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
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
        g_hBtnGotoMarkIn = CreateWindowEx(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 530, 250, 38, 26, hwnd,
            (HMENU)IDC_BTN_GOTO_MARKIN, GetModuleHandle(nullptr), nullptr);
        g_hBtnGotoMarkOut = CreateWindowEx(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 572, 250, 38, 26, hwnd,
            (HMENU)IDC_BTN_GOTO_MARKOUT, GetModuleHandle(nullptr), nullptr);
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
            applyFont(g_hBtnGotoMarkIn);  applyFont(g_hBtnGotoMarkOut);
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
        addTip(g_hBtnMarkIn,       L"Set clip start to current position  [I]");
        addTip(g_hBtnMarkOut,      L"Set clip end to current position  [O]");
        addTip(g_hBtnGotoMarkIn,   L"Jump playhead to mark-in position");
        addTip(g_hBtnGotoMarkOut,  L"Jump playhead to mark-out position");

        // Try to initialise CUDA device for NVDEC hardware decoding.
        TryInitHWDevice();

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

                // Auto-detect external subtitle files (.srt/.ass/.ssa) next to the video
                ScanExternalSubtitles(g_inputPath);
                for (int ei = 0; ei < g_extSubCount; ei++) {
                    const char* fname = strrchr(g_extSubPaths[ei], '\\');
                    if (!fname) fname = strrchr(g_extSubPaths[ei], '/');
                    fname = fname ? fname + 1 : g_extSubPaths[ei];
                    char labelA[MAX_PATH + 16];
                    StringCchPrintfA(labelA, sizeof(labelA), "External: %s", fname);
                    wchar_t labelW[MAX_PATH + 16];
                    MultiByteToWideChar(CP_UTF8, 0, labelA, -1, labelW, MAX_PATH + 16);
                    LRESULT ni = SendMessage(g_hSubsDrop, CB_ADDSTRING, 0, (LPARAM)labelW);
                    // item data -2, -3, ... → index 0, 1, ... into g_extSubPaths
                    SendMessage(g_hSubsDrop, CB_SETITEMDATA, ni, (LPARAM)(INT_PTR)(-(ei + 2)));
                }

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
                g_markInMs  = -1;
                g_markOutMs = -1;
                InvalidateRect(g_hTimeline, nullptr, TRUE);

                EnableWindow(g_hBtnPlayPause, TRUE);
                EnableWindow(g_hBtnFwd, TRUE);
                EnableWindow(g_hBtnBack, TRUE);
                EnableWindow(g_hBtnMarkIn, TRUE);
                EnableWindow(g_hBtnMarkOut, TRUE);
                EnableWindow(g_hBtnGotoMarkIn,  FALSE);
                EnableWindow(g_hBtnGotoMarkOut, FALSE);
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

                // Detect Xvid/MPEG-4 packed B-frames in AVI — these carry stale VOP
                // timestamps from wherever the clip was cut, making position display
                // wildly wrong (e.g. showing 11133 s for a 7420 s file).
                if (IsXvidAvi(g_inputPath)) {
                    int ans = MessageBoxW(hwnd,
                        L"This file is an Xvid/MPEG-4 AVI with packed B-frames.\n\n"
                        L"B-frame timestamps are stored relative to the original recording\n"
                        L"rather than the clip, which corrupts the position display and\n"
                        L"makes in/out point selection unusable.\n\n"
                        L"Would you like to create a corrected MP4 copy?\n"
                        L"This is a fast one-time stream copy \x2014 no re-encoding.",
                        L"Packed B-Frame Timestamps",
                        MB_YESNO | MB_ICONINFORMATION);
                    if (ans == IDYES) {
                        // Build output path: same dir, .mp4 extension.
                        char out_path[MAX_PATH];
                        strcpy_s(out_path, g_inputPath);
                        char* ext = strrchr(out_path, '.');
                        if (ext) *ext = '\0';
                        strcat_s(out_path, ".mp4");
                        // If the .mp4 already exists use _fixed.mp4
                        if (GetFileAttributesA(out_path) != INVALID_FILE_ATTRIBUTES) {
                            strcpy_s(out_path, g_inputPath);
                            char* ext2 = strrchr(out_path, '.');
                            if (ext2) *ext2 = '\0';
                            strcat_s(out_path, "_fixed.mp4");
                        }

                        HCURSOR hWait = LoadCursor(nullptr, IDC_WAIT);
                        HCURSOR hPrev = SetCursor(hWait);
                        bool remux_ok = RemuxXvidToMp4(g_inputPath, out_path);
                        SetCursor(hPrev);

                        if (remux_ok) {
                            wchar_t msg[MAX_PATH + 128];
                            StringCchPrintfW(msg, ARRAYSIZE(msg),
                                L"Done.\n\nFixed file: %S\n\nLoad the new file now?", out_path);
                            if (MessageBoxW(hwnd, msg, L"Remux Complete", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                                strcpy_s(g_inputPath, out_path);
                                // Reload info for the new file and fall through to normal init.
                                GetVideoInfo(g_inputPath, g_vidWidth, g_vidHeight, g_duration);
                                g_tlMax = (int)(g_duration * 1000);
                                InvalidateRect(hwnd, nullptr, TRUE);
                                if (g_hFrameBitmap) { DeleteObject(g_hFrameBitmap); g_hFrameBitmap = nullptr; }
                                g_hFrameBitmap = ExtractMiddleFrameBitmap(g_inputPath, g_vidWidth, g_vidHeight, g_duration);
                                if (g_hFrameBitmap) {
                                    BITMAP bi2; GetObject(g_hFrameBitmap, sizeof(bi2), &bi2);
                                    g_frameWidth = bi2.bmWidth; g_frameHeight = bi2.bmHeight;
                                }
                            }
                        } else {
                            MessageBoxW(hwnd,
                                L"Remux failed. The file may be in use or the destination\n"
                                L"path is not writable.", L"Remux Failed", MB_ICONERROR);
                            if (GetFileAttributesA(out_path) != INVALID_FILE_ATTRIBUTES)
                                DeleteFileA(out_path); // remove partial output
                        }
                    }
                }
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
                StringCchPrintfA(candidate, MAX_PATH, "%s%s_%s.mp4", outDir, fname, suffixA);
                if (_access(candidate, 0) == 0) {
                    for (int i = 1;; i++) {
                        StringCchPrintfA(candidate, MAX_PATH, "%s%s_%s-%d.mp4",
                                         outDir, fname, suffixA, i);
                        if (_access(candidate, 0) != 0) break;
                    }
                }
                StringCchCopyA(outPath, MAX_PATH, candidate);
            }

            // Read user-selected audio and subtitle stream indices
            int  selAudio = -1, selSubs = -1;
            char selExtSubPath[MAX_PATH] = {};
            {
                LRESULT a = SendMessage(g_hAudioDrop, CB_GETCURSEL, 0, 0);
                if (a != CB_ERR) selAudio = (int)(INT_PTR)SendMessage(g_hAudioDrop, CB_GETITEMDATA, a, 0);
                LRESULT s = SendMessage(g_hSubsDrop, CB_GETCURSEL, 0, 0);
                if (s != CB_ERR) {
                    int data = (int)(INT_PTR)SendMessage(g_hSubsDrop, CB_GETITEMDATA, s, 0);
                    if (data >= 0) {
                        selSubs = data;           // embedded stream index
                    } else if (data <= -2) {
                        int extIdx = -(data + 2); // 0-based into g_extSubPaths
                        if (extIdx < g_extSubCount)
                            StringCchCopyA(selExtSubPath, MAX_PATH, g_extSubPaths[extIdx]);
                    }
                    // data == -1: None — leave both at defaults
                }
            }

            bool convertHdrToSdr = false;
            if (g_isHdr) {
                LRESULT colorSel = SendMessage(g_hColorDrop, CB_GETCURSEL, 0, 0);
                convertHdrToSdr = (colorSel == 1);  // index 1 = "Convert to SDR"
            }

            // Launch transcoding on a background thread so the UI stays responsive.
            EnableWindow(g_hStartButton, FALSE);
            StopPlayback();

            EncodeArgs* args = new EncodeArgs{};
            StringCchCopyA(args->inPath,  MAX_PATH, g_inputPath);
            StringCchCopyA(args->outPath, MAX_PATH, outPath);
            args->targetSizeMB   = targetSizeMB;
            args->scaleFactor    = scaleFactor;
            args->origW          = g_vidWidth;
            args->origH          = g_vidHeight;
            args->startSecs      = startSecs;
            args->endSecs        = endSecs;
            args->selAudio       = selAudio;
            args->selSubs        = selSubs;
            args->convertHdrToSdr = convertHdrToSdr;
            args->hwnd           = hwnd;
            StringCchCopyA(args->extSubPath, MAX_PATH, selExtSubPath);

            g_encodeProgress = 0.0f;
            g_encodeRunning  = true;
            InvalidateRect(g_hStartButton, nullptr, TRUE);
            SetTimer(hwnd, IDT_ENCODE_PROGRESS, 100, nullptr);

            if (g_encodeThread) { CloseHandle(g_encodeThread); g_encodeThread = nullptr; }
            g_encodeThread = (HANDLE)_beginthreadex(nullptr, 0, EncodeThreadProc, args, 0, nullptr);
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
        else if (id == IDC_BTN_GOTO_MARKIN && g_playerReady && g_markInMs >= 0) {
            g_tlPos = (int)g_markInMs;
            InvalidateRect(g_hTimeline, nullptr, FALSE);
            g_isGenerating = true;
            EnsureThreadRunningPaused(hwnd);
            SeekMs(g_markInMs, !g_isPlaying);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (id == IDC_BTN_GOTO_MARKOUT && g_playerReady && g_markOutMs >= 0) {
            g_tlPos = (int)g_markOutMs;
            InvalidateRect(g_hTimeline, nullptr, FALSE);
            g_isGenerating = true;
            EnsureThreadRunningPaused(hwnd);
            SeekMs(g_markOutMs, !g_isPlaying);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    }

    case WM_APP_FRAME_READY: {
        g_isGenerating = false;
        if (g_videoTop > 0) {
            RECT rc; GetClientRect(hwnd, &rc);
            rc.top = g_videoTop;
            InvalidateRect(hwnd, &rc, FALSE);
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    }

    case WM_TIMER: {
        if (wParam == IDT_UI_REFRESH) {
            UpdateSeekbarFromPos();
            if (g_isPlaying && g_videoTop > 0) {
                RECT rc; GetClientRect(hwnd, &rc);
                rc.top = g_videoTop;
                InvalidateRect(hwnd, &rc, FALSE);
            }
        }
        if (wParam == IDT_ENCODE_PROGRESS) {
            // Repaint the button to update the progress fill
            InvalidateRect(g_hStartButton, nullptr, FALSE);
            UpdateWindow(g_hStartButton);
        }
        break;
    }

    case WM_APP_ENCODE_DONE: {
        KillTimer(hwnd, IDT_ENCODE_PROGRESS);
        if (g_encodeThread) { CloseHandle(g_encodeThread); g_encodeThread = nullptr; }

        // Show "Done" state briefly, then re-enable
        InvalidateRect(g_hStartButton, nullptr, TRUE);
        UpdateWindow(g_hStartButton);

        bool ok = (wParam != 0);
        if (!ok) {
            MessageBox(hwnd, L"Transcoding failed. See debug output for details.", L"Error", MB_ICONERROR);
        }

        g_encodeProgress = 0.0f;
        g_encodeRunning  = false;
        EnableWindow(g_hStartButton, TRUE);
        InvalidateRect(g_hStartButton, nullptr, TRUE);
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
        if (g_hwDeviceCtx)  { av_buffer_unref(&g_hwDeviceCtx); g_hwDeviceCtx = nullptr; }
        if (g_encodeThread) { CloseHandle(g_encodeThread); g_encodeThread = nullptr; }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ------------------------------ External Subtitle Detection ------------------------------
// Scans for .srt/.ass/.ssa/.sub/.vtt files next to the video with the same base name.
static void ScanExternalSubtitles(const char* videoPath) {
    memset(g_extSubPaths, 0, sizeof(g_extSubPaths));
    g_extSubCount = 0;

    // Split videoPath into directory + base name (without extension)
    char dir[MAX_PATH], base[MAX_PATH];
    StringCchCopyA(dir, MAX_PATH, videoPath);
    char* lastSep = strrchr(dir, '\\');
    if (!lastSep) lastSep = strrchr(dir, '/');
    if (lastSep) {
        StringCchCopyA(base, MAX_PATH, lastSep + 1);
        *(lastSep + 1) = '\0';
    } else {
        StringCchCopyA(base, MAX_PATH, dir);
        dir[0] = '\0';
    }
    char* dot = strrchr(base, '.');
    if (dot) *dot = '\0';

    const char* exts[] = { ".srt", ".ass", ".ssa", ".sub", ".vtt" };
    for (const char* ext : exts) {
        if (g_extSubCount >= 8) break;
        char candidate[MAX_PATH];
        StringCchPrintfA(candidate, MAX_PATH, "%s%s%s", dir, base, ext);
        if (_access(candidate, 0) == 0)
            StringCchCopyA(g_extSubPaths[g_extSubCount++], MAX_PATH, candidate);
    }
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

// ------------------------------ Xvid/packed-B-frame detection & remux ------------------------------

// Returns true if the file is MPEG-4 video inside an AVI container — the condition
// that produces packed B-frames with stale VOP timestamps.
static bool IsXvidAvi(const char* filepath) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, filepath, nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return false; }
    bool result = false;
    if (fmt->iformat && strstr(fmt->iformat->name, "avi")) {
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                fmt->streams[i]->codecpar->codec_id  == AV_CODEC_ID_MPEG4) {
                result = true; break;
            }
        }
    }
    avformat_close_input(&fmt);
    return result;
}

// Stream-copy the AVI to an MP4, applying mpeg4_unpack_bframes to the video
// so every frame gets a proper container-level DTS.  Returns true on success.
static bool RemuxXvidToMp4(const char* in_path, const char* out_path) {
    AVFormatContext* in_fmt  = nullptr;
    AVFormatContext* out_fmt = nullptr;
    AVPacket*        pkt     = av_packet_alloc();
    AVPacket*        bpkt    = av_packet_alloc();
    AVBSFContext*    bsf     = nullptr;
    bool             ok      = false;
    int              vid_idx = -1;

    if (!pkt || !bpkt) goto done;
    if (avformat_open_input(&in_fmt, in_path, nullptr, nullptr) < 0) goto done;
    if (avformat_find_stream_info(in_fmt, nullptr) < 0) goto done;
    if (avformat_alloc_output_context2(&out_fmt, nullptr, nullptr, out_path) < 0) goto done;

    // Map every input stream to the output and find the MPEG-4 video stream.
    for (unsigned i = 0; i < in_fmt->nb_streams; i++) {
        AVStream* is = in_fmt->streams[i];
        AVStream* os = avformat_new_stream(out_fmt, nullptr);
        if (!os) goto done;
        if (avcodec_parameters_copy(os->codecpar, is->codecpar) < 0) goto done;
        os->codecpar->codec_tag = 0; // let the muxer assign the correct tag
        os->time_base = is->time_base;
        if (is->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            is->codecpar->codec_id   == AV_CODEC_ID_MPEG4  && vid_idx < 0) {
            vid_idx = (int)i;
        }
    }

    // Set up mpeg4_unpack_bframes BSF on the video stream.
    if (vid_idx >= 0) {
        const AVBitStreamFilter* f = av_bsf_get_by_name("mpeg4_unpack_bframes");
        if (f && av_bsf_alloc(f, &bsf) >= 0) {
            avcodec_parameters_copy(bsf->par_in, in_fmt->streams[vid_idx]->codecpar);
            bsf->time_base_in = in_fmt->streams[vid_idx]->time_base;
            if (av_bsf_init(bsf) < 0) { av_bsf_free(&bsf); bsf = nullptr; }
        }
    }

    if (!(out_fmt->oformat->flags & AVFMT_NOFILE))
        if (avio_open(&out_fmt->pb, out_path, AVIO_FLAG_WRITE) < 0) goto done;
    if (avformat_write_header(out_fmt, nullptr) < 0) goto done;

    while (av_read_frame(in_fmt, pkt) >= 0) {
        AVStream* is = in_fmt->streams[pkt->stream_index];
        AVStream* os = out_fmt->streams[pkt->stream_index];

        if (pkt->stream_index == vid_idx && bsf) {
            if (av_bsf_send_packet(bsf, pkt) >= 0) {
                while (av_bsf_receive_packet(bsf, bpkt) >= 0) {
                    av_packet_rescale_ts(bpkt, bsf->time_base_out, os->time_base);
                    bpkt->stream_index = vid_idx;
                    av_interleaved_write_frame(out_fmt, bpkt);
                    av_packet_unref(bpkt);
                }
            }
            av_packet_unref(pkt);
        } else {
            av_packet_rescale_ts(pkt, is->time_base, os->time_base);
            av_interleaved_write_frame(out_fmt, pkt);
            av_packet_unref(pkt);
        }
    }

    // Flush the BSF.
    if (bsf) {
        av_bsf_send_packet(bsf, nullptr);
        while (av_bsf_receive_packet(bsf, bpkt) >= 0) {
            av_packet_rescale_ts(bpkt, bsf->time_base_out, out_fmt->streams[vid_idx]->time_base);
            bpkt->stream_index = vid_idx;
            av_interleaved_write_frame(out_fmt, bpkt);
            av_packet_unref(bpkt);
        }
    }

    av_write_trailer(out_fmt);
    ok = true;

done:
    if (bsf)    av_bsf_free(&bsf);
    if (bpkt)   av_packet_free(&bpkt);
    if (pkt)    av_packet_free(&pkt);
    if (out_fmt && !(out_fmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_fmt->pb);
    if (out_fmt) avformat_free_context(out_fmt);
    if (in_fmt)  avformat_close_input(&in_fmt);
    return ok;
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
// Float copy for vectorisable inner loops
static const float k_bt2020_to_bt709f[3][3] = {
    { 1.6605f, -0.5876f, -0.0728f },
    {-0.1246f,  1.1329f, -0.0083f },
    {-0.0182f, -0.1006f,  1.1187f }
};

// ----- Tone-mapping LUTs for fast transcode (built once per encode session) -----
// s_eotf_lut[i] = EOTF(i/65535) — eliminates all pow() calls in Stage 2.
static float   s_eotf_lut[65536]   = {};
// s_srgb_lut16[i] = sRGB_encode(i/65535) as uint8 — eliminates sRGB pow() calls.
static uint8_t s_srgb_lut16[65536] = {};
static bool    s_srgb_lut_ready    = false;

// Call once before each HDR→SDR encode with the source transfer characteristic.
static void BuildToneMappingLuts(bool isPQ) {
    for (int i = 0; i < 65536; i++) {
        double v = i / 65535.0;
        s_eotf_lut[i] = isPQ ? (float)pq_eotf(v) : (float)hlg_eotf(v);
    }
    if (!s_srgb_lut_ready) {
        for (int i = 0; i < 65536; i++)
            s_srgb_lut16[i] = srgb_pack(i / 65535.0);
        s_srgb_lut_ready = true;
    }
}

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
    AVFrame* cpu_frame = nullptr;   // for NVDEC hw→cpu transfer
    int64_t middle_ts = 0;
    uint8_t* rgbBuffer = nullptr;
    HBITMAP hBitmap = nullptr;
    int videoStreamIndex = -1;
    AVStream* videoStream = nullptr;
    const AVCodec* decoder = nullptr;
    bool gotFrame = false;
    bool using_hw = false;
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

    decoder = find_best_decoder(videoStream->codecpar->codec_id, using_hw);
    if (!decoder) goto cleanup;
    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) goto cleanup;
    if (avcodec_parameters_to_context(dec_ctx, videoStream->codecpar) < 0) goto cleanup;
    if (using_hw) {
        dec_ctx->hw_device_ctx = av_buffer_ref(g_hwDeviceCtx);
        dec_ctx->get_format    = get_hw_format;
    }
    if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) goto cleanup;

    middle_ts = (int64_t)((duration / 2.0) * AV_TIME_BASE);
    av_seek_frame(fmt_ctx, -1, middle_ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec_ctx);

    frame = av_frame_alloc();
    rgbFrame = av_frame_alloc();
    if (!frame || !rgbFrame) goto cleanup;
    pkt = av_packet_alloc();
    if (!pkt) goto cleanup;

    // sws_ctx and rgbBuffer are created lazily on the first decoded frame because
    // with NVDEC the pixel format is not known until av_hwframe_transfer_data runs.
    rgbBufSize = av_image_get_buffer_size(AV_PIX_FMT_BGR24, dec_ctx->width, dec_ctx->height, 1);
    rgbBuffer = (uint8_t*)av_malloc(rgbBufSize);
    if (!rgbBuffer) goto cleanup;
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer,
        AV_PIX_FMT_BGR24, dec_ctx->width, dec_ctx->height, 1);

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(dec_ctx, pkt) < 0) { av_packet_unref(pkt); break; }
            if (avcodec_receive_frame(dec_ctx, frame) == 0) {
                // Transfer NVDEC hardware frame to CPU memory if needed.
                AVFrame* sw_frame = frame;
                if (using_hw && frame->format == AV_PIX_FMT_CUDA) {
                    if (!cpu_frame) cpu_frame = av_frame_alloc();
                    if (cpu_frame && av_hwframe_transfer_data(cpu_frame, frame, 0) >= 0) {
                        cpu_frame->color_trc   = frame->color_trc;
                        cpu_frame->colorspace  = frame->colorspace;
                        cpu_frame->color_range = frame->color_range;
                        sw_frame = cpu_frame;
                    }
                }
                // Lazy-init sws_ctx now that we know the actual pixel format.
                if (!sws_ctx) {
                    sws_ctx = sws_getContext(
                        sw_frame->width, sw_frame->height, (AVPixelFormat)sw_frame->format,
                        sw_frame->width, sw_frame->height, AV_PIX_FMT_BGR24,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                }
                bool isPQ  = (sw_frame->color_trc == AVCOL_TRC_SMPTE2084);
                bool isHLG = (sw_frame->color_trc == AVCOL_TRC_ARIB_STD_B67);
                if (isPQ || isHLG) {
                    // Full-quality HDR→SDR: decode → RGB48LE → EOTF+matrix+TM → sRGB
                    int srcCs = (sw_frame->colorspace == AVCOL_SPC_BT2020_NCL ||
                                 sw_frame->colorspace == AVCOL_SPC_BT2020_CL)
                                ? SWS_CS_BT2020 : SWS_CS_ITU709;
                    int srcRange = (sw_frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
                    SwsContext* hdr_sws = sws_getContext(
                        dec_ctx->width, dec_ctx->height, (AVPixelFormat)sw_frame->format,
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
                            sws_scale(hdr_sws, sw_frame->data, sw_frame->linesize,
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
                    if (sws_ctx) {
                        sws_scale(sws_ctx, sw_frame->data, sw_frame->linesize, 0, dec_ctx->height,
                            rgbFrame->data, rgbFrame->linesize);
                        gotFrame = true;
                    }
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
    if (cpu_frame) av_frame_free(&cpu_frame);
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
    AVFrame* cpu_frame = nullptr;   // for NVDEC hw→cpu transfer
    int              videoStreamIndex = -1;
    AVStream* videoStream = nullptr;
    const AVCodec* decoder = nullptr;
    uint8_t* rgbBuffer = nullptr;
    int              rgbBufSize = 0;
    bool             using_hw = false;

    // Audio playback state
    int              audioStreamIdx  = -1;
    AVCodecContext*  aDecCtx         = nullptr;
    SwrContext*      swrCtx          = nullptr;
    HWAVEOUT         hWaveOut        = nullptr;
    AVFrame*         aFrame          = nullptr;
    bool             waveOutIsPaused = false;
    std::deque<WAVEHDR*> waveBlocks;

    // PTS-based video timing
    bool      timingInit     = false;
    ULONGLONG t0Wall         = 0;
    int64_t   t0PtsMs        = 0;
    bool      prevWasPlaying = false; // initialised below after audio init
    int64_t   catchUpToMs    = -1;   // skip frames before this PTS after a seek
    int64_t   vid_play_start = 0;    // stream->start_time offset (AVI/Xvid may be non-zero)
    int64_t   last_vid_dts   = AV_NOPTS_VALUE; // most-recent valid video packet DTS

    bool init_ok = false;
    do {
        if (avformat_open_input(&fmt_ctx, filepath, nullptr, nullptr) < 0) break;
        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) break;
        for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { videoStreamIndex = i; break; }
        }
        if (videoStreamIndex < 0) break;
        videoStream = fmt_ctx->streams[videoStreamIndex];
        // AVI/Xvid: stream->start_time can be non-zero; subtract it from all PTS values
        // so that elapsed time is always measured from the actual start of the file.
        vid_play_start = (videoStream->start_time != AV_NOPTS_VALUE)
                          ? videoStream->start_time : 0;

        decoder = find_best_decoder(videoStream->codecpar->codec_id, using_hw);
        if (!decoder) break;
        dec_ctx = avcodec_alloc_context3(decoder);
        if (!dec_ctx) break;
        if (avcodec_parameters_to_context(dec_ctx, videoStream->codecpar) < 0) break;
        if (using_hw) {
            dec_ctx->hw_device_ctx = av_buffer_ref(g_hwDeviceCtx);
            dec_ctx->get_format    = get_hw_format;
        }
        if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) break;

        // bsf_ctx / bsf_pkt intentionally not used in playback: the mpeg4_unpack_bframes
        // BSF reorders packets in a way that sends B-frames to the decoder before their
        // reference P-frames, causing progressive quality degradation every GOP (~3 s).
        // Wrong VOP timestamps are handled instead by the last_vid_dts fallback below.

        // sws_ctx is created lazily on the first frame so we get the real pixel
        // format after NVDEC hw→cpu transfer (dec_ctx->pix_fmt == CUDA with hw).

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

    // Optional audio init — failure means silent playback, not an error.
    for (unsigned int i = 0; i < fmt_ctx->nb_streams && audioStreamIdx < 0; i++)
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            audioStreamIdx = (int)i;

    if (audioStreamIdx >= 0) {
        AVStream* aStream   = fmt_ctx->streams[audioStreamIdx];
        const AVCodec* aDec = avcodec_find_decoder(aStream->codecpar->codec_id);
        aDecCtx = aDec ? avcodec_alloc_context3(aDec) : nullptr;
        if (aDecCtx && avcodec_parameters_to_context(aDecCtx, aStream->codecpar) >= 0
                    && avcodec_open2(aDecCtx, aDec, nullptr) >= 0) {
            swrCtx = swr_alloc();
        }
        if (swrCtx) {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            av_opt_set_chlayout  (swrCtx, "in_chlayout",    &aDecCtx->ch_layout,   0);
            av_opt_set_int       (swrCtx, "in_sample_rate",  aDecCtx->sample_rate,  0);
            av_opt_set_sample_fmt(swrCtx, "in_sample_fmt",   aDecCtx->sample_fmt,   0);
            av_opt_set_chlayout  (swrCtx, "out_chlayout",   &stereo,                0);
            av_opt_set_int       (swrCtx, "out_sample_rate", aDecCtx->sample_rate,  0);
            av_opt_set_sample_fmt(swrCtx, "out_sample_fmt",  AV_SAMPLE_FMT_S16,     0);
            if (swr_init(swrCtx) < 0) { swr_free(&swrCtx); swrCtx = nullptr; }
        }
        if (swrCtx) {
            WAVEFORMATEX wfx      = {};
            wfx.wFormatTag        = WAVE_FORMAT_PCM;
            wfx.nChannels         = 2;
            wfx.nSamplesPerSec    = (DWORD)aDecCtx->sample_rate;
            wfx.wBitsPerSample    = 16;
            wfx.nBlockAlign       = 4;
            wfx.nAvgBytesPerSec   = (DWORD)(aDecCtx->sample_rate * 4);
            if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR)
                aFrame = av_frame_alloc();
            else
                hWaveOut = nullptr;
        }
        if (!hWaveOut) {
            if (swrCtx)  { swr_free(&swrCtx);           swrCtx  = nullptr; }
            if (aDecCtx) { avcodec_free_context(&aDecCtx); aDecCtx = nullptr; }
            audioStreamIdx = -1;
        }
    }

    prevWasPlaying = g_isPlaying; // avoid spurious pause/resume transition on first tick

    auto doSeek = [&](int64_t toMs) {
        g_stepFileReady = false;
        // Add vid_play_start so the seek target is an absolute stream PTS, not a
        // relative offset.  Without this, files with non-zero start_time (e.g. MP4s
        // produced by h264_nvenc) seek slightly before the intended position.
        int64_t ts = av_rescale_q(toMs, AVRational{ 1,1000 }, videoStream->time_base)
                     + vid_play_start;
        av_seek_frame(fmt_ctx, videoStreamIndex, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec_ctx);
        };

    if (g_seekRequested) { doSeek(g_seekTargetMs); g_seekRequested = false; }
    else { doSeek(g_currentPosMs); }

    while (!g_playThreadShouldExit) {
        if (g_seekRequested) {
            int64_t target = g_seekTargetMs;
            g_seekRequested = false;
            if (hWaveOut) {
                waveOutReset(hWaveOut);
                waveOutIsPaused = false;
                for (WAVEHDR* wh : waveBlocks) {
                    waveOutUnprepareHeader(hWaveOut, wh, sizeof(WAVEHDR));
                    delete[] (uint8_t*)wh->lpData;
                    delete wh;
                }
                waveBlocks.clear();
            }
            if (aDecCtx) avcodec_flush_buffers(aDecCtx);
            timingInit = false;
            doSeek(target);
            catchUpToMs = target;  // discard frames before the actual seek target
        }

        // Pause / resume detection
        {
            bool nowPlaying = g_isPlaying;
            if (prevWasPlaying && !nowPlaying && hWaveOut) {
                waveOutPause(hWaveOut);
                waveOutIsPaused = true;
                timingInit = false;
            }
            if (!prevWasPlaying && nowPlaying && hWaveOut) {
                if (waveOutIsPaused) { waveOutRestart(hWaveOut); waveOutIsPaused = false; }
                timingInit = false;
            }
            prevWasPlaying = nowPlaying;
        }

        // Paused single-frame preview with proper step logic
        if (g_decodeSingleFrame && !g_isPlaying) {
            bool produced = false;
            int64_t targetMs = g_seekTargetMs;
            int64_t bestMs = -1;
            HBITMAP bestBmp = nullptr;

            while (av_read_frame(fmt_ctx, pkt) >= 0) {
                if (pkt->stream_index != videoStreamIndex) { av_packet_unref(pkt); continue; }
                if (pkt->dts != AV_NOPTS_VALUE) last_vid_dts = pkt->dts;
                if (avcodec_send_packet(dec_ctx, pkt) < 0) { av_packet_unref(pkt); break; }

                while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                    // Transfer NVDEC hardware frame to CPU memory if needed.
                    AVFrame* sw_frame = frame;
                    if (using_hw && frame->format == AV_PIX_FMT_CUDA) {
                        if (!cpu_frame) cpu_frame = av_frame_alloc();
                        if (cpu_frame && av_hwframe_transfer_data(cpu_frame, frame, 0) >= 0)
                            sw_frame = cpu_frame;
                    }
                    // Lazy-init sws_ctx on first decoded frame.
                    if (!sws_ctx) {
                        sws_ctx = sws_getContext(
                            sw_frame->width, sw_frame->height, (AVPixelFormat)sw_frame->format,
                            sw_frame->width, sw_frame->height, AV_PIX_FMT_BGR24,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);
                    }
                    if (sws_ctx) {
                        sws_scale(sws_ctx, sw_frame->data, sw_frame->linesize, 0, dec_ctx->height,
                            rgbFrame->data, rgbFrame->linesize);
                    }
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

                    int64_t raw_pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                          ? frame->best_effort_timestamp
                                          : (frame->pts != AV_NOPTS_VALUE ? frame->pts : vid_play_start);
                    int64_t ms = (int64_t)((raw_pts - vid_play_start) *
                        av_q2d(videoStream->time_base) * 1000.0);
                    // If the codec returned a stale VOP timestamp from outside this clip
                    // (e.g. Xvid cut from a long recording), fall back to the container DTS.
                    {
                        int64_t durMs = (int64_t)(g_duration * 1000.0);
                        if (ms < 0 || ms > durMs + 1000) {
                            int64_t dts_pts = (last_vid_dts != AV_NOPTS_VALUE) ? last_vid_dts : vid_play_start;
                            ms = (int64_t)((dts_pts - vid_play_start) * av_q2d(videoStream->time_base) * 1000.0);
                            ms = (ms < 0) ? 0 : (ms > durMs ? durMs : ms);
                        }
                    }

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
                g_stepFileReady = true; // file ptr is now right after decoded frame — next forward step can skip seek
                PostMessage(g_mainHwnd, WM_APP_FRAME_READY, 0, 0);
            }
            Sleep(2);
            continue;
        }

        if (!g_isPlaying) { Sleep(5); continue; }
        g_stepFileReady = false; // playback advances file position — no longer right after a specific frame

        if (av_read_frame(fmt_ctx, pkt) < 0) {
            g_isPlaying = false;
            continue;
        }
        // Audio packet — decode, resample, feed to waveOut
        if (pkt->stream_index == audioStreamIdx && aDecCtx && hWaveOut) {
            // Skip audio during video seek catchup to prevent AV desync:
            // waveOut starts playing the moment the first buffer is written, so
            // if we feed audio before video has caught up to the target PTS the
            // audio will be ahead of the picture by up to one keyframe interval.
            if (catchUpToMs >= 0) { av_packet_unref(pkt); continue; }
            // Reclaim completed buffers to avoid unbounded growth
            while (!waveBlocks.empty() && (waveBlocks.front()->dwFlags & WHDR_DONE)) {
                WAVEHDR* wh = waveBlocks.front();
                waveOutUnprepareHeader(hWaveOut, wh, sizeof(WAVEHDR));
                delete[] (uint8_t*)wh->lpData;
                delete wh;
                waveBlocks.pop_front();
            }
            if (avcodec_send_packet(aDecCtx, pkt) >= 0) {
                while (avcodec_receive_frame(aDecCtx, aFrame) == 0) {
                    int maxSamples = (int)av_rescale_rnd(
                        swr_get_delay(swrCtx, aDecCtx->sample_rate) + aFrame->nb_samples,
                        aDecCtx->sample_rate, aDecCtx->sample_rate, AV_ROUND_UP);
                    uint8_t* pcm = new uint8_t[(size_t)maxSamples * 4];
                    uint8_t* outPtrs[1] = { pcm };
                    int outSamples = swr_convert(swrCtx, outPtrs, maxSamples,
                        (const uint8_t**)aFrame->data, aFrame->nb_samples);
                    if (outSamples > 0) {
                        WAVEHDR* wh  = new WAVEHDR{};
                        wh->lpData        = (LPSTR)pcm;
                        wh->dwBufferLength = (DWORD)(outSamples * 4);
                        wh->dwFlags       = 0;
                        waveOutPrepareHeader(hWaveOut, wh, sizeof(WAVEHDR));
                        waveOutWrite(hWaveOut, wh, sizeof(WAVEHDR));
                        waveBlocks.push_back(wh);
                        pcm = nullptr; // owned by wh
                    }
                    delete[] pcm; // no-op if transferred
                    av_frame_unref(aFrame);
                }
            }
            av_packet_unref(pkt);
            continue;
        }

        if (pkt->stream_index != videoStreamIndex) {
            av_packet_unref(pkt);
            continue;
        }
        if (pkt->dts != AV_NOPTS_VALUE) last_vid_dts = pkt->dts;
        if (avcodec_send_packet(dec_ctx, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        while (avcodec_receive_frame(dec_ctx, frame) == 0) {
            // Transfer NVDEC hardware frame to CPU memory if needed.
            AVFrame* sw_frame = frame;
            if (using_hw && frame->format == AV_PIX_FMT_CUDA) {
                if (!cpu_frame) cpu_frame = av_frame_alloc();
                if (cpu_frame && av_hwframe_transfer_data(cpu_frame, frame, 0) >= 0)
                    sw_frame = cpu_frame;
            }
            // Compute PTS early so we can skip frames still before the seek target.
            int64_t raw_pts2 = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                   ? frame->best_effort_timestamp
                                   : (frame->pts != AV_NOPTS_VALUE ? frame->pts : vid_play_start);
            int64_t ms = (int64_t)((raw_pts2 - vid_play_start) *
                av_q2d(videoStream->time_base) * 1000.0);
            // Guard against stale VOP timestamps from Xvid clips cut from longer recordings.
            {
                int64_t durMs = (int64_t)(g_duration * 1000.0);
                if (ms < 0 || ms > durMs + 1000) {
                    int64_t dts_pts = (last_vid_dts != AV_NOPTS_VALUE) ? last_vid_dts : vid_play_start;
                    ms = (int64_t)((dts_pts - vid_play_start) * av_q2d(videoStream->time_base) * 1000.0);
                    ms = (ms < 0) ? 0 : (ms > durMs ? durMs : ms);
                }
            }
            if (catchUpToMs >= 0 && ms < catchUpToMs) {
                av_frame_unref(frame);
                continue;  // discard — still catching up to the seek target
            }
            catchUpToMs = -1;

            // Lazy-init sws_ctx on first decoded frame.
            if (!sws_ctx) {
                sws_ctx = sws_getContext(
                    sw_frame->width, sw_frame->height, (AVPixelFormat)sw_frame->format,
                    sw_frame->width, sw_frame->height, AV_PIX_FMT_BGR24,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
            }
            if (sws_ctx) {
                sws_scale(sws_ctx, sw_frame->data, sw_frame->linesize, 0, dec_ctx->height,
                    rgbFrame->data, rgbFrame->linesize);
            }
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
                // PTS-based timing: sleep until this frame is due
                if (!timingInit) {
                    t0Wall   = GetTickCount64();
                    t0PtsMs  = ms;
                    timingInit = true;
                } else {
                    int64_t elapsed = (int64_t)(GetTickCount64() - t0Wall);
                    int64_t due     = ms - t0PtsMs;
                    if (due > elapsed + 2)
                        Sleep((DWORD)(due - elapsed));
                }
                EnterCriticalSection(&g_csState);
                if (g_hFrameBitmap) DeleteObject(g_hFrameBitmap);
                g_hFrameBitmap = hNew;
                g_frameWidth   = dec_ctx->width;
                g_frameHeight  = dec_ctx->height;
                g_currentPosMs = ms;
                LeaveCriticalSection(&g_csState);
            }
            PostMessage(g_mainHwnd, WM_APP_FRAME_READY, 0, 0);
            av_frame_unref(frame);
        }
        av_packet_unref(pkt);
    }

    // cleanup — audio first so waveOut stops before we free FFmpeg state
    if (hWaveOut) {
        waveOutReset(hWaveOut);
        for (WAVEHDR* wh : waveBlocks) {
            waveOutUnprepareHeader(hWaveOut, wh, sizeof(WAVEHDR));
            delete[] (uint8_t*)wh->lpData;
            delete wh;
        }
        waveBlocks.clear();
        waveOutClose(hWaveOut);
    }
    if (aFrame)  av_frame_free(&aFrame);
    if (swrCtx)  swr_free(&swrCtx);
    if (aDecCtx) avcodec_free_context(&aDecCtx);

    // video cleanup
    if (pkt) av_packet_free(&pkt);
    if (cpu_frame) av_frame_free(&cpu_frame);
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
    int64_t durMs = (int64_t)(g_duration * 1000.0);
    int64_t target = min(g_currentPosMs + frameMs, durMs);

    if (g_stepFileReady) {
        // Thread file ptr is right after the last decoded frame — skip the costly I-frame seek
        // and let the thread read the next packet directly (1 frame decode vs entire GOP).
        g_stepFileReady = false;
        g_seekTargetMs = target;
        g_decodeSingleFrame = true;
    } else {
        SeekMs(target, true);
    }
    g_tlPos = (int)target;
    InvalidateRect(g_hTimeline, nullptr, FALSE);
}


void StepBackward(HWND hwnd) {
    EnsureThreadRunningPaused(hwnd);
    g_isPlaying = false;
    g_stepFileReady = false;
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
    g_markInMs = (int64_t)g_currentPosMs;
    EnableWindow(g_hBtnGotoMarkIn, TRUE);
    InvalidateRect(g_hTimeline, nullptr, FALSE);
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
    g_markOutMs = (int64_t)g_currentPosMs;
    EnableWindow(g_hBtnGotoMarkOut, TRUE);
    InvalidateRect(g_hTimeline, nullptr, FALSE);
}

// ------------------------------ Transcode (unchanged) ------------------------------

// Bitmap subtitle types (PGS, VOBSUB) are pre-decoded to these structs and
// alpha-blended directly onto YUV frames; text-based subs go through libavfilter/libass.
// x/y/w/h are at OUTPUT (encoder) resolution; yuva holds [Y,Cb,Cr,A] per pixel,
// pre-converted at load time so the per-frame blend loop needs no float math or
// colour conversion.
struct PgsRect  { int x, y, w, h; std::vector<uint8_t> yuva; };
struct PgsEvent { int64_t pts_ms;  std::vector<PgsRect> rects; }; // rects.empty() = clear screen
// Text subtitle event decoded from SUBTITLE_ASS / SUBTITLE_TEXT rects (fallback path)
struct TextSubEvent { double start_s, end_s; std::string ass_text; };

bool TranscodeWithSizeAndScale(const char* in_filename, const char* out_filename, double target_size_mb,
    int scale_factor, int orig_w, int orig_h, double start_seconds, double end_seconds,
    int audio_stream_index, int subtitle_stream_index, bool convert_hdr_to_sdr,
    const char* ext_subtitle_path) {
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
    char              ext_sub_tmp[MAX_PATH]        = {}; // temp copy of the subtitle file at a plain ASCII path
    char              ext_sub_filter_path[MAX_PATH] = {}; // path used in filter (for lazy format-mismatch reinit)
    bool              ext_sub_fmt_fixed             = false; // true after one lazy reinit attempt
    AVFrame*          pre_filter_frame              = nullptr; // YUV420P staging frame for subtitle filter
    SwsContext*       pre_filter_sws                = nullptr; // NV12/P010/etc → YUV420P for filter input

    int               pgs_plane_w      = 0;   // subtitle coordinate space (may differ from video for 4K)
    int               pgs_plane_h      = 0;
    std::vector<PgsEvent>    pgs_events;
    std::vector<TextSubEvent> text_sub_events;
    int               videoStreamIndex = -1;
    int               audioStreamIndex = -1;
    AVFrame*          frame            = nullptr;
    AVFrame*          filt_frame       = nullptr;
    AVFrame*          cpu_frame        = nullptr;   // for NVDEC hw→cpu transfer
    AVPacket*         pkt              = nullptr;
    AVPacket*         enc_pkt          = nullptr;
    bool              success          = false;
    bool              using_hw         = false;
    int64_t           video_start_pts  = 0;
    int64_t           audio_start_pts  = 0;
    int64_t           vid_stream_start = 0;  // stream->start_time for video (AVI/Xvid offset)
    int64_t           aud_stream_start = 0;  // stream->start_time for audio
    int64_t           last_vid_pkt_dts = AV_NOPTS_VALUE; // most-recent valid video packet DTS
    AVBSFContext*     trans_bsf_ctx    = nullptr; // mpeg4_unpack_bframes for packed-B AVIs
    AVPacket*         trans_bsf_pkt    = nullptr;
    AVCodecContext*   aDec_ctx         = nullptr;
    AVCodecContext*   aEnc_ctx         = nullptr;
    SwrContext*       aSwrCtx          = nullptr;
    AVFrame*          aFrame           = nullptr;
    AVFrame*          aEncFrame        = nullptr;
    AVPacket*         aEncPkt          = nullptr;
    int64_t           aOutPts          = 0;

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

    // Stream start_time offsets: AVI/Xvid (and some other containers) can have a
    // non-zero start_time so that the first frame's PTS is not 0.  All timestamp
    // arithmetic must subtract this offset to get elapsed-seconds-from-file-start.
    vid_stream_start = (video_in_stream->start_time != AV_NOPTS_VALUE)
                        ? video_in_stream->start_time : 0;
    aud_stream_start = (audio_in_stream && audio_in_stream->start_time != AV_NOPTS_VALUE)
                        ? audio_in_stream->start_time : 0;

    // Bitrate calculation: subtract audio and a safety margin so the encoder's CBR
    // overshoot and container overhead never push the file over the target size.
    // 5% overhead absorbs: ~1-2% MP4 container (moov/stbl index tables) + 3-4%
    // NVENC CBR overshoot, which is content-dependent and causes occasional oversize.
    {
        int64_t total_bits    = (int64_t)(target_size_mb * 8.0 * 1024.0 * 1024.0);
        int64_t audio_bitrate = audio_in_stream ? 192000 : 0; // always encode stereo AAC @ 192 kbps
        int64_t audio_bits  = (int64_t)(audio_bitrate * segment_duration);
        int64_t overhead    = (int64_t)(total_bits * 0.05); // 5% covers container + encoder overshoot
        int64_t video_bits  = total_bits - audio_bits - overhead;
        if (video_bits <= 0) video_bits = total_bits / 2;  // audio alone exceeds budget; give video 50%
        target_bitrate = (int64_t)(video_bits / segment_duration);
    }
    if (target_bitrate <= 0) { OutputDebugStringA("Invalid target bitrate calculated.\n"); goto cleanup; }

    video_decoder = find_best_decoder(video_in_stream->codecpar->codec_id, using_hw);
    if (!video_decoder) { OutputDebugStringA("Video decoder not found.\n"); goto cleanup; }
    dec_ctx = avcodec_alloc_context3(video_decoder);
    if (!dec_ctx) { OutputDebugStringA("Failed to allocate video decoder context.\n"); goto cleanup; }
    if (avcodec_parameters_to_context(dec_ctx, video_in_stream->codecpar) < 0) { OutputDebugStringA("Failed to copy video params to decoder.\n"); goto cleanup; }
    if (using_hw) {
        dec_ctx->hw_device_ctx = av_buffer_ref(g_hwDeviceCtx);
        dec_ctx->get_format    = get_hw_format;
    }
    if (avcodec_open2(dec_ctx, video_decoder, nullptr) < 0) { OutputDebugStringA("Failed to open video decoder.\n"); goto cleanup; }

    // Apply mpeg4_unpack_bframes BSF for packed-B-frame Xvid/DivX AVIs.
    if (video_in_stream->codecpar->codec_id == AV_CODEC_ID_MPEG4) {
        const AVBitStreamFilter* tbsf = av_bsf_get_by_name("mpeg4_unpack_bframes");
        if (tbsf && av_bsf_alloc(tbsf, &trans_bsf_ctx) >= 0) {
            avcodec_parameters_copy(trans_bsf_ctx->par_in, video_in_stream->codecpar);
            trans_bsf_ctx->time_base_in = video_in_stream->time_base;
            if (av_bsf_init(trans_bsf_ctx) < 0) {
                av_bsf_free(&trans_bsf_ctx); trans_bsf_ctx = nullptr;
            }
        }
    }
    trans_bsf_pkt = trans_bsf_ctx ? av_packet_alloc() : nullptr;

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
    // Use YUV420P for both h264_nvenc and libx264.  h264_nvenc accepts YUV420P
    // and converts to NV12 internally; this avoids semi-planar UV confusion when
    // the decoded frame (NV12 from NVDEC) is scaled or filtered.
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    if (strcmp(video_encoder->name, "h264_nvenc") == 0) {
        av_opt_set(enc_ctx->priv_data, "preset", "p4",  0);
        // vbr + explicit maxrate enforces the ceiling more accurately than cbr
        // across all NVENC driver versions; cbr can overshoot by 3-8% on
        // complex content because the driver-side rate controller isn't HRD-exact.
        av_opt_set(enc_ctx->priv_data, "rc", "vbr", 0);
        // h264_nvenc ignores enc_ctx->max_b_frames; force B-frames off via its own option.
        // Without this the encoder adds a ~3-frame DTS offset that shifts the output
        // video start by ~0.1 s relative to the input, causing AV/timestamp drift.
        av_opt_set(enc_ctx->priv_data, "bf", "0", 0);
    } else {
        av_opt_set(enc_ctx->priv_data, "preset",  "medium", 0);
        av_opt_set(enc_ctx->priv_data, "nal-hrd", "cbr",    0);
    }
    // Propagate input colour-space metadata so players decode with the right matrix/range.
    if (!convert_hdr_to_sdr) {
        enc_ctx->color_range     = video_in_stream->codecpar->color_range;
        enc_ctx->color_primaries = video_in_stream->codecpar->color_primaries;
        enc_ctx->color_trc       = video_in_stream->codecpar->color_trc;
        enc_ctx->colorspace      = video_in_stream->codecpar->color_space;
    }
    {
        // Prefer codec framerate, fall back to stream's avg_frame_rate, then 30 fps
        AVRational fps = dec_ctx->framerate;
        if (fps.num <= 0 || fps.den <= 0) fps = video_in_stream->avg_frame_rate;
        if (fps.num <= 0 || fps.den <= 0) fps = { 30, 1 };
        enc_ctx->time_base    = av_inv_q(fps);
        enc_ctx->max_b_frames = 0;  // no B-frames → DTS always == PTS → clean seeking
        enc_ctx->gop_size     = max(1, (int)(av_q2d(fps) * 2.0)); // keyframe every ~2 s
    }
    enc_ctx->bit_rate       = target_bitrate;
    enc_ctx->rc_max_rate    = target_bitrate;
    enc_ctx->rc_buffer_size = target_bitrate * 2; // 2-second VBV window for smoother rate control
    if (out_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(enc_ctx, video_encoder, nullptr) < 0) { OutputDebugStringA("Could not open video encoder.\n"); goto cleanup; }
    if (avcodec_parameters_from_context(video_out_stream->codecpar, enc_ctx) < 0) { OutputDebugStringA("Failed to copy encoder params to output.\n"); goto cleanup; }
    video_out_stream->time_base = enc_ctx->time_base;

    if (audio_in_stream) {
        audio_out_stream = avformat_new_stream(out_fmt_ctx, nullptr);
        bool audioOk = false;
        if (audio_out_stream) {
            const AVCodec* aDec = avcodec_find_decoder(audio_in_stream->codecpar->codec_id);
            aDec_ctx = aDec ? avcodec_alloc_context3(aDec) : nullptr;
            if (aDec_ctx && avcodec_parameters_to_context(aDec_ctx, audio_in_stream->codecpar) >= 0
                         && avcodec_open2(aDec_ctx, aDec, nullptr) >= 0) {
                const AVCodec* aEnc = avcodec_find_encoder(AV_CODEC_ID_AAC);
                aEnc_ctx = aEnc ? avcodec_alloc_context3(aEnc) : nullptr;
                if (aEnc_ctx) {
                    int outRate = aDec_ctx->sample_rate > 48000 ? 48000 : aDec_ctx->sample_rate;
                    aEnc_ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
                    aEnc_ctx->sample_rate = outRate;
                    aEnc_ctx->bit_rate    = 192000;
                    aEnc_ctx->time_base   = { 1, outRate };
                    AVChannelLayout stereoLayout = AV_CHANNEL_LAYOUT_STEREO;
                    av_channel_layout_copy(&aEnc_ctx->ch_layout, &stereoLayout);
                    if (out_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) aEnc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                    if (avcodec_open2(aEnc_ctx, aEnc, nullptr) >= 0
                     && avcodec_parameters_from_context(audio_out_stream->codecpar, aEnc_ctx) >= 0) {
                        audio_out_stream->time_base = aEnc_ctx->time_base;
                        aSwrCtx = swr_alloc();
                        if (aSwrCtx) {
                            av_opt_set_chlayout  (aSwrCtx, "in_chlayout",    &aDec_ctx->ch_layout, 0);
                            av_opt_set_int       (aSwrCtx, "in_sample_rate",  aDec_ctx->sample_rate, 0);
                            av_opt_set_sample_fmt(aSwrCtx, "in_sample_fmt",   aDec_ctx->sample_fmt,  0);
                            av_opt_set_chlayout  (aSwrCtx, "out_chlayout",   &stereoLayout,         0);
                            av_opt_set_int       (aSwrCtx, "out_sample_rate", outRate,               0);
                            av_opt_set_sample_fmt(aSwrCtx, "out_sample_fmt",  AV_SAMPLE_FMT_FLTP,   0);
                            if (swr_init(aSwrCtx) >= 0) {
                                aFrame    = av_frame_alloc();
                                aEncFrame = av_frame_alloc();
                                aEncPkt   = av_packet_alloc();
                                if (aEncFrame) {
                                    aEncFrame->nb_samples  = aEnc_ctx->frame_size;
                                    aEncFrame->format      = AV_SAMPLE_FMT_FLTP;
                                    aEncFrame->sample_rate = outRate;
                                    av_channel_layout_copy(&aEncFrame->ch_layout, &aEnc_ctx->ch_layout);
                                    av_frame_get_buffer(aEncFrame, 0);
                                }
                                audioOk = aFrame && aEncFrame && aEncPkt;
                            }
                        }
                    }
                }
            }
        }
        if (!audioOk) {
            OutputDebugStringA("Audio encode setup failed; output will have no audio.\n");
            if (aSwrCtx)   { swr_free(&aSwrCtx);              aSwrCtx   = nullptr; }
            if (aDec_ctx)  { avcodec_free_context(&aDec_ctx); aDec_ctx  = nullptr; }
            if (aEnc_ctx)  { avcodec_free_context(&aEnc_ctx); aEnc_ctx  = nullptr; }
            if (aFrame)    { av_frame_free(&aFrame);           aFrame    = nullptr; }
            if (aEncFrame) { av_frame_free(&aEncFrame);        aEncFrame = nullptr; }
            if (aEncPkt)   { av_packet_free(&aEncPkt);         aEncPkt   = nullptr; }
            audio_in_stream  = nullptr;
            audio_out_stream = nullptr;
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
        // Convert start_seconds to stream PTS, accounting for the stream's own start_time
        // offset so that seeking and rel-PTS rebasing both work correctly for AVI/Xvid.
        int64_t start_av = (int64_t)llround(start_seconds * AV_TIME_BASE);
        video_start_pts = av_rescale_q(start_av, AV_TIME_BASE_Q, video_tb) + vid_stream_start;

        // Seek the demuxer to (or just before) the requested start on the video stream.
        if (av_seek_frame(in_fmt_ctx, videoStreamIndex, video_start_pts, AVSEEK_FLAG_BACKWARD) < 0) {
            OutputDebugStringA("Warning: could not seek exactly to start time (video).\n");
        }

        // Prepare audio timeline anchor too (if present).
        if (audio_in_stream) {
            AVRational audio_tb = audio_in_stream->time_base;
            audio_start_pts = av_rescale_q(start_av, AV_TIME_BASE_Q, audio_tb) + aud_stream_start;
        }
    }
    avcodec_flush_buffers(dec_ctx);


    frame = av_frame_alloc();
    filt_frame = av_frame_alloc();
    pkt = av_packet_alloc();
    enc_pkt = av_packet_alloc();
    if (!frame || !filt_frame || !pkt || !enc_pkt) { OutputDebugStringA("Could not allocate frame/packet.\n"); goto cleanup; }
    // sws_ctx is created lazily on the first decoded frame because with NVDEC the
    // pixel format (dec_ctx->pix_fmt) is AV_PIX_FMT_CUDA until hw→cpu transfer reveals it.
    filt_frame->format = enc_ctx->pix_fmt;
    filt_frame->width = enc_ctx->width;
    filt_frame->height = enc_ctx->height;
    if (av_frame_get_buffer(filt_frame, 32) < 0) { OutputDebugStringA("Could not allocate buffer for scaled frame.\n"); goto cleanup; }

    if (convert_hdr_to_sdr) {
        // Stage 1 (sws_hdr2rgb) is created lazily on first frame — input pixel format
        // is not known until after hw→cpu transfer.
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
        if (!sws_rgb2yuv || !hdr_rgb48_buf || !hdr_bgr24_buf) {
            OutputDebugStringA("HDR→SDR pre-setup failed; falling back to direct encode.\n");
            convert_hdr_to_sdr = false;
        } else {
            // Pre-build EOTF + sRGB LUTs so Stage 2 uses table lookups instead of pow().
            BuildToneMappingLuts(g_hdrTrc == AVCOL_TRC_SMPTE2084);
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
                          cid == AV_CODEC_ID_XSUB ||
                          cid == AV_CODEC_ID_DVB_SUBTITLE);
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
                                    // PGS codec sets sub_dec->width/height from the PCS segment
                                    // during avcodec_decode_subtitle2, not before. Refresh here
                                    // so the scaling below uses the correct authored plane size
                                    // (e.g. 1920×1080 for 4K Blu-ray with 1080p PGS track).
                                    if (sub_dec->width  > 0) pgs_plane_w = sub_dec->width;
                                    if (sub_dec->height > 0) pgs_plane_h = sub_dec->height;

                                    // AVSubtitle.pts is in microseconds (AV_TIME_BASE)
                                    int64_t s_ms = (sub.pts != AV_NOPTS_VALUE)
                                        ? sub.pts / 1000
                                        : (int64_t)(sub_pkt->pts * av_q2d(in_fmt_ctx->streams[subtitle_stream_index]->time_base) * 1000.0);
                                    PgsEvent ev;
                                    ev.pts_ms = s_ms;
                                    for (unsigned ri = 0; ri < sub.num_rects; ri++) {
                                        AVSubtitleRect* rect = sub.rects[ri];
                                        if (rect->type == SUBTITLE_BITMAP && rect->w > 0 && rect->h > 0) {
                                            // Pre-scale to output resolution and pre-convert
                                            // palette → [Y, Cb, Cr, A].  Doing this once at load
                                            // time removes all float coordinate math and per-pixel
                                            // colour conversion from the per-frame blend loop.
                                            int srcRefW = (pgs_plane_w > 0) ? pgs_plane_w : dec_ctx->width;
                                            int srcRefH = (pgs_plane_h > 0) ? pgs_plane_h : dec_ctx->height;
                                            double psx = (srcRefW > 0) ? (double)enc_ctx->width  / srcRefW : 1.0;
                                            double psy = (srcRefH > 0) ? (double)enc_ctx->height / srcRefH : 1.0;
                                            PgsRect pr;
                                            pr.x = (int)(rect->x * psx);
                                            pr.y = (int)(rect->y * psy);
                                            pr.w = max(1, (int)(rect->w * psx + 0.5));
                                            pr.h = max(1, (int)(rect->h * psy + 0.5));
                                            pr.yuva.resize((size_t)pr.w * pr.h * 4);
                                            uint8_t* pal = rect->data[1]; // BGRA palette (256 × 4 bytes)
                                            for (int dy = 0; dy < pr.h; dy++) {
                                                int sy_s = min(rect->h - 1, (int)((dy + 0.5) * rect->h / pr.h));
                                                for (int dx = 0; dx < pr.w; dx++) {
                                                    int sx_s = min(rect->w - 1, (int)((dx + 0.5) * rect->w / pr.w));
                                                    uint8_t idx = rect->data[0][sy_s * rect->linesize[0] + sx_s];
                                                    uint8_t b = pal[idx * 4 + 0];
                                                    uint8_t g = pal[idx * 4 + 1];
                                                    uint8_t r = pal[idx * 4 + 2];
                                                    uint8_t a = pal[idx * 4 + 3];
                                                    uint8_t* dst = pr.yuva.data() + ((size_t)dy * pr.w + dx) * 4;
                                                    dst[0] = (uint8_t)max(16, min(235, (( 66*r + 129*g +  25*b + 128) >> 8) + 16));
                                                    dst[1] = (uint8_t)max(16, min(240, ((-38*r -  74*g + 112*b + 128) >> 8) + 128));
                                                    dst[2] = (uint8_t)max(16, min(240, ((112*r -  94*g -  18*b + 128) >> 8) + 128));
                                                    dst[3] = a;
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

    // External subtitle file (.srt/.ass/.ssa alongside the video).
    // libass silently fails to open files whose paths contain special characters
    // (spaces, !!, non-ASCII).  Fix: copy the file into a temp path with a plain
    // ASCII name before passing it to the filter.  This preserves all styling
    // (ASS overrides, fonts, etc.) because libass reads the actual file content.
    if (ext_subtitle_path && ext_subtitle_path[0] && !use_bitmap_subs) {
        {
            char tmp_dir[MAX_PATH] = {}, tmp_base[MAX_PATH] = {};
            GetTempPathA(MAX_PATH, tmp_dir);
            if (GetTempFileNameA(tmp_dir, "sub", 0, tmp_base)) {
                DeleteFileA(tmp_base); // GetTempFileName creates a placeholder; replace it
                const char* orig_dot = strrchr(ext_subtitle_path, '.');
                snprintf(ext_sub_tmp, MAX_PATH, "%s%s", tmp_base, orig_dot ? orig_dot : ".srt");
                FILE* fi = nullptr; FILE* fo = nullptr;
                fopen_s(&fi, ext_subtitle_path, "rb");
                fopen_s(&fo, ext_sub_tmp,        "wb");
                if (fi && fo) {
                    char chunk[65536]; size_t n;
                    while ((n = fread(chunk, 1, sizeof(chunk), fi)) > 0) fwrite(chunk, 1, n, fo);
                }
                if (fo) fclose(fo);
                if (fi) fclose(fi);
            }
        }
        // Use the temp copy if it was created, otherwise fall back to the original path.
        const char* sub_path_for_filter = ext_sub_tmp[0] ? ext_sub_tmp : ext_subtitle_path;
        // Store for the lazy format-mismatch reinit in the encode loop.
        StringCchCopyA(ext_sub_filter_path, MAX_PATH, sub_path_for_filter);

        const AVFilter* buffersrc  = avfilter_get_by_name("buffer");
        const AVFilter* buffersink = avfilter_get_by_name("buffersink");
        if (buffersrc && buffersink) {
            filter_graph = avfilter_graph_alloc();
            if (filter_graph) {
                char src_args[256];
                AVRational tb  = video_in_stream->time_base;
                AVRational sar = dec_ctx->sample_aspect_ratio;
                // The buffersrc is configured for YUV420P.  Frames that arrive in a
                // different format (e.g. NV12 from NVDEC) are converted to YUV420P
                // explicitly in the encode loop before being pushed to the filter.
                AVPixelFormat filter_fmt;
                if (using_hw && dec_ctx->hw_frames_ctx)
                    filter_fmt = ((AVHWFramesContext*)dec_ctx->hw_frames_ctx->data)->sw_format;
                else
                    filter_fmt = AV_PIX_FMT_YUV420P;
                snprintf(src_args, sizeof(src_args),
                    "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                    dec_ctx->width, dec_ctx->height, (int)filter_fmt,
                    tb.num, tb.den, sar.num ? sar.num : 1, sar.den ? sar.den : 1);
                int ret = avfilter_graph_create_filter(&buffersrc_ctx,  buffersrc,  "in",  src_args, nullptr, filter_graph);
                if (ret >= 0)
                    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", nullptr,  nullptr, filter_graph);
                if (ret >= 0) {
                    // Build the subtitle filter directly (no avfilter_graph_parse_ptr).
                    // avfilter_graph_parse_ptr consistently returns EINVAL when the path
                    // contains colons (drive letters) even inside single quotes.
                    // Using av_opt_set bypasses all string-escaping issues entirely.
                    char windir[MAX_PATH] = {};
                    GetWindowsDirectoryA(windir, MAX_PATH);
                    char fontspath_raw[MAX_PATH + 8];
                    snprintf(fontspath_raw, sizeof(fontspath_raw), "%s\\Fonts", windir);

                    const AVFilter* sub_filt = avfilter_get_by_name("subtitles");
                    // avfilter_graph_create_filter calls init() immediately (before we can
                    // set options), so filename is null and init rejects it.
                    // Use alloc_filter + av_opt_set + avfilter_init_str instead:
                    // alloc_filter creates the context without calling init().
                    AVFilterContext* sub_ctx = sub_filt
                        ? avfilter_graph_alloc_filter(filter_graph, sub_filt, "sub")
                        : nullptr;
                    if (sub_ctx) {
                        av_opt_set(sub_ctx->priv, "filename", sub_path_for_filter, 0);
                        av_opt_set(sub_ctx->priv, "fontsdir", fontspath_raw, 0);
                        int init_ret = avfilter_init_str(sub_ctx, nullptr);
                        if (init_ret >= 0 &&
                            avfilter_link(buffersrc_ctx, 0, sub_ctx, 0) == 0 &&
                            avfilter_link(sub_ctx, 0, buffersink_ctx, 0) == 0) {
                            ret = avfilter_graph_config(filter_graph, nullptr);
                        } else { ret = (init_ret < 0) ? init_ret : AVERROR(EINVAL); }
                    } else { ret = AVERROR(ENOSYS); }
                    if (ret >= 0) {
                        use_filter = true;
                    } else {
                        // subtitles filter failed (likely no libass) — open the external
                        // subtitle file with avformat, decode events, and render via GDI.
                        avfilter_graph_free(&filter_graph); filter_graph = nullptr;
                        buffersrc_ctx = nullptr; buffersink_ctx = nullptr;

                        AVFormatContext* ext_fmt = nullptr;
                        if (avformat_open_input(&ext_fmt, ext_subtitle_path, nullptr, nullptr) >= 0 &&
                            avformat_find_stream_info(ext_fmt, nullptr) >= 0) {
                            int ext_sub_idx = -1;
                            for (unsigned si = 0; si < ext_fmt->nb_streams; si++) {
                                if (ext_fmt->streams[si]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                                    ext_sub_idx = (int)si; break;
                                }
                            }
                            if (ext_sub_idx >= 0) {
                                AVCodecID ext_cid = ext_fmt->streams[ext_sub_idx]->codecpar->codec_id;
                                const AVCodec* ext_codec = avcodec_find_decoder(ext_cid);
                                AVCodecContext* ext_dec = ext_codec ? avcodec_alloc_context3(ext_codec) : nullptr;
                                if (ext_dec) {
                                    avcodec_parameters_to_context(ext_dec, ext_fmt->streams[ext_sub_idx]->codecpar);
                                    if (avcodec_open2(ext_dec, ext_codec, nullptr) >= 0) {
                                        AVPacket* ext_pkt = av_packet_alloc();
                                        while (ext_pkt && av_read_frame(ext_fmt, ext_pkt) >= 0) {
                                            if (ext_pkt->stream_index == ext_sub_idx) {
                                                AVSubtitle ext_sub2 = {}; int got2 = 0;
                                                avcodec_decode_subtitle2(ext_dec, &ext_sub2, &got2, ext_pkt);
                                                if (got2) {
                                                    double s2_start = (ext_sub2.pts != AV_NOPTS_VALUE)
                                                        ? ext_sub2.pts * 1e-6
                                                        : ext_pkt->pts * av_q2d(ext_fmt->streams[ext_sub_idx]->time_base);
                                                    double s2_end = s2_start + ext_sub2.end_display_time * 1e-3;
                                                    for (unsigned ri = 0; ri < ext_sub2.num_rects; ri++) {
                                                        AVSubtitleRect* r2 = ext_sub2.rects[ri];
                                                        std::string txt2;
                                                        if (r2->type == SUBTITLE_ASS && r2->ass) {
                                                            const char* p2 = r2->ass;
                                                            // FFmpeg ≥6 omits "Dialogue: " prefix from r->ass; the text field is
// then the 9th field (8 commas) instead of the 10th (9 commas).
{ int _skip = (p2[0]=='D'&&p2[1]=='i'&&p2[2]=='a') ? 9 : 8;
  for (int nc = 0; *p2 && nc < _skip; p2++) if (*p2 == ',') nc++; }
                                                            txt2 = p2;
                                                        } else if (r2->type == SUBTITLE_TEXT && r2->text) {
                                                            for (const char* p2 = r2->text; *p2; p2++) {
                                                                if      (*p2 == '{')  txt2 += "\\{";
                                                                else if (*p2 == '\n') txt2 += "\\N";
                                                                else if (*p2 != '\r') txt2 += *p2;
                                                            }
                                                        }
                                                        if (!txt2.empty())
                                                            text_sub_events.push_back({s2_start, s2_end, std::move(txt2)});
                                                    }
                                                    avsubtitle_free(&ext_sub2);
                                                }
                                            }
                                            av_packet_unref(ext_pkt);
                                        }
                                        if (ext_pkt) av_packet_free(&ext_pkt);
                                    }
                                    avcodec_free_context(&ext_dec);
                                }
                            }
                            avformat_close_input(&ext_fmt);
                        }

                        // GDI render collected text events into pgs_events.
                        if (!text_sub_events.empty()) {
                            int frame_w  = enc_ctx->width;
                            int frame_h  = enc_ctx->height;
                            int font_h   = max(14, frame_h / 20);
                            int margin_v = max(12, frame_h / 18);
                            int max_tw   = frame_w * 85 / 100;
                            HDC screen_dc = GetDC(nullptr);
                            HDC mem_dc    = CreateCompatibleDC(screen_dc);
                            BITMAPINFO bmi2 = {};
                            bmi2.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                            bmi2.bmiHeader.biWidth       = frame_w;
                            bmi2.bmiHeader.biHeight      = -frame_h;
                            bmi2.bmiHeader.biPlanes      = 1;
                            bmi2.bmiHeader.biBitCount    = 32;
                            bmi2.bmiHeader.biCompression = BI_RGB;
                            uint32_t* pixels2 = nullptr;
                            HBITMAP hbm2 = CreateDIBSection(screen_dc, &bmi2, DIB_RGB_COLORS,
                                                            (void**)&pixels2, nullptr, 0);
                            ReleaseDC(nullptr, screen_dc);
                            if (hbm2 && pixels2 && mem_dc) {
                                HBITMAP old_bm2 = (HBITMAP)SelectObject(mem_dc, hbm2);
                                HFONT hfont2 = CreateFontA(
                                    -font_h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
                                HFONT old_font2 = (HFONT)SelectObject(mem_dc, hfont2);
                                SetBkMode(mem_dc, TRANSPARENT);
                                auto strip_ass2 = [](const std::string& s) -> std::wstring {
                                    std::string out;
                                    bool in_tag = false;
                                    for (size_t i = 0; i < s.size(); i++) {
                                        if (s[i] == '{') { in_tag = true;  continue; }
                                        if (s[i] == '}') { in_tag = false; continue; }
                                        if (in_tag) continue;
                                        if (s[i] == '\\' && i + 1 < s.size()) {
                                            char n = s[i + 1];
                                            if (n == 'N' || n == 'n') { out += '\n'; i++; continue; }
                                            if (n == 'h')             { out += ' ';  i++; continue; }
                                        }
                                        out += s[i];
                                    }
                                    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
                                        out.pop_back();
                                    int wlen = MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, nullptr, 0);
                                    std::wstring w(wlen, 0);
                                    MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, &w[0], wlen);
                                    if (!w.empty() && w.back() == 0) w.pop_back();
                                    return w;
                                };
                                const uint32_t MAGENTA2 = 0x00FF00FFu;
                                for (const auto& ev : text_sub_events) {
                                    std::wstring wtxt = strip_ass2(ev.ass_text);
                                    if (wtxt.empty()) continue;
                                    RECT measure2 = {0, 0, (LONG)max_tw, 0};
                                    DrawTextW(mem_dc, wtxt.c_str(), -1, &measure2,
                                              DT_CALCRECT | DT_CENTER | DT_WORDBREAK);
                                    int tw = measure2.right - measure2.left;
                                    int th = measure2.bottom - measure2.top;
                                    if (tw <= 0 || th <= 0) continue;
                                    const int outline2 = 2, pad2 = outline2 + 1;
                                    int tx = (frame_w - tw) / 2;
                                    int ty = frame_h - margin_v - th;
                                    if (tx < pad2) tx = pad2;
                                    if (ty < pad2) ty = pad2;
                                    int rx = tx - pad2, ry = ty - pad2;
                                    int rw = tw + pad2 * 2, rh = th + pad2 * 2;
                                    if (rx + rw > frame_w) rw = frame_w - rx;
                                    if (ry + rh > frame_h) rh = frame_h - ry;
                                    if (rw <= 0 || rh <= 0) continue;
                                    for (int dy = 0; dy < rh; dy++)
                                        for (int dx = 0; dx < rw; dx++)
                                            pixels2[(ry + dy) * frame_w + (rx + dx)] = MAGENTA2;
                                    RECT draw_rect2 = {(LONG)tx, (LONG)ty, (LONG)(tx+tw), (LONG)(ty+th)};
                                    SetTextColor(mem_dc, RGB(0, 0, 0));
                                    for (int oy = -outline2; oy <= outline2; oy++)
                                        for (int ox = -outline2; ox <= outline2; ox++) {
                                            if (ox == 0 && oy == 0) continue;
                                            RECT r2b = {draw_rect2.left+ox, draw_rect2.top+oy,
                                                        draw_rect2.right+ox, draw_rect2.bottom+oy};
                                            DrawTextW(mem_dc, wtxt.c_str(), -1, &r2b, DT_CENTER | DT_WORDBREAK);
                                        }
                                    SetTextColor(mem_dc, RGB(255, 255, 255));
                                    DrawTextW(mem_dc, wtxt.c_str(), -1, &draw_rect2, DT_CENTER | DT_WORDBREAK);
                                    PgsRect pg2;
                                    pg2.x = rx; pg2.y = ry; pg2.w = rw; pg2.h = rh;
                                    pg2.yuva.resize((size_t)rw * rh * 4);
                                    for (int dy = 0; dy < rh; dy++) {
                                        for (int dx = 0; dx < rw; dx++) {
                                            uint32_t pv = pixels2[(ry + dy) * frame_w + (rx + dx)];
                                            uint8_t r = (uint8_t)((pv >> 16) & 0xFF);
                                            uint8_t g = (uint8_t)((pv >>  8) & 0xFF);
                                            uint8_t b = (uint8_t)( pv        & 0xFF);
                                            bool is_bg = (r > 200 && g < 30 && b > 200);
                                            uint8_t Y, Cb, Cr, A;
                                            if (is_bg) {
                                                Y = 16; Cb = 128; Cr = 128; A = 0;
                                            } else {
                                                Y  = (uint8_t)(16  + ( 65*r + 129*g +  25*b + 128) / 256);
                                                Cb = (uint8_t)(128 + (-38*r -  74*g + 112*b + 128) / 256);
                                                Cr = (uint8_t)(128 + (112*r -  94*g -  18*b + 128) / 256);
                                                A  = 255;
                                            }
                                            size_t idx = ((size_t)dy * rw + dx) * 4;
                                            pg2.yuva[idx]   = Y;
                                            pg2.yuva[idx+1] = Cb;
                                            pg2.yuva[idx+2] = Cr;
                                            pg2.yuva[idx+3] = A;
                                        }
                                    }
                                    PgsEvent start_ev2;
                                    start_ev2.pts_ms = (int64_t)(ev.start_s * 1000.0);
                                    start_ev2.rects.push_back(std::move(pg2));
                                    pgs_events.push_back(std::move(start_ev2));
                                    PgsEvent end_ev2;
                                    end_ev2.pts_ms = (int64_t)(ev.end_s * 1000.0);
                                    pgs_events.push_back(std::move(end_ev2));
                                }
                                SelectObject(mem_dc, old_font2);
                                DeleteObject(hfont2);
                                SelectObject(mem_dc, old_bm2);
                            }
                            if (hbm2)    DeleteObject(hbm2);
                            if (mem_dc)  DeleteDC(mem_dc);
                            std::sort(pgs_events.begin(), pgs_events.end(),
                                [](const PgsEvent& a, const PgsEvent& b){ return a.pts_ms < b.pts_ms; });
                            use_bitmap_subs = !pgs_events.empty();
                        }
                    }
                } else {
                    avfilter_graph_free(&filter_graph); filter_graph = nullptr;
                }
            }
        }
    }

    // Set up subtitle burn-in filter graph if a subtitle stream was selected
    // (text-based subtitles only — bitmap types handled by use_bitmap_subs above)
    if (subtitle_stream_index >= 0 && !use_bitmap_subs && !use_filter) {
        const AVFilter* buffersrc  = avfilter_get_by_name("buffer");
        const AVFilter* buffersink = avfilter_get_by_name("buffersink");
        if (buffersrc && buffersink) {
            filter_graph = avfilter_graph_alloc();
            if (filter_graph) {
                // Describe the input video stream for the buffer source
                char src_args[256];
                AVRational tb  = video_in_stream->time_base;
                AVRational sar = dec_ctx->sample_aspect_ratio;
                // With NVDEC, dec_ctx->pix_fmt == AV_PIX_FMT_CUDA.  Use the actual
                // software transfer format from the hw_frames_ctx (e.g. NV12) so the
                // buffersrc matches the frames we will actually feed it.  Falling back
                // to the codecpar format (YUV420P) caused a semi-planar / planar
                // mismatch that corrupted chroma and produced a green tint.
                AVPixelFormat filter_fmt;
                if (using_hw && dec_ctx->hw_frames_ctx) {
                    filter_fmt = ((AVHWFramesContext*)dec_ctx->hw_frames_ctx->data)->sw_format;
                } else if (using_hw) {
                    filter_fmt = (AVPixelFormat)video_in_stream->codecpar->format;
                } else {
                    filter_fmt = dec_ctx->pix_fmt;
                }
                snprintf(src_args, sizeof(src_args),
                    "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                    dec_ctx->width, dec_ctx->height, (int)filter_fmt,
                    tb.num, tb.den, sar.num ? sar.num : 1, sar.den ? sar.den : 1);

                int ret = avfilter_graph_create_filter(&buffersrc_ctx,  buffersrc,  "in",  src_args, nullptr, filter_graph);
                if (ret >= 0)
                    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", nullptr,  nullptr, filter_graph);

                if (ret >= 0) {
                    // The subtitles filter si= takes 0-based index among subtitle streams,
                    // not the global stream index stored in the combo box item data.
                    int subtitle_si = 0;
                    for (unsigned int j = 0; j < in_fmt_ctx->nb_streams && (int)j < subtitle_stream_index; j++)
                        if (in_fmt_ctx->streams[j]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
                            subtitle_si++;

                    // Get Windows Fonts dir so libass can initialize its font engine
                    char windir[MAX_PATH] = {};
                    GetWindowsDirectoryA(windir, MAX_PATH);
                    char fontspath_raw[MAX_PATH + 8];
                    snprintf(fontspath_raw, sizeof(fontspath_raw), "%s\\Fonts", windir);

                    // Use avfilter_graph_alloc_filter + av_opt_set instead of
                    // avfilter_graph_parse_ptr: the parse path consistently returns
                    // EINVAL on Windows when the file path contains a drive-letter
                    // colon (e.g. "C:/..."), even inside single quotes.
                    const AVFilter* sub_filt = avfilter_get_by_name("subtitles");
                    AVFilterContext* sub_ctx = sub_filt
                        ? avfilter_graph_alloc_filter(filter_graph, sub_filt, "sub")
                        : nullptr;
                    if (sub_ctx) {
                        av_opt_set    (sub_ctx->priv, "filename",     in_filename,  0);
                        av_opt_set_int(sub_ctx->priv, "stream_index", subtitle_si,  0);
                        av_opt_set    (sub_ctx->priv, "fontsdir",     fontspath_raw, 0);
                        int init_ret = avfilter_init_str(sub_ctx, nullptr);
                        if (init_ret >= 0 &&
                            avfilter_link(buffersrc_ctx, 0, sub_ctx, 0) == 0 &&
                            avfilter_link(sub_ctx, 0, buffersink_ctx, 0) == 0) {
                            ret = avfilter_graph_config(filter_graph, nullptr);
                        } else { ret = (init_ret < 0) ? init_ret : AVERROR(EINVAL); }
                    } else { ret = AVERROR(ENOSYS); }
                    if (ret >= 0) {
                        use_filter = true;
                    } else {
                        // Primary subtitles filter failed — pre-decode subtitle events via
                        // avcodec_decode_subtitle2, write a temp ASS file, and retry.
                        // This handles formats that libass/subtitles-filter doesn't open
                        // directly (e.g. MOV_TEXT in MP4, unusual codec wrappings).
                        avfilter_graph_free(&filter_graph); filter_graph = nullptr;
                        buffersrc_ctx = nullptr; buffersink_ctx = nullptr;

                        // --- decode all subtitle events from the stream ---
                        {
                            AVCodecID sub_cid2 = in_fmt_ctx->streams[subtitle_stream_index]->codecpar->codec_id;
                            const AVCodec* sub_codec2 = avcodec_find_decoder(sub_cid2);
                            AVCodecContext* sub_dec2 = sub_codec2 ? avcodec_alloc_context3(sub_codec2) : nullptr;
                            if (sub_dec2) {
                                avcodec_parameters_to_context(sub_dec2, in_fmt_ctx->streams[subtitle_stream_index]->codecpar);
                                if (avcodec_open2(sub_dec2, sub_codec2, nullptr) >= 0) {
                                    AVFormatContext* sub_fmt2 = nullptr;
                                    if (avformat_open_input(&sub_fmt2, in_filename, nullptr, nullptr) >= 0 &&
                                        avformat_find_stream_info(sub_fmt2, nullptr) >= 0) {
                                        AVPacket* sub_pkt2 = av_packet_alloc();
                                        while (sub_pkt2 && av_read_frame(sub_fmt2, sub_pkt2) >= 0) {
                                            if (sub_pkt2->stream_index == subtitle_stream_index) {
                                                AVSubtitle sub2 = {};
                                                int got2 = 0;
                                                avcodec_decode_subtitle2(sub_dec2, &sub2, &got2, sub_pkt2);
                                                if (got2) {
                                                    double s2_start = (sub2.pts != AV_NOPTS_VALUE)
                                                        ? sub2.pts * 1e-6
                                                        : sub_pkt2->pts * av_q2d(sub_fmt2->streams[subtitle_stream_index]->time_base);
                                                    double s2_end = s2_start + sub2.end_display_time * 1e-3;
                                                    for (unsigned ri = 0; ri < sub2.num_rects; ri++) {
                                                        AVSubtitleRect* r2 = sub2.rects[ri];
                                                        std::string txt2;
                                                        if (r2->type == SUBTITLE_ASS && r2->ass) {
                                                            // Skip commas to reach the Text field (8 for new FFmpeg, 9 for old).
                                                            const char* p2 = r2->ass;
                                                            // FFmpeg ≥6 omits "Dialogue: " prefix from r->ass; the text field is
// then the 9th field (8 commas) instead of the 10th (9 commas).
{ int _skip = (p2[0]=='D'&&p2[1]=='i'&&p2[2]=='a') ? 9 : 8;
  for (int nc = 0; *p2 && nc < _skip; p2++) if (*p2 == ',') nc++; }
                                                            txt2 = p2;
                                                        } else if (r2->type == SUBTITLE_TEXT && r2->text) {
                                                            for (const char* p2 = r2->text; *p2; p2++) {
                                                                if      (*p2 == '{')  txt2 += "\\{";
                                                                else if (*p2 == '\n') txt2 += "\\N";
                                                                else if (*p2 != '\r') txt2 += *p2;
                                                            }
                                                        }
                                                        if (!txt2.empty())
                                                            text_sub_events.push_back({s2_start, s2_end, std::move(txt2)});
                                                    }
                                                    avsubtitle_free(&sub2);
                                                }
                                            }
                                            av_packet_unref(sub_pkt2);
                                        }
                                        if (sub_pkt2) av_packet_free(&sub_pkt2);
                                        avformat_close_input(&sub_fmt2);
                                    }
                                }
                                avcodec_free_context(&sub_dec2);
                            }
                        }

                        // --- GDI text rendering fallback (no libass required) ---
                        // Render decoded text events as YUVA bitmaps and feed into the
                        // existing pgs_events / use_bitmap_subs blending path.
                        if (!text_sub_events.empty()) {
                            int frame_w  = enc_ctx->width;
                            int frame_h  = enc_ctx->height;
                            int font_h   = max(14, frame_h / 20);
                            int margin_v = max(12, frame_h / 18);
                            int max_tw   = frame_w * 85 / 100;

                            HDC screen_dc = GetDC(nullptr);
                            HDC mem_dc    = CreateCompatibleDC(screen_dc);
                            BITMAPINFO bmi = {};
                            bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                            bmi.bmiHeader.biWidth       = frame_w;
                            bmi.bmiHeader.biHeight      = -frame_h; // top-down
                            bmi.bmiHeader.biPlanes      = 1;
                            bmi.bmiHeader.biBitCount    = 32;
                            bmi.bmiHeader.biCompression = BI_RGB;
                            uint32_t* pixels = nullptr;
                            HBITMAP hbm = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS,
                                                           (void**)&pixels, nullptr, 0);
                            ReleaseDC(nullptr, screen_dc);

                            if (hbm && pixels && mem_dc) {
                                HBITMAP old_bm = (HBITMAP)SelectObject(mem_dc, hbm);
                                // NONANTIALIASED_QUALITY ensures pixels are exactly the text
                                // colour or the background key with no blended intermediates.
                                HFONT hfont = CreateFontA(
                                    -font_h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
                                HFONT old_font = (HFONT)SelectObject(mem_dc, hfont);
                                SetBkMode(mem_dc, TRANSPARENT);

                                // Strip ASS override tags; convert \N / \n to newline, \h to space.
                                auto strip_ass = [](const std::string& s) -> std::wstring {
                                    std::string out;
                                    bool in_tag = false;
                                    for (size_t i = 0; i < s.size(); i++) {
                                        if (s[i] == '{') { in_tag = true;  continue; }
                                        if (s[i] == '}') { in_tag = false; continue; }
                                        if (in_tag) continue;
                                        if (s[i] == '\\' && i + 1 < s.size()) {
                                            char n = s[i + 1];
                                            if (n == 'N' || n == 'n') { out += '\n'; i++; continue; }
                                            if (n == 'h')             { out += ' ';  i++; continue; }
                                        }
                                        out += s[i];
                                    }
                                    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
                                        out.pop_back();
                                    int wlen = MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, nullptr, 0);
                                    std::wstring w(wlen, 0);
                                    MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, &w[0], wlen);
                                    if (!w.empty() && w.back() == 0) w.pop_back();
                                    return w;
                                };

                                // Magenta (R=255,G=0,B=255) used as background transparency key.
                                // In a 32-bpp top-down DIB the DWORD layout is 0x00RRGGBB.
                                const uint32_t MAGENTA = 0x00FF00FFu;

                                for (const auto& ev : text_sub_events) {
                                    std::wstring wtxt = strip_ass(ev.ass_text);
                                    if (wtxt.empty()) continue;

                                    // Measure wrapped text dimensions.
                                    RECT measure = {0, 0, (LONG)max_tw, 0};
                                    DrawTextW(mem_dc, wtxt.c_str(), -1, &measure,
                                              DT_CALCRECT | DT_CENTER | DT_WORDBREAK);
                                    int tw = measure.right  - measure.left;
                                    int th = measure.bottom - measure.top;
                                    if (tw <= 0 || th <= 0) continue;

                                    // Position: centred horizontally, above bottom margin.
                                    const int outline = 2;
                                    const int pad     = outline + 1;
                                    int tx = (frame_w - tw) / 2;
                                    int ty = frame_h - margin_v - th;
                                    if (tx < pad) tx = pad;
                                    if (ty < pad) ty = pad;

                                    // Bounding rect including outline padding.
                                    int rx = tx - pad, ry = ty - pad;
                                    int rw = tw + pad * 2, rh = th + pad * 2;
                                    if (rx + rw > frame_w) rw = frame_w - rx;
                                    if (ry + rh > frame_h) rh = frame_h - ry;
                                    if (rw <= 0 || rh <= 0) continue;

                                    // Fill bounding area with magenta key colour.
                                    for (int dy = 0; dy < rh; dy++)
                                        for (int dx = 0; dx < rw; dx++)
                                            pixels[(ry + dy) * frame_w + (rx + dx)] = MAGENTA;

                                    RECT draw_rect = {(LONG)tx, (LONG)ty,
                                                      (LONG)(tx + tw), (LONG)(ty + th)};

                                    // Draw black outline at all surrounding offsets.
                                    SetTextColor(mem_dc, RGB(0, 0, 0));
                                    for (int oy = -outline; oy <= outline; oy++) {
                                        for (int ox = -outline; ox <= outline; ox++) {
                                            if (ox == 0 && oy == 0) continue;
                                            RECT r2 = {draw_rect.left + ox, draw_rect.top + oy,
                                                       draw_rect.right + ox, draw_rect.bottom + oy};
                                            DrawTextW(mem_dc, wtxt.c_str(), -1, &r2,
                                                      DT_CENTER | DT_WORDBREAK);
                                        }
                                    }

                                    // Draw white main text on top.
                                    SetTextColor(mem_dc, RGB(255, 255, 255));
                                    DrawTextW(mem_dc, wtxt.c_str(), -1, &draw_rect,
                                              DT_CENTER | DT_WORDBREAK);

                                    // Convert bounding rect pixels to YUVA (BT.709 limited range).
                                    PgsRect pg;
                                    pg.x = rx; pg.y = ry; pg.w = rw; pg.h = rh;
                                    pg.yuva.resize((size_t)rw * rh * 4);
                                    for (int dy = 0; dy < rh; dy++) {
                                        for (int dx = 0; dx < rw; dx++) {
                                            uint32_t pv = pixels[(ry + dy) * frame_w + (rx + dx)];
                                            uint8_t r = (uint8_t)((pv >> 16) & 0xFF);
                                            uint8_t g = (uint8_t)((pv >>  8) & 0xFF);
                                            uint8_t b = (uint8_t)( pv        & 0xFF);
                                            bool is_bg = (r > 200 && g < 30 && b > 200);
                                            uint8_t Y, Cb, Cr, A;
                                            if (is_bg) {
                                                Y = 16; Cb = 128; Cr = 128; A = 0;
                                            } else {
                                                Y  = (uint8_t)(16  + ( 65*r + 129*g +  25*b + 128) / 256);
                                                Cb = (uint8_t)(128 + (-38*r -  74*g + 112*b + 128) / 256);
                                                Cr = (uint8_t)(128 + (112*r -  94*g -  18*b + 128) / 256);
                                                A  = 255;
                                            }
                                            size_t idx = ((size_t)dy * rw + dx) * 4;
                                            pg.yuva[idx]   = Y;
                                            pg.yuva[idx+1] = Cb;
                                            pg.yuva[idx+2] = Cr;
                                            pg.yuva[idx+3] = A;
                                        }
                                    }

                                    // Add start event (rect visible) and end event (clear screen).
                                    PgsEvent start_ev;
                                    start_ev.pts_ms = (int64_t)(ev.start_s * 1000.0);
                                    start_ev.rects.push_back(std::move(pg));
                                    pgs_events.push_back(std::move(start_ev));

                                    PgsEvent end_ev;
                                    end_ev.pts_ms = (int64_t)(ev.end_s * 1000.0);
                                    pgs_events.push_back(std::move(end_ev));
                                }

                                SelectObject(mem_dc, old_font);
                                DeleteObject(hfont);
                                SelectObject(mem_dc, old_bm);
                            }
                            if (hbm)    DeleteObject(hbm);
                            if (mem_dc) DeleteDC(mem_dc);

                            std::sort(pgs_events.begin(), pgs_events.end(),
                                [](const PgsEvent& a, const PgsEvent& b){ return a.pts_ms < b.pts_ms; });
                            use_bitmap_subs = !pgs_events.empty();
                        }
                    }
                } else {
                    avfilter_graph_free(&filter_graph); filter_graph = nullptr;
                }
            }
        }
    }

    while (av_read_frame(in_fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == videoStreamIndex) {
            if (pkt->dts != AV_NOPTS_VALUE) last_vid_pkt_dts = pkt->dts;
            if (trans_bsf_ctx) {
                if (av_bsf_send_packet(trans_bsf_ctx, pkt) >= 0) {
                    while (av_bsf_receive_packet(trans_bsf_ctx, trans_bsf_pkt) >= 0) {
                        if (trans_bsf_pkt->dts != AV_NOPTS_VALUE) last_vid_pkt_dts = trans_bsf_pkt->dts;
                        avcodec_send_packet(dec_ctx, trans_bsf_pkt);
                        av_packet_unref(trans_bsf_pkt);
                    }
                }
                av_packet_unref(pkt);
            } else {
                if (avcodec_send_packet(dec_ctx, pkt) < 0) { av_packet_unref(pkt); break; }
            }
            while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                int64_t in_pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                    ? frame->best_effort_timestamp
                                    : (frame->pts != AV_NOPTS_VALUE ? frame->pts : vid_stream_start);
                // Subtract stream start_time so in_time is elapsed seconds from the
                // beginning of the file regardless of container offset (AVI/Xvid fix).
                double in_time = (in_pts - vid_stream_start) * av_q2d(video_in_stream->time_base);
                // Guard against stale VOP timestamps (e.g. Xvid clip cut from a long recording).
                if (in_time < -1.0 || in_time > end_seconds + segment_duration) {
                    int64_t dts_fb = (last_vid_pkt_dts != AV_NOPTS_VALUE) ? last_vid_pkt_dts : vid_stream_start;
                    in_pts  = dts_fb;
                    in_time = (in_pts - vid_stream_start) * av_q2d(video_in_stream->time_base);
                }
                if (in_time > end_seconds) { av_frame_unref(frame); goto flush_encoder; }

                // Drop frames that still decode before the requested start
                if (in_time < start_seconds) { av_frame_unref(frame); continue; }
                if (in_time > end_seconds) { av_frame_unref(frame); goto flush_encoder; }

                // Update encode progress for the button's progress bar
                if (end_seconds > start_seconds)
                    g_encodeProgress = (float)((in_time - start_seconds) / (end_seconds - start_seconds));

                // Transfer NVDEC hardware frame to CPU memory if needed.
                AVFrame* sw_frame = frame;
                if (using_hw && frame->format == AV_PIX_FMT_CUDA) {
                    if (!cpu_frame) cpu_frame = av_frame_alloc();
                    if (cpu_frame && av_hwframe_transfer_data(cpu_frame, frame, 0) >= 0) {
                        cpu_frame->pts                  = frame->pts;
                        cpu_frame->best_effort_timestamp = frame->best_effort_timestamp;
                        cpu_frame->color_trc            = frame->color_trc;
                        cpu_frame->colorspace           = frame->colorspace;
                        cpu_frame->color_range          = frame->color_range;
                        sw_frame = cpu_frame;
                    }
                }

                // Lazy-init sws_hdr2rgb (HDR→SDR Stage 1) on first decoded frame.
                if (convert_hdr_to_sdr && !sws_hdr2rgb) {
                    int srcCs    = (dec_ctx->colorspace == AVCOL_SPC_BT2020_NCL ||
                                    dec_ctx->colorspace == AVCOL_SPC_BT2020_CL)
                                   ? SWS_CS_BT2020 : SWS_CS_ITU709;
                    int srcRange = (dec_ctx->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
                    sws_hdr2rgb = sws_getContext(
                        dec_ctx->width, dec_ctx->height, (AVPixelFormat)sw_frame->format,
                        enc_ctx->width, enc_ctx->height, AV_PIX_FMT_RGB48LE,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (sws_hdr2rgb) {
                        sws_setColorspaceDetails(sws_hdr2rgb,
                            sws_getCoefficients(srcCs),         srcRange,
                            sws_getCoefficients(SWS_CS_ITU709), 1,
                            0, 1 << 16, 1 << 16);
                    } else {
                        OutputDebugStringA("HDR→SDR sws_hdr2rgb init failed; falling back.\n");
                        convert_hdr_to_sdr = false;
                    }
                }

                // Rebase video PTS to 0 at start_seconds, then convert to encoder time_base
                int64_t rel_vid_pts = in_pts - video_start_pts;
                if (rel_vid_pts < 0) rel_vid_pts = 0;
                filt_frame->pts = av_rescale_q(rel_vid_pts, video_in_stream->time_base, enc_ctx->time_base);

                // Route through subtitle filter graph if active, otherwise scale directly
                AVFrame* src_frame = sw_frame;
                AVFrame* filter_out = nullptr;
                if (use_filter) {
                    sw_frame->pts = in_pts; // filter needs original pts for subtitle timing

                    // The subtitles filter only accepts planar YUV.  NVDEC gives NV12 (semi-planar),
                    // so convert to YUV420P with a dedicated sws context before pushing to the filter.
                    AVFrame* filt_input = sw_frame;
                    if ((AVPixelFormat)sw_frame->format != AV_PIX_FMT_YUV420P) {
                        if (!pre_filter_sws)
                            pre_filter_sws = sws_getContext(
                                sw_frame->width, sw_frame->height, (AVPixelFormat)sw_frame->format,
                                sw_frame->width, sw_frame->height, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
                        if (!pre_filter_frame) {
                            pre_filter_frame = av_frame_alloc();
                            if (pre_filter_frame) {
                                pre_filter_frame->format = AV_PIX_FMT_YUV420P;
                                pre_filter_frame->width  = sw_frame->width;
                                pre_filter_frame->height = sw_frame->height;
                                av_frame_get_buffer(pre_filter_frame, 32);
                            }
                        }
                        if (pre_filter_sws && pre_filter_frame) {
                            sws_scale(pre_filter_sws,
                                (const uint8_t* const*)sw_frame->data, sw_frame->linesize,
                                0, sw_frame->height,
                                pre_filter_frame->data, pre_filter_frame->linesize);
                            pre_filter_frame->pts = sw_frame->pts;
                            filt_input = pre_filter_frame;
                        }
                    }

                    if (av_buffersrc_add_frame_flags(buffersrc_ctx, filt_input, AV_BUFFERSRC_FLAG_KEEP_REF) >= 0) {
                        filter_out = av_frame_alloc();
                        int fret = filter_out ? av_buffersink_get_frame(buffersink_ctx, filter_out) : AVERROR(ENOMEM);
                        if (fret < 0) { av_frame_free(&filter_out); filter_out = nullptr; }
                        else { src_frame = filter_out; }
                    }
                }
                // Lazy-init sws_ctx (non-HDR path) using the ACTUAL source frame format
                // (post-filter).  Initialising from sw_frame before the filter ran could
                // produce a format mismatch when NVDEC outputs NV12 but the filter graph
                // was configured for YUV420P — causing incorrect chroma (green tint).
                if (!sws_ctx) {
                    sws_ctx = sws_getContext(
                        src_frame->width, src_frame->height, (AVPixelFormat)src_frame->format,
                        enc_ctx->width,   enc_ctx->height,   enc_ctx->pix_fmt,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
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
                    // Stage 2: EOTF (LUT) + BT.2020→BT.709 matrix + Reinhard TM + sRGB (LUT)
                    // Multi-threaded across rows; uses precomputed float LUTs to avoid
                    // per-pixel pow() calls (was ~9 pow() calls/pixel at 4K = billions/sec).
                    {
                        const bool  bPQ   = (g_hdrTrc == AVCOL_TRC_SMPTE2084);
                        const float refWf = bPQ ? 0.0203f : 0.25f;
                        const uint8_t* srcBuf  = hdr_rgb48_buf;
                        uint8_t*       dstBuf  = hdr_bgr24_buf;
                        const int      dstStride = rgb48W * 3;

                        auto process_rows = [&](int rStart, int rEnd) {
                            for (int row = rStart; row < rEnd; row++) {
                                const uint16_t* s = (const uint16_t*)(srcBuf + row * rgb48Stride);
                                uint8_t*        d = dstBuf + row * dstStride;
                                for (int x = 0; x < rgb48W; x++) {
                                    float R = s_eotf_lut[s[x*3+0]];
                                    float G = s_eotf_lut[s[x*3+1]];
                                    float B = s_eotf_lut[s[x*3+2]];
                                    float Ro = k_bt2020_to_bt709f[0][0]*R + k_bt2020_to_bt709f[0][1]*G + k_bt2020_to_bt709f[0][2]*B;
                                    float Go = k_bt2020_to_bt709f[1][0]*R + k_bt2020_to_bt709f[1][1]*G + k_bt2020_to_bt709f[1][2]*B;
                                    float Bo = k_bt2020_to_bt709f[2][0]*R + k_bt2020_to_bt709f[2][1]*G + k_bt2020_to_bt709f[2][2]*B;
                                    if (Ro < 0.0f) Ro = 0.0f;
                                    if (Go < 0.0f) Go = 0.0f;
                                    if (Bo < 0.0f) Bo = 0.0f;
                                    Ro = 2.0f * Ro / (refWf + Ro);
                                    Go = 2.0f * Go / (refWf + Go);
                                    Bo = 2.0f * Bo / (refWf + Bo);
                                    int ir = (int)(Ro * 65535.0f + 0.5f); if (ir > 65535) ir = 65535;
                                    int ig = (int)(Go * 65535.0f + 0.5f); if (ig > 65535) ig = 65535;
                                    int ib = (int)(Bo * 65535.0f + 0.5f); if (ib > 65535) ib = 65535;
                                    d[x*3+0] = s_srgb_lut16[ib];
                                    d[x*3+1] = s_srgb_lut16[ig];
                                    d[x*3+2] = s_srgb_lut16[ir];
                                }
                            }
                        };

                        static const int nWorkers = max(1, min(8, (int)std::thread::hardware_concurrency()));
                        if (nWorkers > 1 && rgb48H >= nWorkers * 4) {
                            int rowsEach = (rgb48H + nWorkers - 1) / nWorkers;
                            std::vector<std::future<void>> futures;
                            futures.reserve(nWorkers);
                            for (int t = 0; t < nWorkers; t++) {
                                int r0 = t * rowsEach;
                                int r1 = min(r0 + rowsEach, rgb48H);
                                if (r0 >= rgb48H) break;
                                futures.push_back(std::async(std::launch::async, process_rows, r0, r1));
                            }
                            for (auto& f : futures) f.get();
                        } else {
                            process_rows(0, rgb48H);
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

                // Alpha-blend PGS bitmap subtitle onto the scaled YUV output frame.
                // Rects are already at output resolution with pre-converted YCbCr values,
                // so this loop contains no float math or colour conversion.
                if (use_bitmap_subs) {
                    int64_t cur_ms = (int64_t)(in_time * 1000.0);
                    const PgsEvent* active = nullptr;
                    for (const auto& ev : pgs_events) {
                        if (ev.pts_ms <= cur_ms) active = &ev;
                        else break;
                    }
                    if (active && !active->rects.empty()) {
                        for (const auto& rect : active->rects) {
                            const uint8_t* yuva = rect.yuva.data();
                            for (int dy = 0; dy < rect.h; dy++) {
                                int fy = rect.y + dy;
                                if (fy < 0 || fy >= enc_ctx->height) continue;
                                uint8_t* Yrow = filt_frame->data[0] + fy * filt_frame->linesize[0];
                                for (int dx = 0; dx < rect.w; dx++) {
                                    int fx = rect.x + dx;
                                    if (fx < 0 || fx >= enc_ctx->width) continue;
                                    const uint8_t* px = yuva + ((size_t)dy * rect.w + dx) * 4;
                                    uint8_t a = px[3];
                                    if (a == 0) continue;
                                    int inv_a = 255 - a;
                                    Yrow[fx] = (uint8_t)((px[0] * a + Yrow[fx] * inv_a) >> 8);
                                    if ((fx & 1) == 0 && (fy & 1) == 0) {
                                        int cy = fy >> 1, cx = fx >> 1;
                                        if (enc_ctx->pix_fmt == AV_PIX_FMT_YUV420P) {
                                            uint8_t* Up = filt_frame->data[1] + cy * filt_frame->linesize[1] + cx;
                                            uint8_t* Vp = filt_frame->data[2] + cy * filt_frame->linesize[2] + cx;
                                            *Up = (uint8_t)((px[1] * a + *Up * inv_a) >> 8);
                                            *Vp = (uint8_t)((px[2] * a + *Vp * inv_a) >> 8);
                                        } else if (enc_ctx->pix_fmt == AV_PIX_FMT_NV12) {
                                            uint8_t* UVp = filt_frame->data[1] + cy * filt_frame->linesize[1] + cx * 2;
                                            UVp[0] = (uint8_t)((px[1] * a + UVp[0] * inv_a) >> 8);
                                            UVp[1] = (uint8_t)((px[2] * a + UVp[1] * inv_a) >> 8);
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

        else if (audio_in_stream && pkt->stream_index == audioStreamIndex
                 && aDec_ctx && aEnc_ctx && aSwrCtx) {
            AVRational in_tb = audio_in_stream->time_base;
            int64_t aud_in_pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts
                                : (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : aud_stream_start;
            double aud_time = (aud_in_pts - aud_stream_start) * av_q2d(in_tb);
            if (aud_time < start_seconds || aud_time > end_seconds) { av_packet_unref(pkt); continue; }

            if (avcodec_send_packet(aDec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(aDec_ctx, aFrame) == 0) {
                    // Feed decoded samples into swr (no output pull yet)
                    swr_convert(aSwrCtx, nullptr, 0,
                                (const uint8_t**)aFrame->extended_data, aFrame->nb_samples);
                    av_frame_unref(aFrame);

                    // Pull complete AAC frames (frame_size = 1024 samples)
                    while (swr_get_out_samples(aSwrCtx, 0) >= aEnc_ctx->frame_size) {
                        av_frame_make_writable(aEncFrame);
                        swr_convert(aSwrCtx, aEncFrame->data, aEnc_ctx->frame_size, nullptr, 0);
                        aEncFrame->pts = aOutPts;
                        aOutPts += aEnc_ctx->frame_size;
                        avcodec_send_frame(aEnc_ctx, aEncFrame);
                        while (avcodec_receive_packet(aEnc_ctx, aEncPkt) == 0) {
                            aEncPkt->stream_index = audio_out_stream->index;
                            av_packet_rescale_ts(aEncPkt, aEnc_ctx->time_base, audio_out_stream->time_base);
                            av_interleaved_write_frame(out_fmt_ctx, aEncPkt);
                            av_packet_unref(aEncPkt);
                        }
                    }
                }
            }
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

    // Flush audio: drain swr remainder (partial frame), then flush encoder
    if (aEnc_ctx && aSwrCtx && audio_out_stream && aEncFrame && aEncPkt) {
        int remaining = swr_get_out_samples(aSwrCtx, 0);
        if (remaining > 0) {
            av_frame_make_writable(aEncFrame);
            int got = swr_convert(aSwrCtx, aEncFrame->data, aEnc_ctx->frame_size, nullptr, 0);
            // zero-pad the rest of the frame so the encoder sees a complete frame
            if (got < aEnc_ctx->frame_size) {
                int ch = aEncFrame->ch_layout.nb_channels;
                for (int c = 0; c < ch; c++)
                    memset(aEncFrame->data[c] + got * sizeof(float), 0,
                           (aEnc_ctx->frame_size - got) * sizeof(float));
            }
            aEncFrame->pts = aOutPts;
            aOutPts += aEnc_ctx->frame_size;
            avcodec_send_frame(aEnc_ctx, aEncFrame);
        }
        avcodec_send_frame(aEnc_ctx, nullptr);
        while (avcodec_receive_packet(aEnc_ctx, aEncPkt) == 0) {
            aEncPkt->stream_index = audio_out_stream->index;
            av_packet_rescale_ts(aEncPkt, aEnc_ctx->time_base, audio_out_stream->time_base);
            av_interleaved_write_frame(out_fmt_ctx, aEncPkt);
            av_packet_unref(aEncPkt);
        }
    }

    av_write_trailer(out_fmt_ctx);
    success = true;



cleanup:
    if (ext_sub_tmp[0]) DeleteFileA(ext_sub_tmp);
    if (pre_filter_frame) av_frame_free(&pre_filter_frame);
    if (pre_filter_sws)   sws_freeContext(pre_filter_sws);
    if (trans_bsf_pkt) av_packet_free(&trans_bsf_pkt);
    if (trans_bsf_ctx) av_bsf_free(&trans_bsf_ctx);
    if (filter_graph) avfilter_graph_free(&filter_graph);
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (sws_hdr2rgb) sws_freeContext(sws_hdr2rgb);
    if (sws_rgb2yuv) sws_freeContext(sws_rgb2yuv);
    if (hdr_rgb48_buf) av_free(hdr_rgb48_buf);
    if (hdr_bgr24_buf) av_free(hdr_bgr24_buf);
    if (cpu_frame) av_frame_free(&cpu_frame);
    if (frame) av_frame_free(&frame);
    if (filt_frame) av_frame_free(&filt_frame);
    if (pkt) av_packet_free(&pkt);
    if (enc_pkt) av_packet_free(&enc_pkt);
    if (dec_ctx)  avcodec_free_context(&dec_ctx);
    if (enc_ctx)  avcodec_free_context(&enc_ctx);
    if (aDec_ctx) avcodec_free_context(&aDec_ctx);
    if (aEnc_ctx) avcodec_free_context(&aEnc_ctx);
    if (aSwrCtx)  swr_free(&aSwrCtx);
    if (aFrame)    av_frame_free(&aFrame);
    if (aEncFrame) av_frame_free(&aEncFrame);
    if (aEncPkt)   av_packet_free(&aEncPkt);
    if (in_fmt_ctx) avformat_close_input(&in_fmt_ctx);
    if (out_fmt_ctx) {
        if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_fmt_ctx->pb);
        avformat_free_context(out_fmt_ctx);
    }
    return success;
}

// ------------------------------ Filmstrip Thumbnail Extraction ------------------------------
// All 21 thumbnails share one persistent format/codec context opened once.
// The seek is skipped for thumbnails whose target time is already past the last
// decoded position — the same "skip the GOP seek" trick as StepForward.
// A pre-drain step at the start of each thumbnail's loop flushes any frames
// left in the decoder from the previous thumbnail, preventing avcodec_send_packet
// from returning EAGAIN and silently dropping packets.

static unsigned __stdcall ThumbExtractThreadProc(void* /*param*/) {
    int tlW = 0;
    if (g_hTimeline) { RECT rc; GetClientRect(g_hTimeline, &rc); tlW = rc.right; }
    const int N = 21;

    // Persistent state — opened once, reused across all N thumbnails.
    AVFormatContext* fmt_ctx   = nullptr;
    AVCodecContext*  dec_ctx   = nullptr;
    SwsContext*      sws_ctx   = nullptr;  // SDR: lazy-init, kept alive across thumbnails
    AVPacket*        pkt       = nullptr;
    AVFrame*         frame     = nullptr;
    AVFrame*         rgbFrame  = nullptr;
    uint8_t*         rgbBuffer = nullptr;
    int              videoIdx  = -1;
    int              dstW = 0, dstH = 90;
    int              srcW = 0, srcH = 0;

    if (avformat_open_input(&fmt_ctx, g_inputPath, nullptr, nullptr) < 0) goto tf_done;
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) goto tf_done;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIdx = (int)i; break;
        }
    }
    if (videoIdx < 0) goto tf_done;
    {
        AVStream* vs = fmt_ctx->streams[videoIdx];
        const AVCodec* dec = avcodec_find_decoder(vs->codecpar->codec_id);
        if (!dec) goto tf_done;
        dec_ctx = avcodec_alloc_context3(dec);
        if (!dec_ctx) goto tf_done;
        if (avcodec_parameters_to_context(dec_ctx, vs->codecpar) < 0) goto tf_done;
        if (avcodec_open2(dec_ctx, dec, nullptr) < 0) goto tf_done;

        srcW = dec_ctx->width; srcH = dec_ctx->height;
        dstW = (srcH > 0) ? (int)((double)srcW / srcH * dstH) : 160;
        if (dstW < 1) dstW = 1;
        if (g_thumbW == 0) { g_thumbW = dstW; g_thumbH = dstH; }

        // sws_ctx created after first frame so frame->format is used rather than
        // dec_ctx->pix_fmt, which can be wrong for 10-bit HEVC before decoding.
        int bufSize = av_image_get_buffer_size(AV_PIX_FMT_BGR24, dstW, dstH, 1);
        rgbBuffer = (uint8_t*)av_malloc(bufSize);
        frame    = av_frame_alloc();
        rgbFrame = av_frame_alloc();
        pkt      = av_packet_alloc();
        if (!rgbBuffer || !frame || !rgbFrame || !pkt) goto tf_done;
        av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer,
            AV_PIX_FMT_BGR24, dstW, dstH, 1);
    }

    for (int i = 0; i < N && !g_thumbThreadStop; i++) {
        // Use the left edge of each slot so the thumbnail shows the first frame
        // of that section — more intuitive than the centre when browsing.
        double t = (double)i / N * g_duration;

        AVStream* vs    = fmt_ctx->streams[videoIdx];
        double    tbase = av_q2d(vs->time_base);
        bool      gotFrame = false;

        // Seek to the keyframe before t and flush the decoder for a clean state.
        // Using the same seek+flush per thumbnail as the original per-call code
        // is the most reliable approach; the open/close savings (×20) are already
        // the dominant win and this keeps the decode logic straightforward.
        av_seek_frame(fmt_ctx, -1, (int64_t)(t * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec_ctx);

        // Original proven send-then-drain pattern.
        while (!g_thumbThreadStop && !gotFrame && av_read_frame(fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index == videoIdx && avcodec_send_packet(dec_ctx, pkt) == 0) {
                while (!gotFrame && avcodec_receive_frame(dec_ctx, frame) == 0) {
                    int64_t pts = frame->best_effort_timestamp;
                    if (pts == AV_NOPTS_VALUE) pts = frame->pts;
                    double fSecs = (pts != AV_NOPTS_VALUE) ? (double)pts * tbase : t;

                    if (fSecs + 0.001 >= t) {
                        gotFrame = true;
                        AVPixelFormat srcFmt = (AVPixelFormat)frame->format;
                        bool isPQ  = (frame->color_trc == AVCOL_TRC_SMPTE2084);
                        bool isHLG = (frame->color_trc == AVCOL_TRC_ARIB_STD_B67);
                        if (isPQ || isHLG) {
                            int srcCs    = (frame->colorspace == AVCOL_SPC_BT2020_NCL ||
                                           frame->colorspace == AVCOL_SPC_BT2020_CL)
                                           ? SWS_CS_BT2020 : SWS_CS_ITU709;
                            int srcRange = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
                            SwsContext* hdr_sws = sws_getContext(
                                srcW, srcH, srcFmt, dstW, dstH, AV_PIX_FMT_RGB48LE,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
                            if (hdr_sws) {
                                sws_setColorspaceDetails(hdr_sws,
                                    sws_getCoefficients(srcCs),    srcRange,
                                    sws_getCoefficients(SWS_CS_ITU709), 1, 0, 1<<16, 1<<16);
                                int      rgb48Stride = dstW * 6;
                                uint8_t* rgb48       = (uint8_t*)av_malloc(dstH * rgb48Stride);
                                if (rgb48) {
                                    uint8_t* d[1]={rgb48}; int s[1]={rgb48Stride};
                                    sws_scale(hdr_sws, frame->data, frame->linesize, 0, srcH, d, s);
                                    const double refW = isPQ ? 0.0203 : 0.25;
                                    for (int y = 0; y < dstH; y++) {
                                        const uint16_t* src16 = (const uint16_t*)(rgb48 + y*rgb48Stride);
                                        uint8_t* dst8 = rgbFrame->data[0] + y*rgbFrame->linesize[0];
                                        for (int x = 0; x < dstW; x++) {
                                            double R=src16[x*3+0]/65535.0, G=src16[x*3+1]/65535.0, B=src16[x*3+2]/65535.0;
                                            if (isPQ){R=pq_eotf(R);G=pq_eotf(G);B=pq_eotf(B);}
                                            else     {R=hlg_eotf(R);G=hlg_eotf(G);B=hlg_eotf(B);}
                                            double Ro=k_bt2020_to_bt709[0][0]*R+k_bt2020_to_bt709[0][1]*G+k_bt2020_to_bt709[0][2]*B;
                                            double Go=k_bt2020_to_bt709[1][0]*R+k_bt2020_to_bt709[1][1]*G+k_bt2020_to_bt709[1][2]*B;
                                            double Bo=k_bt2020_to_bt709[2][0]*R+k_bt2020_to_bt709[2][1]*G+k_bt2020_to_bt709[2][2]*B;
                                            if(Ro<0)Ro=0; if(Go<0)Go=0; if(Bo<0)Bo=0;
                                            Ro=reinhard_tm(Ro,refW); Go=reinhard_tm(Go,refW); Bo=reinhard_tm(Bo,refW);
                                            dst8[x*3+0]=srgb_pack(Bo); dst8[x*3+1]=srgb_pack(Go); dst8[x*3+2]=srgb_pack(Ro);
                                        }
                                    }
                                    av_free(rgb48);
                                } else { gotFrame = false; }
                                sws_freeContext(hdr_sws);
                            } else { gotFrame = false; }
                        } else {
                            // SDR — lazy-init sws_ctx once, reuse across thumbnails.
                            if (!sws_ctx)
                                sws_ctx = sws_getContext(srcW, srcH, srcFmt, dstW, dstH,
                                    AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
                            if (sws_ctx)
                                sws_scale(sws_ctx, frame->data, frame->linesize, 0, srcH,
                                          rgbFrame->data, rgbFrame->linesize);
                            else
                                gotFrame = false;
                        }
                    }
                    av_frame_unref(frame);
                }
            }
            av_packet_unref(pkt);
        }

        if (gotFrame && !g_thumbThreadStop) {
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth       = dstW;
            bmi.bmiHeader.biHeight      = -dstH;
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 24;
            bmi.bmiHeader.biCompression = BI_RGB;
            void* dibBits = nullptr;
            HDC hdc = GetDC(nullptr);
            HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
            ReleaseDC(nullptr, hdc);
            if (bmp && dibBits) {
                int srcRowBytes = dstW * 3;
                int dibStride   = (srcRowBytes + 3) & ~3;  // DWORD-align
                for (int y = 0; y < dstH; y++)
                    memcpy((uint8_t*)dibBits + y * dibStride,
                           rgbFrame->data[0] + y * rgbFrame->linesize[0], srcRowBytes);
                g_thumbs[i] = bmp;
                PostMessage(g_mainHwnd, WM_APP_THUMBS_READY, 0, 0);
            } else {
                if (bmp) DeleteObject(bmp);
            }
        }
    }

tf_done:
    if (sws_ctx)  sws_freeContext(sws_ctx);
    if (frame)    av_frame_free(&frame);
    if (rgbFrame) { if (rgbBuffer) av_free(rgbBuffer); av_frame_free(&rgbFrame); }
    if (pkt)      av_packet_free(&pkt);
    if (dec_ctx)  avcodec_free_context(&dec_ctx);
    if (fmt_ctx)  avformat_close_input(&fmt_ctx);
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

        // Off-screen double buffer: all drawing goes to memDC; a single BitBlt
        // at the end copies the finished image to the screen with no visible
        // intermediate state, eliminating the dark-flash between frames.
        HDC     memDC  = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, W, H);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        // Dark base
        HBRUSH bgBrush = CreateSolidBrush(RGB(18, 18, 22));
        FillRect(memDC, &rc, bgBrush);
        DeleteObject(bgBrush);

        // Draw filmstrip thumbnails
        if (g_thumbW > 0 && g_thumbH > 0 && W > 0) {
            float slotW = (float)W / 21.0f;
            for (int i = 0; i <= 20; i++) {
                if (!g_thumbs[i]) continue;
                int x0 = (int)(i * slotW);
                int x1 = (int)((i + 1) * slotW);
                int sw = max(1, x1 - x0);
                HDC srcDC = CreateCompatibleDC(memDC);
                HBITMAP old = (HBITMAP)SelectObject(srcDC, g_thumbs[i]);
                SetStretchBltMode(memDC, HALFTONE);
                SetBrushOrgEx(memDC, 0, 0, nullptr);
                StretchBlt(memDC, x0, 0, sw, H, srcDC, 0, 0, g_thumbW, g_thumbH, SRCCOPY);
                SelectObject(srcDC, old);
                DeleteDC(srcDC);
            }
        }

        // Thin separator lines between thumbnail slots
        if (g_thumbW > 0 && W > 0) {
            HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
            HPEN oldPen = (HPEN)SelectObject(memDC, sepPen);
            float slotW = (float)W / 21.0f;
            for (int i = 1; i <= 20; i++) {
                int x = (int)(i * slotW);
                MoveToEx(memDC, x, 0, nullptr);
                LineTo(memDC, x, H);
            }
            SelectObject(memDC, oldPen);
            DeleteObject(sepPen);
        }

        // Mark-in / mark-out overlays (darken excluded ranges)
        if (g_tlMax > 0 && W > 0 && (g_markInMs >= 0 || g_markOutMs >= 0)) {
            HDC overDC      = CreateCompatibleDC(memDC);
            HBITMAP overBmp = CreateCompatibleBitmap(memDC, W, H);
            HBITMAP overOld = (HBITMAP)SelectObject(overDC, overBmp);
            HBRUSH darkBrush = CreateSolidBrush(RGB(0, 0, 0));
            RECT overRc = {0, 0, W, H};
            FillRect(overDC, &overRc, darkBrush);
            DeleteObject(darkBrush);
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, 220, 0};
            if (g_markInMs >= 0) {
                int px = (int)((double)g_markInMs / g_tlMax * W);
                if (px > 0) AlphaBlend(memDC, 0, 0, px, H, overDC, 0, 0, px, H, bf);
            }
            if (g_markOutMs >= 0) {
                int px = (int)((double)g_markOutMs / g_tlMax * W);
                if (px < W - 1) AlphaBlend(memDC, px + 1, 0, W - px - 1, H, overDC, px + 1, 0, W - px - 1, H, bf);
            }
            SelectObject(overDC, overOld);
            DeleteObject(overBmp);
            DeleteDC(overDC);
            // Mark-in line (violet, matches button icon colour)
            if (g_markInMs >= 0) {
                int px = (int)((double)g_markInMs / g_tlMax * W);
                HPEN markPen = CreatePen(PS_SOLID, 2, RGB(155, 95, 255));
                HPEN oldPen  = (HPEN)SelectObject(memDC, markPen);
                MoveToEx(memDC, px, 0, nullptr);
                LineTo(memDC, px, H);
                SelectObject(memDC, oldPen);
                DeleteObject(markPen);
            }
            // Mark-out line (hot-pink, matches button icon colour)
            if (g_markOutMs >= 0) {
                int px = (int)((double)g_markOutMs / g_tlMax * W);
                HPEN markPen = CreatePen(PS_SOLID, 2, RGB(255, 75, 135));
                HPEN oldPen  = (HPEN)SelectObject(memDC, markPen);
                MoveToEx(memDC, px, 0, nullptr);
                LineTo(memDC, px, H);
                SelectObject(memDC, oldPen);
                DeleteObject(markPen);
            }
        }

        // White playhead line
        if (g_tlMax > 0 && W > 0) {
            int px = (int)((double)g_tlPos / g_tlMax * W);
            if (px < 0) px = 0; if (px >= W) px = W - 1;
            HPEN headPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HPEN oldPen  = (HPEN)SelectObject(memDC, headPen);
            MoveToEx(memDC, px, 0, nullptr);
            LineTo(memDC, px, H);
            SelectObject(memDC, oldPen);
            DeleteObject(headPen);
        }

        // Atomic blit to screen
        BitBlt(hdc, 0, 0, W, H, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

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
