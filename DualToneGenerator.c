/*
 * Dual Tone Generator - a small native Win32 application.
 *
 * Plays an independent sine tone in the left and right channel
 * (1 Hz - 20000 Hz each), changeable live while playing, and can
 * export the current pair of tones to a 16-bit 44.1 kHz stereo WAV.
 *
 * Build (mingw-w64):
 *   windres app.rc -O coff -o app.res
 *   x86_64-w64-mingw32-gcc -O2 -mwindows DualToneGenerator.c app.res \
 *       -o DualToneGenerator.exe -lwinmm -lcomctl32 -lcomdlg32 -static
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* audio configuration                                                 */
/* ------------------------------------------------------------------ */
#define SAMPLE_RATE   44100
#define BUF_FRAMES    2205            /* 50 ms per buffer              */
#define NUM_BUFFERS   4
#define FADE_SECONDS  0.010           /* click-free start / stop       */

#define FREQ_MIN      1.0
#define FREQ_MAX      20000.0
#define SLIDER_STEPS  2000

/* control ids */
#define IDC_SLIDER_L   1001
#define IDC_SLIDER_R   1002
#define IDC_EDIT_L     1003
#define IDC_EDIT_R     1004
#define IDC_SLIDER_VOL 1005
#define IDC_BTN_PLAY   1006
#define IDC_BTN_SAVE   1007
#define IDC_EDIT_DUR   1008
#define IDC_LBL_VOL    1009
#define IDC_LBL_STATUS 1010

/* ------------------------------------------------------------------ */
/* shared state between the UI thread and the audio thread             */
/* ------------------------------------------------------------------ */
static volatile double g_freqL  = 440.0;
static volatile double g_freqR  = 445.0;
static volatile double g_volume = 0.35;

static volatile LONG   g_stopRequest = 0;
static volatile LONG   g_isPlaying   = 0;
static HANDLE          g_audioThread = NULL;
static HANDLE          g_audioEvent  = NULL;

static HWND  g_hMain = NULL;
static HFONT g_font  = NULL;
static BOOL  g_suppressEditNotify = FALSE;

/* ------------------------------------------------------------------ */
/* helpers: logarithmic slider <-> frequency mapping                   */
/* ------------------------------------------------------------------ */
static double PosToFreq(int pos)
{
    double t = (double)pos / (double)SLIDER_STEPS;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return FREQ_MIN * pow(FREQ_MAX / FREQ_MIN, t);
}

static int FreqToPos(double f)
{
    if (f < FREQ_MIN) f = FREQ_MIN;
    if (f > FREQ_MAX) f = FREQ_MAX;
    return (int)(SLIDER_STEPS * log(f / FREQ_MIN) / log(FREQ_MAX / FREQ_MIN) + 0.5);
}

static double ClampFreq(double f)
{
    if (f < FREQ_MIN) return FREQ_MIN;
    if (f > FREQ_MAX) return FREQ_MAX;
    return f;
}

static void FormatFreq(char *dst, size_t n, double f)
{
    if (fabs(f - floor(f + 0.5)) < 1e-9)
        snprintf(dst, n, "%.0f", f);
    else
        snprintf(dst, n, "%.2f", f);
}

/* ------------------------------------------------------------------ */
/* audio thread: streams sine waves through waveOut                    */
/* ------------------------------------------------------------------ */
static short g_pcm[NUM_BUFFERS][BUF_FRAMES * 2];

