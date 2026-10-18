/**
 * Monochrome Music Player — Host PC (Windows) Platform Implementation
 *
 * Provides:
 *   - Native Win32 GDI 1280x720 60 FPS Visual GUI with double-buffering
 *   - Live HTTPS API & FLAC stream fetching via Windows WinHTTP
 *   - Hardware DAC 48 kHz LPCM stereo audio playback via Windows waveOut
 *   - Mouse & keyboard input polling (Window Messages + Console)
 */

#if !defined(__powerpc__) && !defined(__CELLOS_LV2__)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <mmsystem.h>
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "monochrome.h"
#include "platform.h"

/* ── WinHTTP Session ─────────────────────────────────────────────── */
static HINTERNET s_http_session = NULL;

/* ── Audio Output State (waveOut) ────────────────────────────────── */
#define NUM_AUDIO_BUFFERS 4
#define AUDIO_BUF_FRAMES  1024

static HWAVEOUT  s_wave_out = NULL;
static WAVEHDR   s_wave_headers[NUM_AUDIO_BUFFERS];
static int16_t   s_audio_buffers[NUM_AUDIO_BUFFERS][AUDIO_BUF_FRAMES * 2];
static int       s_buf_index = 0;
static float     s_volume = 1.0f;

/* ── Win32 Window & GDI State ────────────────────────────────────── */
static HWND         s_hwnd          = NULL;
static HDC          s_mem_dc        = NULL;
static HBITMAP      s_mem_bmp       = NULL;
static HBITMAP      s_old_bmp       = NULL;
static int          s_win_width     = 1280;
static int          s_win_height    = 720;
static bool         s_window_open   = false;

/* Cached GDI Fonts */
static HFONT        s_font_title    = NULL;
static HFONT        s_font_sub      = NULL;
static HFONT        s_font_heading  = NULL;
static HFONT        s_font_track    = NULL;
static HFONT        s_font_meta     = NULL;
static HFONT        s_font_mono     = NULL;

/* Cached GDI Brushes */
static HBRUSH       s_br_bg         = NULL;
static HBRUSH       s_br_row_even   = NULL;
static HBRUSH       s_br_row_odd    = NULL;
static HBRUSH       s_br_highlight  = NULL;
static HBRUSH       s_br_accent     = NULL;
static HBRUSH       s_br_dock       = NULL;
static HBRUSH       s_br_bar_bg     = NULL;
static HBRUSH       s_br_bar_fill   = NULL;
static HBRUSH       s_br_card       = NULL;

/* Cached GDI Pens */
static HPEN         s_pen_border    = NULL;
static HPEN         s_pen_highlight = NULL;
static HPEN         s_pen_accent    = NULL;
static HPEN         s_pen_dim       = NULL;

/* Input Queues */
static InputAction  s_queued_action = INPUT_NONE;
static int          s_mouse_clicked_track = -1;

