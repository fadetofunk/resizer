// main.cpp
// A Win32 C++ application that lets you drag‐and‐drop a video file.
// It reads its duration and resolution using FFmpeg, then:
//  1. Displays “Full video” / “Custom range” (all on one line), disabling the start/end edits if “Full video” is checked.
//  2. Offers “Full”, “Half”, “Quarter” resolution.
//  3. Has a “Suffix” textbox (default “RESIZED”) to append to the output filename (with an incremental number if needed).
//  4. Scales the bitrate so the resulting file is approximately the user’s MB target, using NVENC if available else CPU H.264.
//  5. Immediately extracts the exact middle frame of the WHOLE VIDEO (ignoring any custom range) and stores it in g_hFrameBitmap.
//  6. In WM_PAINT, stretches/blits that bitmap with HALFTONE to fill the client area under the filename label, updating on‐the‐fly as you resize.
//
// Build flags (MSVC):
//   cl /EHsc /I"path\to\ffmpeg\include" main.cpp /link /LIBPATH:"path\to\ffmpeg\lib" avformat.lib avcodec.lib avutil.lib swscale.lib comctl32.lib user32.lib gdi32.lib shell32.lib
//

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>
#include <string>
#include <cmath>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

// Control IDs
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

// Globals
static char     g_inputPath[MAX_PATH] = { 0 };
static int      g_vidWidth = 0;
static int      g_vidHeight = 0;
static double   g_duration = 0.0; // in seconds

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

static HBITMAP  g_hFrameBitmap = nullptr;
static int      g_frameWidth = 0;
static int      g_frameHeight = 0;

// Forward declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
bool GetVideoInfo(const char* filepath, int& width, int& height, double& durationSeconds);
HBITMAP ExtractMiddleFrameBitmap(const char* filepath, int orig_w, int orig_h, double duration);
bool TranscodeWithSizeAndScale(
    const char* in_filename,
    const char* out_filename,
    double target_size_mb,
    int scale_factor,
    int orig_w,
    int orig_h,
    double start_seconds,
    double end_seconds
);

