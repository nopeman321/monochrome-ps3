/**
 * Monochrome Music Player — Host PC (Windows) Platform Implementation
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

/* ── Lifecycle & Timers ──────────────────────────────────────────── */
int platform_init(void) {
    /* Set console UTF-8 output */
    SetConsoleOutputCP(CP_UTF8);

    /* Hide cursor for clean TUI rendering */
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hConsole, &cursorInfo);
    cursorInfo.bVisible = FALSE;
    SetConsoleCursorInfo(hConsole, &cursorInfo);

    /* Initialize WinHTTP */
    s_http_session = WinHttpOpen(
        L"Monochrome-Host/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    return (s_http_session != NULL) ? 0 : -1;
}

void platform_shutdown(void) {
    platform_audio_shutdown();

    if (s_http_session) {
        WinHttpCloseHandle(s_http_session);
        s_http_session = NULL;
    }

    /* Restore cursor */
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hConsole, &cursorInfo);
    cursorInfo.bVisible = TRUE;
    SetConsoleCursorInfo(hConsole, &cursorInfo);
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

    /* Parse Host and Path */
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;

    char host[256] = {0};
    char path[1024] = {0};
    const char *slash = strchr(p, '/');

    if (slash) {
        size_t host_len = slash - p;
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        memcpy(host, p, host_len);
        snprintf(path, sizeof(path), "%s", slash);
    } else {
        snprintf(host, sizeof(host), "%s", p);
        snprintf(path, sizeof(path), "/");
    }

    wchar_t wHost[256];
    wchar_t wPath[1024];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, wHost, 256);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wPath, 1024);

    HINTERNET hConnect = WinHttpConnect(s_http_session, wHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) return -2;

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        wPath,
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        return -3;
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

/* ── Keyboard Input Polling ──────────────────────────────────────── */
InputAction platform_poll_input(void) {
    if (!_kbhit()) return INPUT_NONE;

    int ch = _getch();
    if (ch == 0 || ch == 224) { /* Extended keys (arrow keys) */
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

/* ── Console / Terminal UI ───────────────────────────────────────── */
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