/* ── Window Procedure ────────────────────────────────────────────── */
static LRESULT CALLBACK PlatformWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_KEYDOWN: {
            switch (wParam) {
                case VK_UP:     s_queued_action = INPUT_UP; break;
                case VK_DOWN:   s_queued_action = INPUT_DOWN; break;
                case VK_LEFT:   s_queued_action = INPUT_LEFT; break;
                case VK_RIGHT:  s_queued_action = INPUT_RIGHT; break;
                case VK_RETURN: s_queued_action = INPUT_SELECT; break;
                case VK_SPACE:  s_queued_action = INPUT_PLAY_PAUSE; break;
                case 'S':       s_queued_action = INPUT_ACTION_SEARCH; break;
                case VK_ESCAPE:
                case 'Q':       s_queued_action = INPUT_QUIT; break;
                default: break;
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int mx = LOWORD(lParam);
            int my = HIWORD(lParam);

            /* Track rows clicked */
            if (my >= 155 && my < 155 + 10 * 43) {
                int row = (my - 155) / 43;
                s_mouse_clicked_track = row;
            }
            /* Dock play/pause area clicked */
            else if (my >= 600 && my <= 710 && mx >= 350 && mx <= 920) {
                s_queued_action = INPUT_PLAY_PAUSE;
            }
            /* Search banner clicked */
            else if (my >= 85 && my <= 125 && mx >= 30 && mx <= 400) {
                s_queued_action = INPUT_ACTION_SEARCH;
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (s_mem_dc) {
                BitBlt(hdc, 0, 0, s_win_width, s_win_height, s_mem_dc, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CLOSE: {
            s_queued_action = INPUT_QUIT;
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            s_window_open = false;
            PostQuitMessage(0);
            return 0;
        }

        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

/* ── Lifecycle & Timers ──────────────────────────────────────────── */
int platform_init(void) {
    SetConsoleOutputCP(CP_UTF8);

    /* Initialize WinHTTP */
    s_http_session = WinHttpOpen(
        L"Monochrome-Host/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (s_http_session) {
        WinHttpSetTimeouts(s_http_session, 3000, 3000, 5000, 8000);
    }

    return (s_http_session != NULL) ? 0 : -1;
}

void platform_shutdown(void) {
    platform_gfx_shutdown();
    platform_audio_shutdown();

    if (s_http_session) {
        WinHttpCloseHandle(s_http_session);
        s_http_session = NULL;
    }
}

void platform_sleep_ms(uint32_t ms) {
    Sleep(ms);
}

uint64_t platform_get_time_ms(void) {
    return (uint64_t)GetTickCount64();
}

/* ── WinHTTP GET Implementation ──────────────────────────────────── */
int platform_http_get(const char *url, char **response_out, size_t *size_out) {
    if (!url || !response_out || !size_out) return -1;
    *response_out = NULL;
    *size_out = 0;

    if (!s_http_session) {
        s_http_session = WinHttpOpen(L"Monochrome/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
        if (!s_http_session) return -1;
    }

    /* Parse Host, Port, and Path */
    const char *p = url;
    bool is_https = true;
    INTERNET_PORT default_port = INTERNET_DEFAULT_HTTPS_PORT;
    if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        is_https = true;
        default_port = INTERNET_DEFAULT_HTTPS_PORT;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        is_https = false;
        default_port = INTERNET_DEFAULT_HTTP_PORT;
    }

    char host[256] = {0};
    char path[1024] = {0};
    const char *slash = strchr(p, '/');

    if (slash) {
        size_t host_len = slash - p;
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        memcpy(host, p, host_len);
        host[host_len] = '\0';
        snprintf(path, sizeof(path), "%s", slash);
    } else {
        snprintf(host, sizeof(host), "%s", p);
        snprintf(path, sizeof(path), "/");
    }

    /* Check for custom port in host: e.g. 127.0.0.1:8080 */
    INTERNET_PORT port = default_port;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        port = (INTERNET_PORT)atoi(colon + 1);
    }

    wchar_t wHost[256];
    wchar_t wPath[1024];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, wHost, 256);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wPath, 1024);

    HINTERNET hConnect = WinHttpConnect(s_http_session, wHost, port, 0);
    if (!hConnect) return -2;

    DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        wPath,
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags
    );
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        return -3;
    }

    /* Ignore certificate errors for self-signed or proxy servers */
    if (is_https) {
        DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &sec_flags, sizeof(sec_flags));
    }

    BOOL bRes = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (bRes) bRes = WinHttpReceiveResponse(hRequest, NULL);

    if (!bRes) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        return -4;
    }

    /* Read stream chunks */
    size_t capacity = 64 * 1024;
    size_t total = 0;
    char *buf = (char *)malloc(capacity);
    if (!buf) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        return -5;
    }

    DWORD dwRead = 0;
    do {
        DWORD dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;

        if (total + dwSize + 1 > capacity) {
            capacity = (total + dwSize + 1) * 2;
            char *new_buf = (char *)realloc(buf, capacity);
            if (!new_buf) { free(buf); buf = NULL; break; }
            buf = new_buf;
        }

        if (!WinHttpReadData(hRequest, buf + total, dwSize, &dwRead)) break;
        total += dwRead;
    } while (dwRead > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);

    if (!buf) return -6;

    buf[total] = '\0';
    *response_out = buf;
    *size_out = total;
    return 0;
}

void platform_http_free(char *response) {
    if (response) free(response);
}