// On WM_SIZE, reposition/resize every control, then InvalidateRect to force repaint
void HandleResize(HWND hwnd, int clientW, int clientH) {
    const int margin = 10;

    // 1. “Target size (MB):” label + edit
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

    // 2. “Suffix:” label + edit
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

    // 3. “Full video” / “Custom range” radios + “Start time” + “End time” on one line
    int yRange = ySuffix + labelHeightSuffix + margin;
    MoveWindow(g_hRangeFullRadio, margin, yRange, 120, 20, TRUE);
    MoveWindow(g_hRangeCustomRadio, margin + 140, yRange, 120, 20, TRUE);
    MoveWindow(g_hStartStatic, margin + 280, yRange, 100, 20, TRUE);
    MoveWindow(g_hStartEdit, margin + 380, yRange, 80, 20, TRUE);
    MoveWindow(g_hEndStatic, margin + 480, yRange, 100, 20, TRUE);
    MoveWindow(g_hEndEdit, margin + 580, yRange, 80, 20, TRUE);

    // 4. Resolution radios below
    int yRes = yRange + 30;
    MoveWindow(g_hFullRadio, margin, yRes, 300, 20, TRUE);
    MoveWindow(g_hHalfRadio, margin, yRes + 25, 300, 20, TRUE);
    MoveWindow(g_hQuarterRadio, margin, yRes + 50, 300, 20, TRUE);

    // 5. “Start Processing” button
    int btnY = yRes + 75;
    MoveWindow(g_hStartButton, margin, btnY, 150, 30, TRUE);

    // 6. Filename label (if no bitmap, bottom‐right; if bitmap exists, just under the button)
    if (g_hFrameBitmap) {
        HDC hdc = GetDC(hwnd);
        HFONT hFont = (HFONT)SendMessage(g_hInfoStatic, WM_GETFONT, 0, 0);
        SelectObject(hdc, hFont);
        wchar_t buf[512];
        GetWindowTextW(g_hInfoStatic, buf, ARRAYSIZE(buf));
        SIZE sz;
        GetTextExtentPoint32W(hdc, buf, lstrlenW(buf), &sz);
        ReleaseDC(hwnd, hdc);

        MoveWindow(g_hInfoStatic, margin, btnY + 30 + margin, sz.cx, sz.cy, TRUE);
    }
    else {
        int staticW = 240, staticH = 20;
        int x = clientW - staticW - margin;
        int y = clientH - staticH - margin;
        MoveWindow(g_hInfoStatic, x, y, staticW, staticH, TRUE);
    }

    // Force a repaint so the bitmap is redrawn at the new size
    InvalidateRect(hwnd, nullptr, TRUE);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // FFmpeg registers all formats/codecs automatically; no need for av_register_all() in newer FFmpeg

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES };
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

    HWND hwnd = CreateWindowEx(
        0,
        CLASS_NAME,
        L"FFmpeg Drag‐Drop Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 700,
        nullptr, nullptr, hInstance, nullptr
    );
    if (!hwnd) {
        MessageBox(nullptr, L"Failed to create main window.", L"Error", MB_ICONERROR);
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        DragAcceptFiles(hwnd, TRUE);

        // “Drop a video file onto this window” (initially at top‐left)
        g_hInfoStatic = CreateWindowEx(
            0, L"STATIC", L"Drop a video file onto this window",
            WS_CHILD | WS_VISIBLE,
            10, 10, 240, 20, hwnd,
            (HMENU)IDC_INFO_STATIC, GetModuleHandle(nullptr), nullptr
        );

        // “Target size (MB):”
        g_hSizeStatic = CreateWindowEx(
            0, L"STATIC", L"Target size (MB):",
            WS_CHILD | WS_VISIBLE | WS_DISABLED,
            10, 40, 100, 20, hwnd,
            (HMENU)IDC_SIZE_STATIC, GetModuleHandle(nullptr), nullptr
        );
        g_hSizeEdit = CreateWindowEx(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | ES_NUMBER,
            120, 40, 200, 20, hwnd,
            (HMENU)IDC_SIZE_EDIT, GetModuleHandle(nullptr), nullptr
        );

        // “Suffix:”
        g_hSuffixStatic = CreateWindowEx(
            0, L"STATIC", L"Suffix:",
            WS_CHILD | WS_VISIBLE | WS_DISABLED,
            10, 70, 60, 20, hwnd,
            (HMENU)IDC_SUFFIX_STATIC, GetModuleHandle(nullptr), nullptr
        );
        g_hSuffixEdit = CreateWindowEx(
            WS_EX_CLIENTEDGE, L"EDIT", L"RESIZED",
            WS_CHILD | WS_VISIBLE | WS_DISABLED,
            80, 70, 200, 20, hwnd,
            (HMENU)IDC_SUFFIX_EDIT, GetModuleHandle(nullptr), nullptr
        );

        // “Full video” / “Custom range” radios (disabled until drop)
        g_hRangeFullRadio = CreateWindowEx(
            0, L"BUTTON", L"Full video",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_AUTORADIOBUTTON | WS_GROUP,
            10, 100, 120, 20, hwnd,
            (HMENU)IDC_RANGE_FULL_RADIO, GetModuleHandle(nullptr), nullptr
        );
        g_hRangeCustomRadio = CreateWindowEx(
            0, L"BUTTON", L"Custom range",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_AUTORADIOBUTTON,
            150, 100, 120, 20, hwnd,
            (HMENU)IDC_RANGE_CUSTOM_RADIO, GetModuleHandle(nullptr), nullptr
        );

        // “Start time (s):” / “End time (s):” (disabled until drop)
        g_hStartStatic = CreateWindowEx(
            0, L"STATIC", L"Start time (s):",
            WS_CHILD | WS_VISIBLE | WS_DISABLED,
            290, 100, 100, 20, hwnd,
            (HMENU)IDC_START_STATIC, GetModuleHandle(nullptr), nullptr
        );
        g_hStartEdit = CreateWindowEx(
            WS_EX_CLIENTEDGE, L"EDIT", L"0",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | ES_NUMBER,
            390, 100, 80, 20, hwnd,
            (HMENU)IDC_START_EDIT, GetModuleHandle(nullptr), nullptr
        );
        g_hEndStatic = CreateWindowEx(
            0, L"STATIC", L"End time (s):",
            WS_CHILD | WS_VISIBLE | WS_DISABLED,
            480, 100, 100, 20, hwnd,
            (HMENU)IDC_END_STATIC, GetModuleHandle(nullptr), nullptr
        );
        g_hEndEdit = CreateWindowEx(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | ES_NUMBER,
            580, 100, 80, 20, hwnd,
            (HMENU)IDC_END_EDIT, GetModuleHandle(nullptr), nullptr
        );

        // “Full resolution” / “Half resolution” / “Quarter resolution” radios
        // (first of this group also gets WS_GROUP)
        g_hFullRadio = CreateWindowEx(
            0, L"BUTTON", L"Full resolution",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_AUTORADIOBUTTON | WS_GROUP,
            10, 130, 300, 20, hwnd,
            (HMENU)IDC_SCALE_FULL_RADIO, GetModuleHandle(nullptr), nullptr
        );
        g_hHalfRadio = CreateWindowEx(
            0, L"BUTTON", L"Half resolution",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_AUTORADIOBUTTON,
            10, 155, 300, 20, hwnd,
            (HMENU)IDC_SCALE_HALF_RADIO, GetModuleHandle(nullptr), nullptr
        );
        g_hQuarterRadio = CreateWindowEx(
            0, L"BUTTON", L"Quarter resolution",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_AUTORADIOBUTTON,
            10, 180, 300, 20, hwnd,
            (HMENU)IDC_SCALE_QUARTER_RADIO, GetModuleHandle(nullptr), nullptr
        );

        // “Start Processing” button
        g_hStartButton = CreateWindowEx(
            0, L"BUTTON", L"Start Processing",
            WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_DEFPUSHBUTTON,
            10, 210, 150, 30, hwnd,
            (HMENU)IDC_START_BUTTON, GetModuleHandle(nullptr), nullptr
        );
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
            // 1) Retrieve video info
            if (GetVideoInfo(g_inputPath, g_vidWidth, g_vidHeight, g_duration)) {
                // 2) Update filename label
                wchar_t infoText[512];
                double minutes = floor(g_duration / 60.0);
                double seconds = g_duration - minutes * 60.0;
                StringCchPrintfW(
                    infoText, 512,
                    L"%S   (%.0f:%02.0f)   %dx%d",
                    g_inputPath, minutes, seconds,
                    g_vidWidth, g_vidHeight
                );
                SetWindowTextW(g_hInfoStatic, infoText);

                // 3) Compute resolution options
                int fullW = g_vidWidth;
                int fullH = g_vidHeight;
                int halfW = g_vidWidth / 2;
                int halfH = g_vidHeight / 2;
                int quarterW = g_vidWidth / 4;
                int quarterH = g_vidHeight / 4;

                wchar_t fullText[64], halfText[64], quarterText[64];
                StringCchPrintfW(fullText, 64,
                    L"Full resolution    (%dx%d)", fullW, fullH);
                StringCchPrintfW(halfText, 64,
                    L"Half resolution    (%dx%d)", halfW, halfH);
                StringCchPrintfW(quarterText, 64,
                    L"Quarter resolution (%dx%d)", quarterW, quarterH);
                SetWindowTextW(g_hFullRadio, fullText);
                SetWindowTextW(g_hHalfRadio, halfText);
                SetWindowTextW(g_hQuarterRadio, quarterText);

                // 4) Enable all controls now that we have a video
                EnableWindow(g_hSizeStatic, TRUE);
                EnableWindow(g_hSizeEdit, TRUE);
                EnableWindow(g_hSuffixStatic, TRUE);
                EnableWindow(g_hSuffixEdit, TRUE);

                EnableWindow(g_hRangeFullRadio, TRUE);
                EnableWindow(g_hRangeCustomRadio, TRUE);
                // Default to “Full video”
                SendMessage(g_hRangeFullRadio, BM_SETCHECK, BST_CHECKED, 0);
                SendMessage(g_hRangeCustomRadio, BM_SETCHECK, BST_UNCHECKED, 0);

                // Disable start/end edits until “Custom range” is clicked
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

                // 5) Populate start/end defaults (0 and full duration)
                wchar_t startBuf[32], endBuf[32];
                StringCchPrintfW(startBuf, 32, L"0");
                int durInt = (int)floor(g_duration);
                StringCchPrintfW(endBuf, 32, L"%d", durInt);
                SetWindowTextW(g_hStartEdit, startBuf);
                SetWindowTextW(g_hEndEdit, endBuf);

                // 6) Extract middle frame of the *full* clip (always uses g_duration)
                if (g_hFrameBitmap) {
                    DeleteObject(g_hFrameBitmap);
                    g_hFrameBitmap = nullptr;
                }
                g_hFrameBitmap = ExtractMiddleFrameBitmap(
                    g_inputPath,
                    g_vidWidth,
                    g_vidHeight,
                    g_duration   // full duration
                );
                if (g_hFrameBitmap) {
                    BITMAP bi;
                    GetObject(g_hFrameBitmap, sizeof(bi), &bi);
                    g_frameWidth = bi.bmWidth;
                    g_frameHeight = bi.bmHeight;
                }

                // 7) Force an immediate repaint so the screenshot appears right away
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            else {
                MessageBox(hwnd,
                    L"Failed to retrieve video information.",
                    L"Error",
                    MB_ICONERROR
                );
                SetWindowTextW(g_hInfoStatic,
                    L"Drop a video file onto this window");
            }
        }
        DragFinish(hDrop);

        // Also reposition everything once more
        RECT rc;
        GetClientRect(hwnd, &rc);
        HandleResize(hwnd, rc.right, rc.bottom);
        break;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);

        // Toggling Full vs. Custom range
        if (wmId == IDC_RANGE_FULL_RADIO) {
            SendMessage(g_hRangeFullRadio, BM_SETCHECK, BST_CHECKED, 0);
            SendMessage(g_hRangeCustomRadio, BM_SETCHECK, BST_UNCHECKED, 0);
            EnableWindow(g_hStartStatic, FALSE);
            EnableWindow(g_hStartEdit, FALSE);
            EnableWindow(g_hEndStatic, FALSE);
            EnableWindow(g_hEndEdit, FALSE);
        }
        else if (wmId == IDC_RANGE_CUSTOM_RADIO) {
            SendMessage(g_hRangeFullRadio, BM_SETCHECK, BST_UNCHECKED, 0);
            SendMessage(g_hRangeCustomRadio, BM_SETCHECK, BST_CHECKED, 0);
            EnableWindow(g_hStartStatic, TRUE);
            EnableWindow(g_hStartEdit, TRUE);
            EnableWindow(g_hEndStatic, TRUE);
            EnableWindow(g_hEndEdit, TRUE);
        }

        // “Start Processing” clicked
        if (wmId == IDC_START_BUTTON) {
            wchar_t sizeBuf[32] = { 0 };
            GetWindowTextW(g_hSizeEdit, sizeBuf, ARRAYSIZE(sizeBuf));
            double targetSizeMB = _wtof(sizeBuf);
            if (targetSizeMB <= 0.0) {
                MessageBox(hwnd,
                    L"Please enter a valid target size in MB.",
                    L"Input Error",
                    MB_ICONWARNING
                );
                break;
            }

            wchar_t suffixW[128] = { 0 };
            GetWindowTextW(g_hSuffixEdit, suffixW, ARRAYSIZE(suffixW));
            if (wcslen(suffixW) == 0) {
                wcscpy_s(suffixW, L"RESIZED");
            }
            char suffixA[256] = { 0 };
            WideCharToMultiByte(
                CP_ACP, 0,
                suffixW, -1,
                suffixA, sizeof(suffixA),
                nullptr, nullptr
            );

            // Determine start/end for transcoding
            double startSecs = 0.0;
            double endSecs = g_duration;
            if (SendMessage(
                g_hRangeCustomRadio,
                BM_GETCHECK,
                0, 0
            ) == BST_CHECKED) {
                wchar_t startBuf[32], endBuf[32];
                GetWindowTextW(g_hStartEdit, startBuf, ARRAYSIZE(startBuf));
                GetWindowTextW(g_hEndEdit, endBuf, ARRAYSIZE(endBuf));
                startSecs = _wtof(startBuf);
                endSecs = _wtof(endBuf);
                if (startSecs < 0.0
                    || endSecs <= startSecs
                    || endSecs > g_duration
                    ) {
                    MessageBox(hwnd,
                        L"Please enter a valid start/end range within video duration.",
                        L"Input Error",
                        MB_ICONWARNING
                    );
                    break;
                }
            }

            // Determine scale factor
            int scaleFactor = 1;
            if (SendMessage(g_hFullRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                scaleFactor = 1;
            }
            else if (
                SendMessage(g_hHalfRadio, BM_GETCHECK, 0, 0) == BST_CHECKED
                ) {
                scaleFactor = 2;
            }
            else if (
                SendMessage(g_hQuarterRadio, BM_GETCHECK, 0, 0)
                == BST_CHECKED
                ) {
                scaleFactor = 4;
            }
            else {
                MessageBox(hwnd,
                    L"Please select a scale option.",
                    L"Input Error",
                    MB_ICONWARNING
                );
                break;
            }

            // Build output filename (append suffix + number if needed)
            char outPath[MAX_PATH] = { 0 };
            {
                char drive[_MAX_DRIVE], dir[_MAX_DIR],
                    fname[_MAX_FNAME], ext[_MAX_EXT];
                _splitpath_s(
                    g_inputPath,
                    drive, _MAX_DRIVE,
                    dir, _MAX_DIR,
                    fname, _MAX_FNAME,
                    ext, _MAX_EXT
                );

                char baseName[MAX_PATH];
                StringCchPrintfA(
                    baseName, MAX_PATH,
                    "%s_%s%s", fname, suffixA, ext
                );

                char candidate[MAX_PATH];
                _makepath_s(
                    candidate, MAX_PATH,
                    drive, dir, baseName, nullptr
                );

                if (_access(candidate, 0) == 0) {
                    for (int i = 1; ; i++) {
                        char numbered[MAX_PATH];
                        StringCchPrintfA(
                            numbered, MAX_PATH,
                            "%s_%s-%d%s",
                            fname, suffixA, i, ext
                        );
                        _makepath_s(
                            candidate, MAX_PATH,
                            drive, dir, numbered, nullptr
                        );
                        if (_access(candidate, 0) != 0) {
                            break;
                        }
                    }
                }
                StringCchCopyA(outPath, MAX_PATH, candidate);
            }

            // Disable button while transcoding
            EnableWindow(g_hStartButton, FALSE);
            bool success = TranscodeWithSizeAndScale(
                g_inputPath,
                outPath,
                targetSizeMB,
                scaleFactor,
                g_vidWidth,
                g_vidHeight,
                startSecs,
                endSecs
            );
            if (success) {
                std::string msg = "Successfully created:\n";
                msg += outPath;
                MessageBoxA(hwnd, msg.c_str(), "Success", MB_ICONINFORMATION);
            }
            else {
                MessageBox(hwnd,
                    L"Transcoding failed. See debug output for details.",
                    L"Error",
                    MB_ICONERROR
                );
            }
            EnableWindow(g_hStartButton, TRUE);
        }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_hFrameBitmap) {
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);

            // Get the filename-label's bottom edge to know where to start drawing
            RECT lblRect;
            GetWindowRect(g_hInfoStatic, &lblRect);
            ScreenToClient(hwnd, (POINT*)&lblRect.left);
            ScreenToClient(hwnd, (POINT*)&lblRect.right);
            int margin = 10;
            int topY = lblRect.bottom + margin;

            int availW = clientRect.right - margin * 2;
            int availH = clientRect.bottom - topY - margin;
            if (availW > 0 && availH > 0) {
                double imgAR = (double)g_frameWidth / g_frameHeight;
                int destW = availW;
                int destH = (int)(availW / imgAR);
                if (destH > availH) {
                    destH = availH;
                    destW = (int)(availH * imgAR);
                }
                int destX = (clientRect.right - destW) / 2;
                int destY = topY;

                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, g_hFrameBitmap);

                SetStretchBltMode(hdc, HALFTONE);
                SetBrushOrgEx(hdc, 0, 0, nullptr);
                StretchBlt(
                    hdc, destX, destY, destW, destH,
                    memDC, 0, 0, g_frameWidth, g_frameHeight,
                    SRCCOPY
                );

                SelectObject(memDC, oldBmp);
                DeleteDC(memDC);
            }
        }
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DESTROY:
        if (g_hFrameBitmap) {
            DeleteObject(g_hFrameBitmap);
            g_hFrameBitmap = nullptr;
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

//-----------------------------------------------------------------------------
// GetVideoInfo: retrieve width, height, and duration using FFmpeg.
//-----------------------------------------------------------------------------
bool GetVideoInfo(const char* filepath, int& width, int& height, double& durationSeconds) {
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filepath, nullptr, nullptr) < 0) {
        return false;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        return false;
    }
    int videoStreamIndex = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = i;
            break;
        }
    }
    if (videoStreamIndex < 0) {
        avformat_close_input(&fmt_ctx);
        return false;
    }
    AVStream* videoStream = fmt_ctx->streams[videoStreamIndex];
    width = videoStream->codecpar->width;
    height = videoStream->codecpar->height;
    if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        durationSeconds = fmt_ctx->duration / (double)AV_TIME_BASE;
    }
    else {
        durationSeconds = 0.0;
    }
    avformat_close_input(&fmt_ctx);
    return true;
}