static DWORD WINAPI AudioThreadProc(LPVOID param)
{
    WAVEFORMATEX wf;
    HWAVEOUT     hwo = NULL;
    WAVEHDR      hdr[NUM_BUFFERS];
    double       phaseL = 0.0, phaseR = 0.0;
    double       gain = 0.0;
    double       fadeStep = 1.0 / (FADE_SECONDS * SAMPLE_RATE);
    int          i, closing = 0;

    (void)param;

    ZeroMemory(&wf, sizeof(wf));
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 2;
    wf.nSamplesPerSec  = SAMPLE_RATE;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

    if (waveOutOpen(&hwo, WAVE_MAPPER, &wf, (DWORD_PTR)g_audioEvent, 0,
                    CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        InterlockedExchange(&g_isPlaying, 0);
        if (g_hMain) PostMessage(g_hMain, WM_APP + 1, 0, 0);
        return 1;
    }

    for (i = 0; i < NUM_BUFFERS; i++) {
        ZeroMemory(&hdr[i], sizeof(WAVEHDR));
        hdr[i].lpData         = (LPSTR)g_pcm[i];
        hdr[i].dwBufferLength = BUF_FRAMES * 2 * sizeof(short);
        waveOutPrepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
        hdr[i].dwFlags |= WHDR_DONE;      /* mark as free to fill */
    }

    for (;;) {
        int filledAny = 0;

        for (i = 0; i < NUM_BUFFERS; i++) {
            short *out;
            int    n;
            double fL, fR, vol, stepL, stepR, target;

            if (!(hdr[i].dwFlags & WHDR_DONE))
                continue;

            fL  = g_freqL;
            fR  = g_freqR;
            vol = g_volume;
            target = g_stopRequest ? 0.0 : 1.0;

            stepL = 2.0 * M_PI * fL / SAMPLE_RATE;
            stepR = 2.0 * M_PI * fR / SAMPLE_RATE;

            out = g_pcm[i];
            for (n = 0; n < BUF_FRAMES; n++) {
                double a, l, r;

                if (gain < target) { gain += fadeStep; if (gain > target) gain = target; }
                else if (gain > target) { gain -= fadeStep; if (gain < target) gain = target; }

                a = gain * vol;
                l = sin(phaseL) * a;
                r = sin(phaseR) * a;

                phaseL += stepL;
                phaseR += stepR;
                if (phaseL > 2.0 * M_PI) phaseL -= 2.0 * M_PI;
                if (phaseR > 2.0 * M_PI) phaseR -= 2.0 * M_PI;

                *out++ = (short)(l * 32000.0);
                *out++ = (short)(r * 32000.0);
            }

            hdr[i].dwFlags &= ~WHDR_DONE;
            if (waveOutWrite(hwo, &hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                hdr[i].dwFlags |= WHDR_DONE;
                closing = 1;
                break;
            }
            filledAny = 1;
        }

        if (closing)
            break;

        if (g_stopRequest && gain <= 0.0) {
            /* let the faded-out tail drain, then finish */
            Sleep((DWORD)(BUF_FRAMES * 1000 / SAMPLE_RATE * NUM_BUFFERS));
            break;
        }

        if (!filledAny)
            WaitForSingleObject(g_audioEvent, 100);
    }

    waveOutReset(hwo);
    for (i = 0; i < NUM_BUFFERS; i++)
        waveOutUnprepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
    waveOutClose(hwo);

    InterlockedExchange(&g_isPlaying, 0);
    if (g_hMain) PostMessage(g_hMain, WM_APP + 1, 0, 0);
    return 0;
}

static void StartPlayback(void)
{
    if (g_isPlaying) return;
    InterlockedExchange(&g_stopRequest, 0);
    InterlockedExchange(&g_isPlaying, 1);
    if (g_audioThread) { CloseHandle(g_audioThread); g_audioThread = NULL; }
    g_audioThread = CreateThread(NULL, 0, AudioThreadProc, NULL, 0, NULL);
    if (!g_audioThread) InterlockedExchange(&g_isPlaying, 0);
}

static void StopPlayback(BOOL wait)
{
    InterlockedExchange(&g_stopRequest, 1);
    if (g_audioEvent) SetEvent(g_audioEvent);
    if (wait && g_audioThread) {
        WaitForSingleObject(g_audioThread, 2000);
        CloseHandle(g_audioThread);
        g_audioThread = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* WAV export                                                          */
/* ------------------------------------------------------------------ */
static BOOL WriteWavFile(const char *path, double fL, double fR,
                         double seconds, double vol)
{
    FILE  *fp;
    long   total  = (long)(seconds * SAMPLE_RATE + 0.5);
    long   fadeN  = (long)(0.005 * SAMPLE_RATE);
    long   i;
    double phL = 0.0, phR = 0.0;
    double stepL = 2.0 * M_PI * fL / SAMPLE_RATE;
    double stepR = 2.0 * M_PI * fR / SAMPLE_RATE;
    unsigned long dataBytes, riffSize, byteRate;
    unsigned short blockAlign = 4, channels = 2, bits = 16, fmt = 1;
    unsigned long  sr = SAMPLE_RATE, sub1 = 16;

    if (total < 1) total = 1;
    if (fadeN * 2 > total) fadeN = total / 2;
    dataBytes = (unsigned long)total * 4UL;
    riffSize  = 36UL + dataBytes;
    byteRate  = sr * 4UL;

    fp = fopen(path, "wb");
    if (!fp) return FALSE;

    fwrite("RIFF", 1, 4, fp);
    fwrite(&riffSize, 4, 1, fp);
    fwrite("WAVEfmt ", 1, 8, fp);
    fwrite(&sub1, 4, 1, fp);
    fwrite(&fmt, 2, 1, fp);
    fwrite(&channels, 2, 1, fp);
    fwrite(&sr, 4, 1, fp);
    fwrite(&byteRate, 4, 1, fp);
    fwrite(&blockAlign, 2, 1, fp);
    fwrite(&bits, 2, 1, fp);
    fwrite("data", 1, 4, fp);
    fwrite(&dataBytes, 4, 1, fp);

    for (i = 0; i < total; i++) {
        double env = 1.0;
        short  s[2];

        if (fadeN > 0) {
            if (i < fadeN)              env = (double)i / fadeN;
            else if (i >= total - fadeN) env = (double)(total - 1 - i) / fadeN;
        }
        s[0] = (short)(sin(phL) * env * vol * 32000.0);
        s[1] = (short)(sin(phR) * env * vol * 32000.0);
        fwrite(s, sizeof(short), 2, fp);

        phL += stepL; phR += stepR;
        if (phL > 2.0 * M_PI) phL -= 2.0 * M_PI;
        if (phR > 2.0 * M_PI) phR -= 2.0 * M_PI;
    }

    fclose(fp);
    return TRUE;
}

static void DoSaveWav(HWND hwnd)
{
    OPENFILENAMEA ofn;
    char path[MAX_PATH] = "dual-tone.wav";
    char durText[32];
    double seconds;

    GetDlgItemTextA(hwnd, IDC_EDIT_DUR, durText, sizeof(durText));
    seconds = atof(durText);
    if (seconds <= 0.0)   seconds = 10.0;
    if (seconds > 3600.0) seconds = 3600.0;

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = "WAV audio (*.wav)\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = "Save tone as WAV";
    ofn.lpstrDefExt = "wav";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameA(&ofn))
        return;

    SetCursor(LoadCursor(NULL, IDC_WAIT));
    if (WriteWavFile(path, g_freqL, g_freqR, seconds, g_volume)) {
        char msg[MAX_PATH + 96];
        snprintf(msg, sizeof(msg), "Saved %.10g s of %.10g Hz (L) / %.10g Hz (R) to:\n\n%s",
                 seconds, g_freqL, g_freqR, path);
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        MessageBoxA(hwnd, msg, "Dual Tone Generator", MB_OK | MB_ICONINFORMATION);
    } else {
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        MessageBoxA(hwnd, "Could not write that file.", "Dual Tone Generator",
                    MB_OK | MB_ICONERROR);
    }
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */
static HWND MakeCtl(HWND parent, const char *cls, const char *text,
                    DWORD style, int x, int y, int w, int h, int id)
{
    HWND h2 = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                              x, y, w, h, parent, (HMENU)(INT_PTR)id,
                              (HINSTANCE)GetWindowLongPtr(parent, GWLP_HINSTANCE), NULL);
    if (h2 && g_font) SendMessage(h2, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h2;
}

static void UpdateVolumeLabel(HWND hwnd)
{
    char t[32];
    snprintf(t, sizeof(t), "Volume: %d%%", (int)(g_volume * 100.0 + 0.5));
    SetDlgItemTextA(hwnd, IDC_LBL_VOL, t);
}

static void UpdateStatus(HWND hwnd)
{
    SetDlgItemTextA(hwnd, IDC_BTN_PLAY, g_isPlaying ? "Stop" : "Play");
    SetDlgItemTextA(hwnd, IDC_LBL_STATUS,
        g_isPlaying ? "Playing - drag the sliders to change pitch live."
                    : "Stopped.");
}

static void SetFreqFromSlider(HWND hwnd, int which, int pos)
{
    char t[32];
    double f = ClampFreq(PosToFreq(pos));

    if (which == 0) g_freqL = f; else g_freqR = f;

    FormatFreq(t, sizeof(t), f);
    g_suppressEditNotify = TRUE;
    SetDlgItemTextA(hwnd, which == 0 ? IDC_EDIT_L : IDC_EDIT_R, t);
    g_suppressEditNotify = FALSE;
}

static void SetFreqFromEdit(HWND hwnd, int which)
{
    char t[32];
    double f;

    GetDlgItemTextA(hwnd, which == 0 ? IDC_EDIT_L : IDC_EDIT_R, t, sizeof(t));
    if (t[0] == '\0') return;
    f = atof(t);
    if (f <= 0.0) return;
    f = ClampFreq(f);

    if (which == 0) g_freqL = f; else g_freqR = f;
    SendDlgItemMessage(hwnd, which == 0 ? IDC_SLIDER_L : IDC_SLIDER_R,
                       TBM_SETPOS, TRUE, FreqToPos(f));
}

static void CreateControls(HWND hwnd)
{
    char t[32];
    const int M = 16, SW = 330, EW = 96;

    MakeCtl(hwnd, "STATIC", "Left channel", SS_LEFT, M, 12, 200, 18, -1);
    MakeCtl(hwnd, TRACKBAR_CLASS, "", TBS_HORZ | TBS_NOTICKS,
            M, 32, SW, 30, IDC_SLIDER_L);
    MakeCtl(hwnd, "EDIT", "", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT,
            M + SW + 12, 35, EW, 24, IDC_EDIT_L);
    MakeCtl(hwnd, "STATIC", "Hz", SS_LEFT, M + SW + 12 + EW + 6, 39, 30, 18, -1);

    MakeCtl(hwnd, "STATIC", "Right channel", SS_LEFT, M, 76, 200, 18, -1);
    MakeCtl(hwnd, TRACKBAR_CLASS, "", TBS_HORZ | TBS_NOTICKS,
            M, 96, SW, 30, IDC_SLIDER_R);
    MakeCtl(hwnd, "EDIT", "", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT,
            M + SW + 12, 99, EW, 24, IDC_EDIT_R);
    MakeCtl(hwnd, "STATIC", "Hz", SS_LEFT, M + SW + 12 + EW + 6, 103, 30, 18, -1);

    MakeCtl(hwnd, "STATIC", "Volume: 35%", SS_LEFT, M, 144, 120, 18, IDC_LBL_VOL);
    MakeCtl(hwnd, TRACKBAR_CLASS, "", TBS_HORZ | TBS_NOTICKS,
            M + 110, 138, 220, 30, IDC_SLIDER_VOL);

    MakeCtl(hwnd, "BUTTON", "Play", BS_PUSHBUTTON | WS_TABSTOP,
            M, 186, 110, 34, IDC_BTN_PLAY);
    MakeCtl(hwnd, "BUTTON", "Save WAV...", BS_PUSHBUTTON | WS_TABSTOP,
            M + 122, 186, 120, 34, IDC_BTN_SAVE);
    MakeCtl(hwnd, "EDIT", "10", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT,
            M + 254, 191, 56, 24, IDC_EDIT_DUR);
    MakeCtl(hwnd, "STATIC", "seconds", SS_LEFT, M + 316, 195, 70, 18, -1);

    MakeCtl(hwnd, "STATIC", "Stopped.", SS_LEFT, M, 234, 460, 18, IDC_LBL_STATUS);

    SendDlgItemMessage(hwnd, IDC_SLIDER_L, TBM_SETRANGE, TRUE, MAKELPARAM(0, SLIDER_STEPS));
    SendDlgItemMessage(hwnd, IDC_SLIDER_R, TBM_SETRANGE, TRUE, MAKELPARAM(0, SLIDER_STEPS));
    SendDlgItemMessage(hwnd, IDC_SLIDER_L, TBM_SETPOS, TRUE, FreqToPos(g_freqL));
    SendDlgItemMessage(hwnd, IDC_SLIDER_R, TBM_SETPOS, TRUE, FreqToPos(g_freqR));
    SendDlgItemMessage(hwnd, IDC_SLIDER_VOL, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendDlgItemMessage(hwnd, IDC_SLIDER_VOL, TBM_SETPOS, TRUE, (int)(g_volume * 100));

    g_suppressEditNotify = TRUE;
    FormatFreq(t, sizeof(t), g_freqL); SetDlgItemTextA(hwnd, IDC_EDIT_L, t);
    FormatFreq(t, sizeof(t), g_freqR); SetDlgItemTextA(hwnd, IDC_EDIT_R, t);
    g_suppressEditNotify = FALSE;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hMain = hwnd;
        CreateControls(hwnd);
        return 0;

    case WM_HSCROLL: {
        HWND ctl = (HWND)lp;
        int  id  = GetDlgCtrlID(ctl);
        int  pos = (int)SendMessage(ctl, TBM_GETPOS, 0, 0);
        if (id == IDC_SLIDER_L)      SetFreqFromSlider(hwnd, 0, pos);
        else if (id == IDC_SLIDER_R) SetFreqFromSlider(hwnd, 1, pos);
        else if (id == IDC_SLIDER_VOL) { g_volume = pos / 100.0; UpdateVolumeLabel(hwnd); }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_EDIT_L:
            if (HIWORD(wp) == EN_CHANGE && !g_suppressEditNotify)
                SetFreqFromEdit(hwnd, 0);
            return 0;
        case IDC_EDIT_R:
            if (HIWORD(wp) == EN_CHANGE && !g_suppressEditNotify)
                SetFreqFromEdit(hwnd, 1);
            return 0;
        case IDC_BTN_PLAY:
            if (g_isPlaying) StopPlayback(FALSE);
            else             StartPlayback();
            UpdateStatus(hwnd);
            return 0;
        case IDC_BTN_SAVE:
            DoSaveWav(hwnd);
            return 0;
        }
        return 0;

    case WM_APP + 1:            /* audio thread finished */
        UpdateStatus(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_CLOSE:
        StopPlayback(TRUE);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow)
{
    WNDCLASSEXA wc;
    INITCOMMONCONTROLSEX icc;
    HWND hwnd;
    MSG  msg;
    RECT rc = { 0, 0, 500, 262 };
    NONCLIENTMETRICSA ncm;

    (void)hPrev; (void)cmdLine;

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        g_font = CreateFontIndirectA(&ncm.lfMessageFont);
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_audioEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "DualToneGenWnd";
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm       = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExA(&wc);

    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    hwnd = CreateWindowExA(0, "DualToneGenWnd", "Dual Tone Generator",
                           WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top,
                           NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessage(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    StopPlayback(TRUE);
    if (g_audioEvent) CloseHandle(g_audioEvent);
    return (int)msg.wParam;
}