/* ── Audio Output via Windows waveOut ────────────────────────────── */
int platform_audio_init(uint32_t sample_rate, uint8_t channels) {
    platform_audio_shutdown();

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = channels;
    wfx.nSamplesPerSec  = sample_rate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = channels * 2;
    wfx.nAvgBytesPerSec = sample_rate * wfx.nBlockAlign;

    MMRESULT res = waveOutOpen(&s_wave_out, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (res != MMSYSERR_NOERROR) {
        s_wave_out = NULL;
        return -1;
    }

    memset(s_wave_headers, 0, sizeof(s_wave_headers));
    memset(s_audio_buffers, 0, sizeof(s_audio_buffers));
    s_buf_index = 0;

    return 0;
}

void platform_audio_shutdown(void) {
    if (s_wave_out) {
        waveOutReset(s_wave_out);
        for (int i = 0; i < NUM_AUDIO_BUFFERS; i++) {
            if (s_wave_headers[i].dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(s_wave_out, &s_wave_headers[i], sizeof(WAVEHDR));
            }
        }
        waveOutClose(s_wave_out);
        s_wave_out = NULL;
    }
}

void platform_audio_write(const int16_t *samples, size_t frames) {
    if (!s_wave_out || !samples || frames == 0) return;

    WAVEHDR *hdr = &s_wave_headers[s_buf_index];

    /* Wait if buffer is still playing */
    if (hdr->dwFlags & WHDR_PREPARED) {
        while (!(hdr->dwFlags & WHDR_DONE)) {
            Sleep(2);
        }
        waveOutUnprepareHeader(s_wave_out, hdr, sizeof(WAVEHDR));
    }

    size_t copy_frames = (frames > AUDIO_BUF_FRAMES) ? AUDIO_BUF_FRAMES : frames;
    int16_t *dst = s_audio_buffers[s_buf_index];

    /* Apply volume scale */
    if (s_volume < 0.99f) {
        for (size_t i = 0; i < copy_frames * 2; i++) {
            dst[i] = (int16_t)(samples[i] * s_volume);
        }
    } else {
        memcpy(dst, samples, copy_frames * 2 * sizeof(int16_t));
    }

    memset(hdr, 0, sizeof(WAVEHDR));
    hdr->lpData         = (LPSTR)dst;
    hdr->dwBufferLength = (DWORD)(copy_frames * 2 * sizeof(int16_t));

    waveOutPrepareHeader(s_wave_out, hdr, sizeof(WAVEHDR));
    waveOutWrite(s_wave_out, hdr, sizeof(WAVEHDR));

    s_buf_index = (s_buf_index + 1) % NUM_AUDIO_BUFFERS;
}

void platform_audio_set_volume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    s_volume = vol;
}