//-----------------------------------------------------------------------------
// ExtractMiddleFrameBitmap:
// Seeks to the exact middle timestamp (of the *full* video), decodes one frame,
// converts it to BGR24, and creates an HBITMAP.
// Always uses `duration` (the WHOLE CLIP), ignoring any custom range.
//-----------------------------------------------------------------------------
HBITMAP ExtractMiddleFrameBitmap(const char* filepath, int orig_w, int orig_h, double duration) {
    // Declarations only:
    AVFormatContext* fmt_ctx;
    AVCodecContext* dec_ctx;
    SwsContext* sws_ctx;
    AVPacket* pkt;
    AVFrame* frame;
    AVFrame* rgbFrame;
    int64_t          middle_ts;
    uint8_t* rgbBuffer;
    HBITMAP          hBitmap;
    int              videoStreamIndex;
    AVStream* videoStream;
    const AVCodec* decoder;
    bool             gotFrame;
    int              rgbBufSize;
    BITMAPINFO       bmi;
    HDC              hdc;
    void* dibBits;
    int              rowBytes;

    fmt_ctx = nullptr;
    dec_ctx = nullptr;
    sws_ctx = nullptr;
    pkt = nullptr;
    frame = nullptr;
    rgbFrame = nullptr;
    middle_ts = 0;
    rgbBuffer = nullptr;
    hBitmap = nullptr;
    videoStreamIndex = -1;
    videoStream = nullptr;
    decoder = nullptr;
    gotFrame = false;
    rgbBufSize = 0;
    ZeroMemory(&bmi, sizeof(bmi));
    hdc = nullptr;
    dibBits = nullptr;
    rowBytes = 0;

    if (avformat_open_input(&fmt_ctx, filepath, nullptr, nullptr) < 0) {
        goto cleanup;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        goto cleanup;
    }
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = i;
            break;
        }
    }
    if (videoStreamIndex < 0) {
        goto cleanup;
    }
    videoStream = fmt_ctx->streams[videoStreamIndex];

    decoder = avcodec_find_decoder(videoStream->codecpar->codec_id);
    if (!decoder) {
        goto cleanup;
    }
    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) {
        goto cleanup;
    }
    if (avcodec_parameters_to_context(dec_ctx, videoStream->codecpar) < 0) {
        goto cleanup;
    }
    if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) {
        goto cleanup;
    }

    // **Always** seek to the midpoint of the *full* duration:
    middle_ts = (int64_t)((duration / 2.0) * AV_TIME_BASE);
    if (av_seek_frame(fmt_ctx, -1, middle_ts, AVSEEK_FLAG_BACKWARD) < 0) {
        // If seeking fails, we still attempt to decode frames from the start of the stream.
    }
    avcodec_flush_buffers(dec_ctx);

    frame = av_frame_alloc();
    rgbFrame = av_frame_alloc();
    if (!frame || !rgbFrame) {
        goto cleanup;
    }
    pkt = av_packet_alloc();
    if (!pkt) {
        goto cleanup;
    }

    sws_ctx = sws_getContext(
        dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
        dec_ctx->width, dec_ctx->height, AV_PIX_FMT_BGR24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!sws_ctx) {
        goto cleanup;
    }
    rgbBufSize = av_image_get_buffer_size(
        AV_PIX_FMT_BGR24, dec_ctx->width, dec_ctx->height, 1
    );
    rgbBuffer = (uint8_t*)av_malloc(rgbBufSize);
    if (!rgbBuffer) {
        goto cleanup;
    }
    av_image_fill_arrays(
        rgbFrame->data, rgbFrame->linesize,
        rgbBuffer,
        AV_PIX_FMT_BGR24,
        dec_ctx->width, dec_ctx->height, 1
    );

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(dec_ctx, pkt) < 0) {
                av_packet_unref(pkt);
                break;
            }
            if (avcodec_receive_frame(dec_ctx, frame) == 0) {
                sws_scale(
                    sws_ctx,
                    frame->data, frame->linesize,
                    0, dec_ctx->height,
                    rgbFrame->data, rgbFrame->linesize
                );
                gotFrame = true;
                av_frame_unref(frame);
                av_packet_unref(pkt);
                break;
            }
        }
        av_packet_unref(pkt);
    }
    if (!gotFrame) {
        goto cleanup;
    }

    // Create an HBITMAP from the BGR24 data
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = dec_ctx->width;
    bmi.bmiHeader.biHeight = -dec_ctx->height;  // top‐down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    hdc = GetDC(nullptr);
    hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hBitmap) {
        goto cleanup;
    }

    rowBytes = dec_ctx->width * 3;
    for (int y = 0; y < dec_ctx->height; y++) {
        memcpy(
            (uint8_t*)dibBits + y * rowBytes,
            rgbFrame->data[0] + y * rgbFrame->linesize[0],
            rowBytes
        );
    }

cleanup:
    if (sws_ctx)     sws_freeContext(sws_ctx);
    if (frame)       av_frame_free(&frame);
    if (rgbFrame) {
        if (rgbBuffer) {
            av_free(rgbBuffer);
            rgbBuffer = nullptr;
        }
        av_frame_free(&rgbFrame);
    }
    if (pkt)         av_packet_free(&pkt);
    if (dec_ctx)     avcodec_free_context(&dec_ctx);
    if (fmt_ctx)     avformat_close_input(&fmt_ctx);

    return hBitmap;
}

//-----------------------------------------------------------------------------
// TranscodeWithSizeAndScale:
// Uses NVENC if available, else CPU H.264 encoder, enforcing CBR to hit target size.
// Supports full‐clip or custom start/end.
//-----------------------------------------------------------------------------
bool TranscodeWithSizeAndScale(
    const char* in_filename,
    const char* out_filename,
    double      target_size_mb,
    int         scale_factor,
    int         orig_w,
    int         orig_h,
    double      start_seconds,
    double      end_seconds
) {
    // 1) Declarations
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

    // 2) Validate times and compute target bitrate
    if (end_seconds <= start_seconds) {
        OutputDebugStringA("End time must be greater than start time.\n");
        return false;
    }
    double segment_duration = end_seconds - start_seconds;
    if (segment_duration <= 0.0) {
        OutputDebugStringA("Invalid segment duration.\n");
        return false;
    }
    {
        int64_t total_bits = (int64_t)(target_size_mb * 8.0 * 1024.0 * 1024.0);
        int64_t video_bits = (int64_t)(total_bits * 0.95);
        target_bitrate = video_bits / (int64_t)segment_duration;
    }
    if (target_bitrate <= 0) {
        OutputDebugStringA("Invalid target bitrate calculated.\n");
        return false;
    }

    // 3) Open input and find streams
    if (avformat_open_input(&in_fmt_ctx, in_filename, nullptr, nullptr) < 0) {
        OutputDebugStringA("Could not open input file.\n");
        goto cleanup;
    }
    if (avformat_find_stream_info(in_fmt_ctx, nullptr) < 0) {
        OutputDebugStringA("Could not find stream info.\n");
        goto cleanup;
    }
    for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; i++) {
        AVStream* st = in_fmt_ctx->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStreamIndex < 0) {
            videoStreamIndex = i;
        }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStreamIndex < 0) {
            audioStreamIndex = i;
        }
    }
    if (videoStreamIndex < 0) {
        OutputDebugStringA("No video stream found.\n");
        goto cleanup;
    }
    video_in_stream = in_fmt_ctx->streams[videoStreamIndex];
    if (audioStreamIndex >= 0) {
        audio_in_stream = in_fmt_ctx->streams[audioStreamIndex];
    }

    // 4) Open video decoder
    video_decoder = avcodec_find_decoder(video_in_stream->codecpar->codec_id);
    if (!video_decoder) {
        OutputDebugStringA("Video decoder not found.\n");
        goto cleanup;
    }
    dec_ctx = avcodec_alloc_context3(video_decoder);
    if (!dec_ctx) {
        OutputDebugStringA("Failed to allocate video decoder context.\n");
        goto cleanup;
    }
    if (avcodec_parameters_to_context(dec_ctx, video_in_stream->codecpar) < 0) {
        OutputDebugStringA("Failed to copy video params to decoder.\n");
        goto cleanup;
    }
    if (avcodec_open2(dec_ctx, video_decoder, nullptr) < 0) {
        OutputDebugStringA("Failed to open video decoder.\n");
        goto cleanup;
    }

    // 5) Create output format context
    avformat_alloc_output_context2(&out_fmt_ctx, nullptr, nullptr, out_filename);
    if (!out_fmt_ctx) {
        OutputDebugStringA("Could not create output format context.\n");
        goto cleanup;
    }

    // 6) Choose NVENC if available, else CPU H.264
    video_encoder = avcodec_find_encoder_by_name("h264_nvenc");
    if (video_encoder) {
        OutputDebugStringA("Using NVENC encoder.\n");
    }
    else {
        OutputDebugStringA("NVENC not found; falling back to CPU H.264.\n");
        video_encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!video_encoder) {
            OutputDebugStringA("H.264 encoder not found.\n");
            goto cleanup;
        }
    }

    // 7) Set up video encoder context
    video_out_stream = avformat_new_stream(out_fmt_ctx, video_encoder);
    if (!video_out_stream) {
        OutputDebugStringA("Could not create video output stream.\n");
        goto cleanup;
    }
    enc_ctx = avcodec_alloc_context3(video_encoder);
    if (!enc_ctx) {
        OutputDebugStringA("Failed to allocate video encoder context.\n");
        goto cleanup;
    }
    enc_ctx->height = orig_h / scale_factor;
    enc_ctx->width = orig_w / scale_factor;
    enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
    if (strcmp(video_encoder->name, "h264_nvenc") == 0) {
        enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    }
    else {
        enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        av_opt_set(enc_ctx->priv_data, "preset", "medium", 0);
        av_opt_set(enc_ctx->priv_data, "nal-hrd", "cbr", 0);
    }
    enc_ctx->time_base = av_inv_q(dec_ctx->framerate);
    enc_ctx->bit_rate = target_bitrate;
    enc_ctx->rc_max_rate = target_bitrate;
    enc_ctx->rc_buffer_size = target_bitrate;
    if (out_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (avcodec_open2(enc_ctx, video_encoder, nullptr) < 0) {
        OutputDebugStringA("Could not open video encoder.\n");
        goto cleanup;
    }
    if (avcodec_parameters_from_context(video_out_stream->codecpar, enc_ctx) < 0) {
        OutputDebugStringA("Failed to copy encoder params to output.\n");
        goto cleanup;
    }
    video_out_stream->time_base = enc_ctx->time_base;

    // 8) Audio passthrough (if present)
    if (audio_in_stream) {
        audio_out_stream = avformat_new_stream(out_fmt_ctx, nullptr);
        if (!audio_out_stream) {
            OutputDebugStringA("Could not create audio output stream. Disabling audio.\n");
            audio_in_stream = nullptr;
        }
        else {
            if (avcodec_parameters_copy(audio_out_stream->codecpar, audio_in_stream->codecpar) < 0) {
                OutputDebugStringA("Failed to copy audio params. Disabling audio.\n");
                audio_in_stream = nullptr;
                audio_out_stream = nullptr;
            }
            else {
                audio_out_stream->time_base = audio_in_stream->time_base;
            }
        }
    }

    // 9) Open output file & write header
    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt_ctx->pb, out_filename, AVIO_FLAG_WRITE) < 0) {
            OutputDebugStringA("Could not open output file.\n");
            goto cleanup;
        }
    }
    if (avformat_write_header(out_fmt_ctx, nullptr) < 0) {
        OutputDebugStringA("Error writing header to output.\n");
        goto cleanup;
    }

    // 10) SEEK to start time (calculate both video & audio start PTS)
    {
        // 10a) Calculate video_start_pts in VIDEO stream’s time_base
        AVRational video_tb = in_fmt_ctx->streams[videoStreamIndex]->time_base;
        int64_t pts_in_avtime = (int64_t)(start_seconds * AV_TIME_BASE);
        video_start_pts = av_rescale_q(pts_in_avtime, AV_TIME_BASE_Q, video_tb);

        if (av_seek_frame(in_fmt_ctx, videoStreamIndex, video_start_pts, AVSEEK_FLAG_BACKWARD) < 0) {
            OutputDebugStringA("Warning: could not seek exactly to start time (video).\n");
        }

        // 10b) Calculate audio_start_pts in AUDIO stream’s time_base (if audio exists)
        if (audio_in_stream) {
            AVRational audio_tb = in_fmt_ctx->streams[audioStreamIndex]->time_base;
            audio_start_pts = av_rescale_q(pts_in_avtime, AV_TIME_BASE_Q, audio_tb);
        }
    }
    avcodec_flush_buffers(dec_ctx);

    // 11) Allocate frames/packets + SwsContext
    frame = av_frame_alloc();
    filt_frame = av_frame_alloc();
    pkt = av_packet_alloc();
    enc_pkt = av_packet_alloc();
    if (!frame || !filt_frame || !pkt || !enc_pkt) {
        OutputDebugStringA("Could not allocate frame/packet.\n");
        goto cleanup;
    }
    sws_ctx = sws_getContext(
        dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
        enc_ctx->width, enc_ctx->height, enc_ctx->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!sws_ctx) {
        OutputDebugStringA("Could not initialize SwsContext.\n");
        goto cleanup;
    }
    filt_frame->format = enc_ctx->pix_fmt;
    filt_frame->width = enc_ctx->width;
    filt_frame->height = enc_ctx->height;
    if (av_frame_get_buffer(filt_frame, 32) < 0) {
        OutputDebugStringA("Could not allocate buffer for scaled frame.\n");
        goto cleanup;
    }

    // 12) Read packets and encode until end_seconds
    while (av_read_frame(in_fmt_ctx, pkt) >= 0) {
        // VIDEO packets
        if (pkt->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(dec_ctx, pkt) < 0) {
                av_packet_unref(pkt);
                break;
            }
            while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                int64_t in_pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                    ? frame->best_effort_timestamp
                    : frame->pts;
                double in_time = in_pts * av_q2d(video_in_stream->time_base);
                if (in_time > end_seconds) {
                    av_frame_unref(frame);
                    goto flush_encoder;
                }

                // ─── ZERO‐BASED VIDEO PTS ───
                int64_t rel_vid_pts = in_pts - video_start_pts;
                filt_frame->pts = av_rescale_q(
                    rel_vid_pts,
                    video_in_stream->time_base,
                    enc_ctx->time_base
                );
                // ─────────────────────────────

                sws_scale(
                    sws_ctx,
                    frame->data, frame->linesize,
                    0, dec_ctx->height,
                    filt_frame->data, filt_frame->linesize
                );
                if (avcodec_send_frame(enc_ctx, filt_frame) < 0) {
                    OutputDebugStringA("Error sending frame to video encoder.\n");
                    break;
                }
                while (avcodec_receive_packet(enc_ctx, enc_pkt) == 0) {
                    enc_pkt->stream_index = video_out_stream->index;
                    av_packet_rescale_ts(
                        enc_pkt,
                        enc_ctx->time_base,
                        video_out_stream->time_base
                    );
                    av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
                    av_packet_unref(enc_pkt);
                }
                av_frame_unref(frame);
            }
        }
        // AUDIO packets (passthrough), only if within range
        else if (audio_in_stream && pkt->stream_index == audioStreamIndex) {
            int64_t aud_in_pts = pkt->pts;
            double aud_time = aud_in_pts * av_q2d(audio_in_stream->time_base);
            if (aud_time < start_seconds || aud_time > end_seconds) {
                av_packet_unref(pkt);
                continue;
            }

            // ─── ZERO‐BASED AUDIO PTS ───
            int64_t rel_aud_pts = aud_in_pts - audio_start_pts;
            pkt->pts = rel_aud_pts;
            if (pkt->dts != AV_NOPTS_VALUE) {
                pkt->dts = rel_aud_pts;
            }
            // ─────────────────────────────

            pkt->stream_index = audio_out_stream->index;
            av_packet_rescale_ts(
                pkt,
                audio_in_stream->time_base,
                audio_out_stream->time_base
            );
            av_interleaved_write_frame(out_fmt_ctx, pkt);
        }
        av_packet_unref(pkt);
    }