/* ── Win32 Visual Window & GDI Renderer ──────────────────────────── */
int platform_gfx_init(int width, int height, const char *title) {
    s_win_width  = width;
    s_win_height = height;

    HINSTANCE hInstance = GetModuleHandle(NULL);

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(WNDCLASSEXA);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = PlatformWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "MonochromeWinClass";

    RegisterClassExA(&wc);

    RECT rc = { 0, 0, width, height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    int real_w = rc.right - rc.left;
    int real_h = rc.bottom - rc.top;

    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int pos_x = (screen_w - real_w) / 2;
    int pos_y = (screen_h - real_h) / 2;

    s_hwnd = CreateWindowExA(
        0,
        "MonochromeWinClass",
        title ? title : "Monochrome Music Player",
        WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
        pos_x, pos_y, real_w, real_h,
        NULL, NULL, hInstance, NULL
    );

    if (!s_hwnd) {
        printf("[GFX] Failed to create Win32 window\n");
        return -1;
    }

    HDC hdc = GetDC(s_hwnd);
    s_mem_dc = CreateCompatibleDC(hdc);
    s_mem_bmp = CreateCompatibleBitmap(hdc, width, height);
    s_old_bmp = (HBITMAP)SelectObject(s_mem_dc, s_mem_bmp);
    ReleaseDC(s_hwnd, hdc);

    /* Create Typography Fonts */
    s_font_title   = CreateFontA(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    s_font_sub     = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    s_font_heading = CreateFontA(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    s_font_track   = CreateFontA(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    s_font_meta    = CreateFontA(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    s_font_mono    = CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");

    /* Create UI Brushes */
    s_br_bg        = CreateSolidBrush(RGB(14, 15, 18));
    s_br_row_even  = CreateSolidBrush(RGB(17, 18, 22));
    s_br_row_odd   = CreateSolidBrush(RGB(22, 23, 29));
    s_br_highlight = CreateSolidBrush(RGB(38, 42, 54));
    s_br_accent    = CreateSolidBrush(RGB(255, 255, 255));
    s_br_dock      = CreateSolidBrush(RGB(22, 24, 30));
    s_br_bar_bg    = CreateSolidBrush(RGB(45, 48, 58));
    s_br_bar_fill  = CreateSolidBrush(RGB(255, 255, 255));
    s_br_card      = CreateSolidBrush(RGB(28, 30, 38));

    /* Create UI Pens */
    s_pen_border    = CreatePen(PS_SOLID, 1, RGB(48, 52, 64));
    s_pen_highlight = CreatePen(PS_SOLID, 1, RGB(100, 130, 200));
    s_pen_accent    = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    s_pen_dim       = CreatePen(PS_SOLID, 1, RGB(32, 34, 42));

    ShowWindow(s_hwnd, SW_SHOW);
    UpdateWindow(s_hwnd);
    s_window_open = true;

    printf("[GFX] Win32 1280x720 Native Double-Buffered GDI Window Initialized\n");
    return 0;
}

void platform_gfx_shutdown(void) {
    if (s_mem_dc) {
        if (s_old_bmp) SelectObject(s_mem_dc, s_old_bmp);
        if (s_mem_bmp) DeleteObject(s_mem_bmp);
        DeleteDC(s_mem_dc);
        s_mem_dc = NULL;
        s_mem_bmp = NULL;
    }

    if (s_font_title)   DeleteObject(s_font_title);
    if (s_font_sub)     DeleteObject(s_font_sub);
    if (s_font_heading) DeleteObject(s_font_heading);
    if (s_font_track)   DeleteObject(s_font_track);
    if (s_font_meta)    DeleteObject(s_font_meta);
    if (s_font_mono)    DeleteObject(s_font_mono);

    if (s_br_bg)        DeleteObject(s_br_bg);
    if (s_br_row_even)  DeleteObject(s_br_row_even);
    if (s_br_row_odd)   DeleteObject(s_br_row_odd);
    if (s_br_highlight) DeleteObject(s_br_highlight);
    if (s_br_accent)    DeleteObject(s_br_accent);
    if (s_br_dock)      DeleteObject(s_br_dock);
    if (s_br_bar_bg)    DeleteObject(s_br_bar_bg);
    if (s_br_bar_fill)  DeleteObject(s_br_bar_fill);
    if (s_br_card)      DeleteObject(s_br_card);

    if (s_pen_border)    DeleteObject(s_pen_border);
    if (s_pen_highlight) DeleteObject(s_pen_highlight);
    if (s_pen_accent)    DeleteObject(s_pen_accent);
    if (s_pen_dim)       DeleteObject(s_pen_dim);

    if (s_hwnd) {
        DestroyWindow(s_hwnd);
        s_hwnd = NULL;
    }
}

void platform_gfx_render(int state, const struct TrackMetadata *track,
                         int pstate, float progress,
                         const struct TrackMetadata *tracks, int track_count, int cursor,
                         const char *search_query) {
    if (!s_mem_dc || !s_window_open) return;

    SetBkMode(s_mem_dc, TRANSPARENT);

    /* 1. Clear background */
    RECT r_all = { 0, 0, s_win_width, s_win_height };
    FillRect(s_mem_dc, &r_all, s_br_bg);

    /* 2. Top Header Bar (y: 0 - 80) */
    RECT r_head = { 0, 0, s_win_width, 80 };
    FillRect(s_mem_dc, &r_head, s_br_dock);

    /* Title Logo */
    SelectObject(s_mem_dc, s_font_title);
    SetTextColor(s_mem_dc, RGB(255, 255, 255));
    TextOutA(s_mem_dc, 35, 15, "M O N O C H R O M E", 19);

    /* Subtitle */
    SelectObject(s_mem_dc, s_font_sub);
    SetTextColor(s_mem_dc, RGB(160, 164, 178));
    TextOutA(s_mem_dc, 37, 48, "LOSSLESS FLAC STREAMER · TIDAL HI-RES · CROSS-PLATFORM PS3 & PC ENGINE", 70);

    /* Top Badges (Right side) */
    SelectObject(s_mem_dc, s_font_mono);
    RECT r_badge1 = { 880, 24, 1050, 52 };
    FillRect(s_mem_dc, &r_badge1, s_br_card);
    FrameRect(s_mem_dc, &r_badge1, s_br_highlight);
    SetTextColor(s_mem_dc, RGB(180, 210, 255));
    DrawTextA(s_mem_dc, "24-BIT / 96 KHZ", -1, &r_badge1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT r_badge2 = { 1065, 24, 1245, 52 };
    FillRect(s_mem_dc, &r_badge2, s_br_card);
    FrameRect(s_mem_dc, &r_badge2, s_br_highlight);
    SetTextColor(s_mem_dc, RGB(100, 240, 140));
    DrawTextA(s_mem_dc, "● ONLINE: MONOCHROME.ST", -1, &r_badge2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* Header divider line */
    SelectObject(s_mem_dc, s_pen_border);
    MoveToEx(s_mem_dc, 0, 80, NULL);
    LineTo(s_mem_dc, s_win_width, 80);

    /* 3. Section Title & Search Banner (y: 92 - 138) */
    char cat_str[256];
    snprintf(cat_str, sizeof(cat_str), "CATALOGUE: %s  (%d tracks loaded)",
             search_query ? search_query : "Trending", track_count);
    SelectObject(s_mem_dc, s_font_heading);
    SetTextColor(s_mem_dc, RGB(255, 255, 255));
    TextOutA(s_mem_dc, 35, 94, cat_str, (int)strlen(cat_str));

    SelectObject(s_mem_dc, s_font_sub);
    SetTextColor(s_mem_dc, RGB(140, 145, 160));
    TextOutA(s_mem_dc, 900, 96, "Press [S] or Click here to cycle genres", 39);

    /* Column Headers (y: 125) */
    SelectObject(s_mem_dc, s_font_mono);
    SetTextColor(s_mem_dc, RGB(120, 125, 140));
    TextOutA(s_mem_dc, 45,   126, "#", 1);
    TextOutA(s_mem_dc, 95,   126, "TITLE", 5);
    TextOutA(s_mem_dc, 480,  126, "ARTIST", 6);
    TextOutA(s_mem_dc, 760,  126, "ALBUM", 5);
    TextOutA(s_mem_dc, 1040, 126, "QUALITY", 7);
    TextOutA(s_mem_dc, 1180, 126, "TIME", 4);

    SelectObject(s_mem_dc, s_pen_dim);
    MoveToEx(s_mem_dc, 35, 146, NULL);
    LineTo(s_mem_dc, 1245, 146);

    /* 4. Track List Rows (y: 152 to 585) */
    int max_rows = (track_count > 10) ? 10 : track_count;
    for (int i = 0; i < max_rows; i++) {
        int row_y = 152 + i * 43;
        RECT r_row = { 35, row_y, 1245, row_y + 40 };

        bool is_sel = (i == cursor);

        if (is_sel) {
            FillRect(s_mem_dc, &r_row, s_br_highlight);
            FrameRect(s_mem_dc, &r_row, s_br_accent);

            /* Left Accent Indicator Strip */
            RECT r_strip = { 35, row_y, 40, row_y + 40 };
            FillRect(s_mem_dc, &r_strip, s_br_accent);
        } else {
            FillRect(s_mem_dc, &r_row, (i % 2 == 0) ? s_br_row_even : s_br_row_odd);
        }

        /* Track Number / Play icon */
        SelectObject(s_mem_dc, s_font_mono);
        SetTextColor(s_mem_dc, is_sel ? RGB(255, 255, 255) : RGB(140, 145, 160));
        char num_buf[16];
        if (is_sel) snprintf(num_buf, sizeof(num_buf), "▶%2d", i + 1);
        else        snprintf(num_buf, sizeof(num_buf), " %2d", i + 1);
        TextOutA(s_mem_dc, 45, row_y + 11, num_buf, (int)strlen(num_buf));

        /* Title */
        SelectObject(s_mem_dc, s_font_track);
        SetTextColor(s_mem_dc, is_sel ? RGB(255, 255, 255) : RGB(225, 228, 235));
        RECT r_title = { 95, row_y, 460, row_y + 40 };
        DrawTextA(s_mem_dc, tracks[i].title, -1, &r_title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        /* Artist */
        SelectObject(s_mem_dc, s_font_meta);
        SetTextColor(s_mem_dc, is_sel ? RGB(210, 215, 230) : RGB(160, 165, 178));
        RECT r_artist = { 480, row_y, 740, row_y + 40 };
        DrawTextA(s_mem_dc, tracks[i].artist, -1, &r_artist, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        /* Album */
        SetTextColor(s_mem_dc, is_sel ? RGB(180, 185, 200) : RGB(130, 134, 146));
        RECT r_album = { 760, row_y, 1020, row_y + 40 };
        DrawTextA(s_mem_dc, tracks[i].album, -1, &r_album, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        /* Quality Badge */
        SelectObject(s_mem_dc, s_font_mono);
        RECT r_qbadge = { 1040, row_y + 9, 1140, row_y + 31 };
        FillRect(s_mem_dc, &r_qbadge, s_br_card);
        FrameRect(s_mem_dc, &r_qbadge, s_br_highlight);
        SetTextColor(s_mem_dc, tracks[i].is_hi_res ? RGB(255, 215, 100) : RGB(160, 220, 255));
        DrawTextA(s_mem_dc, tracks[i].is_hi_res ? "HI-RES" : "FLAC", -1, &r_qbadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        /* Duration */
        char time_buf[16];
        uint32_t dur = tracks[i].duration_sec;
        snprintf(time_buf, sizeof(time_buf), "%02u:%02u", dur / 60, dur % 60);
        SetTextColor(s_mem_dc, is_sel ? RGB(255, 255, 255) : RGB(150, 155, 168));
        TextOutA(s_mem_dc, 1180, row_y + 11, time_buf, (int)strlen(time_buf));
    }

    /* 5. Bottom Elevated Now-Playing Dock (y: 595 - 715) */
    RECT r_dock = { 0, 595, s_win_width, s_win_height };
    FillRect(s_mem_dc, &r_dock, s_br_dock);

    SelectObject(s_mem_dc, s_pen_border);
    MoveToEx(s_mem_dc, 0, 595, NULL);
    LineTo(s_mem_dc, s_win_width, 595);

    /* Cover Art Square */
    RECT r_art = { 35, 610, 115, 690 };
    FillRect(s_mem_dc, &r_art, s_br_card);
    FrameRect(s_mem_dc, &r_art, s_br_highlight);
    SelectObject(s_mem_dc, s_font_heading);
    SetTextColor(s_mem_dc, RGB(255, 255, 255));
    DrawTextA(s_mem_dc, "♫", -1, &r_art, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* Track metadata */
    const char *now_t = (track && track->title[0] != '\0') ? track->title : "No Track Selected";
    const char *now_a = (track && track->artist[0] != '\0') ? track->artist : "Select a track from above to begin lossless streaming";

    SelectObject(s_mem_dc, s_font_track);
    SetTextColor(s_mem_dc, RGB(255, 255, 255));
    RECT r_now_t = { 130, 615, 420, 642 };
    DrawTextA(s_mem_dc, now_t, -1, &r_now_t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(s_mem_dc, s_font_meta);
    SetTextColor(s_mem_dc, RGB(160, 165, 178));
    RECT r_now_a = { 130, 644, 420, 668 };
    DrawTextA(s_mem_dc, now_a, -1, &r_now_a, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* Center Controls & Progress Bar (x: 440 - 920) */
    SelectObject(s_mem_dc, s_font_mono);
    if (pstate == PLAYBACK_PLAYING) {
        SetTextColor(s_mem_dc, RGB(100, 240, 140));
        TextOutA(s_mem_dc, 440, 616, "▶ PLAYING (48,000 Hz LPCM DAC)", 30);
    } else if (pstate == PLAYBACK_PAUSED) {
        SetTextColor(s_mem_dc, RGB(255, 200, 80));
        TextOutA(s_mem_dc, 440, 616, "⏸ PAUSED", 8);
    } else {
        SetTextColor(s_mem_dc, RGB(140, 145, 160));
        TextOutA(s_mem_dc, 440, 616, "⏹ READY", 7);
    }

    /* Progress bar track */
    RECT r_prog_bg = { 440, 648, 920, 656 };
    FillRect(s_mem_dc, &r_prog_bg, s_br_bar_bg);

    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    int fill_w = (int)(progress * (920 - 440));
    RECT r_prog_fill = { 440, 648, 440 + fill_w, 656 };
    FillRect(s_mem_dc, &r_prog_fill, s_br_bar_fill);

    /* Progress elapsed / total */
    uint32_t total_sec = (track) ? track->duration_sec : 0;
    uint32_t cur_sec = (uint32_t)(progress * (float)total_sec);
    char cur_time_str[32];
    snprintf(cur_time_str, sizeof(cur_time_str), "%02u:%02u / %02u:%02u",
             cur_sec / 60, cur_sec % 60, total_sec / 60, total_sec % 60);
    SetTextColor(s_mem_dc, RGB(180, 185, 200));
    TextOutA(s_mem_dc, 440, 665, cur_time_str, (int)strlen(cur_time_str));

    /* Right Keybindings & Engine Specs (x: 950 - 1245) */
    SelectObject(s_mem_dc, s_font_mono);
    SetTextColor(s_mem_dc, RGB(120, 180, 255));
    TextOutA(s_mem_dc, 950, 615, "DAC: waveOut 48 kHz / 16-bit", 28);
    TextOutA(s_mem_dc, 950, 633, "BUFFER: 8.0 MB XDR Ring Buffer", 30);

    SetTextColor(s_mem_dc, RGB(160, 165, 180));
    TextOutA(s_mem_dc, 950, 655, "[▲/▼] Navigate | [ENTER] Play", 29);
    TextOutA(s_mem_dc, 950, 672, "[SPACE] Pause  | [S] Cycle Genre", 32);
    TextOutA(s_mem_dc, 950, 689, "[CLICK] Select | [ESC] Exit", 27);

    /* 6. Blit from memory DC to Window DC */
    HDC hdc = GetDC(s_hwnd);
    BitBlt(hdc, 0, 0, s_win_width, s_win_height, s_mem_dc, 0, 0, SRCCOPY);
    ReleaseDC(s_hwnd, hdc);

    (void)state;
}

/* ── Mouse & Keyboard Input Polling ──────────────────────────────── */
InputAction platform_poll_input(void) {
    /* 1. Pump Windows Messages */
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    /* 2. Return queued action from window events if available */
    if (s_queued_action != INPUT_NONE) {
        InputAction act = s_queued_action;
        s_queued_action = INPUT_NONE;
        return act;
    }

    /* 3. Fallback to console keyboard polling */
    if (!_kbhit()) return INPUT_NONE;

    int ch = _getch();
    if (ch == 0 || ch == 224) { /* Extended keys */
        int code = _getch();
        switch (code) {
            case 72: return INPUT_UP;
            case 80: return INPUT_DOWN;
            case 75: return INPUT_LEFT;
            case 77: return INPUT_RIGHT;
            default: return INPUT_NONE;
        }
    }

    switch (ch) {
        case 13: return INPUT_SELECT;        /* Enter */
        case 27: return INPUT_QUIT;          /* ESC */
        case ' ': return INPUT_PLAY_PAUSE;   /* Space */
        case 's': case 'S': case '/': return INPUT_ACTION_SEARCH;
        case 'q': case 'Q': return INPUT_QUIT;
        case 'w': case 'W': return INPUT_UP;
        case 'z': case 'Z': return INPUT_DOWN;
        default: return INPUT_NONE;
    }
}

int platform_get_mouse_track_selection(void) {
    int sel = s_mouse_clicked_track;
    s_mouse_clicked_track = -1;
    return sel;
}

/* ── Console / Terminal UI Stubs ─────────────────────────────────── */
void platform_clear_screen(void) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD coordScreen = { 0, 0 };
    DWORD cCharsWritten;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD dwConSize;

    if (!GetConsoleScreenBufferInfo(hConsole, &csbi)) return;
    dwConSize = csbi.dwSize.X * csbi.dwSize.Y;

    FillConsoleOutputCharacter(hConsole, (TCHAR)' ', dwConSize, coordScreen, &cCharsWritten);
    GetConsoleScreenBufferInfo(hConsole, &csbi);
    FillConsoleOutputAttribute(hConsole, csbi.wAttributes, dwConSize, coordScreen, &cCharsWritten);
    SetConsoleCursorPosition(hConsole, coordScreen);
}

void platform_set_cursor(int row, int col) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD coord = { (SHORT)col, (SHORT)row };
    SetConsoleCursorPosition(hConsole, coord);
}

#endif /* !PLATFORM_IS_PS3 */