flush_encoder:
    // Flush video encoder
    avcodec_send_frame(enc_ctx, nullptr);
    while (avcodec_receive_packet(enc_ctx, enc_pkt) == 0) {
        enc_pkt->stream_index = video_out_stream->index;
        av_packet_rescale_ts(
            enc_pkt,
            enc_ctx->time_base,
            video_out_stream->time_base
        );
        av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
        av_packet_unref(enc_pkt);
    }
    av_packet_unref(enc_pkt);

    // 13) Write trailer & cleanup
    av_write_trailer(out_fmt_ctx);
    success = true;

cleanup:
    if (sws_ctx)         sws_freeContext(sws_ctx);
    if (frame)           av_frame_free(&frame);
    if (filt_frame)      av_frame_free(&filt_frame);
    if (pkt)             av_packet_free(&pkt);
    if (enc_pkt)         av_packet_free(&enc_pkt);
    if (dec_ctx)         avcodec_free_context(&dec_ctx);
    if (enc_ctx)         avcodec_free_context(&enc_ctx);
    if (in_fmt_ctx)      avformat_close_input(&in_fmt_ctx);
    if (out_fmt_ctx) {
        if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&out_fmt_ctx->pb);
        }
        avformat_free_context(out_fmt_ctx);
    }
    return success;
}
