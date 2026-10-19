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
#include <wchar.h>
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
static HBRUSH       s_br_sidebar    = NULL;
static HBRUSH       s_br_row_even   = NULL;
static HBRUSH       s_br_row_odd    = NULL;
static HBRUSH       s_br_row_hover  = NULL;
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

/* Cached Cursors */
static HCURSOR      s_cur_arrow     = NULL;
static HCURSOR      s_cur_hand      = NULL;
static HCURSOR      s_cur_ibeam     = NULL;

/* Input & Interaction State */
static InputAction  s_queued_action = INPUT_NONE;
static int          s_mouse_clicked_track   = -1;
static int          s_current_state         = 0;
static int          s_scroll_offset         = 0;
static bool         s_search_focused        = false;
static int          s_hover_track           = -1;
static int          s_hover_btn             = 0; /* 1=shuf, 2=prev, 3=pp, 4=next, 5=rep, 7=clear */
static bool         s_dragging_scrubber     = false;
static bool         s_dragging_volume       = false;
static float        s_total_duration_cached = 0.0f;
static int          s_cached_track_count    = 0;

/* ── Unicode GDI Drawing Helpers (Zero mojibake / Clean UTF-8) ──── */
static void gdi_draw_text_utf8(HDC hdc, const char *utf8_str, RECT *rc, UINT format) {
    if (!hdc || !utf8_str || !rc) return;
    wchar_t wbuf[512];
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, wbuf, (int)(sizeof(wbuf)/sizeof(wbuf[0])));
    if (wlen > 0) {
        DrawTextW(hdc, wbuf, -1, rc, format);
    }
}

static void gdi_text_out_utf8(HDC hdc, int x, int y, const char *utf8_str) {
    if (!hdc || !utf8_str) return;
    wchar_t wbuf[512];
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, wbuf, (int)(sizeof(wbuf)/sizeof(wbuf[0])));
    if (wlen > 1) {
        TextOutW(hdc, x, y, wbuf, wlen - 1);
    }
}

/* ── Window Procedure ────────────────────────────────────────────── */
static LRESULT CALLBACK PlatformWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);

            /* Search input box */
            if (pt.x >= 335 && pt.x <= 780 && pt.y >= 18 && pt.y <= 56) {
                if (s_cur_ibeam) SetCursor(s_cur_ibeam);
                return TRUE;
            }
            /* Clickable areas: Sidebar, Track Rows, Dock buttons, Progress, Volume */
            if (pt.x <= 230 ||
                (pt.x >= 245 && pt.x <= 1250 && pt.y >= 150 && pt.y <= 590) ||
                (pt.y >= 625 && pt.y <= 720)) {
                if (s_cur_hand) SetCursor(s_cur_hand);
                return TRUE;
            }
            if (s_cur_arrow) SetCursor(s_cur_arrow);
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            int mx = LOWORD(lParam);
            int my = HIWORD(lParam);

            /* Scrubber drag */
            if (s_dragging_scrubber && (wParam & MK_LBUTTON)) {
                float p = (float)(mx - 420) / 420.0f;
                if (p < 0.0f) p = 0.0f;
                if (p > 1.0f) p = 1.0f;
                audio_seek_progress(p, (uint32_t)s_total_duration_cached);
            }
            /* Volume drag */
            if (s_dragging_volume && (wParam & MK_LBUTTON)) {
                float v = (float)(mx - 1105) / 90.0f;
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                platform_audio_set_volume(v);
            }

            /* Hover track detection */
            if (mx >= 245 && mx <= 1250 && my >= 150 && my < 150 + 10 * 44) {
                s_hover_track = s_scroll_offset + (my - 150) / 44;
            } else {
                s_hover_track = -1;
            }

            /* Hover buttons */
            s_hover_btn = 0;
            if (my >= 628 && my <= 668) {
                if (mx >= 515 && mx <= 545) s_hover_btn = 1;      /* Shuffle */
                else if (mx >= 560 && mx <= 590) s_hover_btn = 2; /* Prev */
                else if (mx >= 605 && mx <= 655) s_hover_btn = 3; /* Play/Pause */
                else if (mx >= 660 && mx <= 690) s_hover_btn = 4; /* Next */
                else if (mx >= 705 && mx <= 735) s_hover_btn = 5; /* Repeat */
            } else if (mx >= 750 && mx <= 778 && my >= 18 && my <= 56) {
                s_hover_btn = 7; /* Clear search */
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int mx = LOWORD(lParam);
            int my = HIWORD(lParam);

            /* 1. Search Box Click */
            if (mx >= 335 && mx <= 780 && my >= 18 && my <= 56) {
                if (mx >= 750 && mx <= 778) {
                    /* Clear button */
                    ui_set_search_query("");
                    s_search_focused = true;
                } else {
                    s_search_focused = true;
                }
                return 0;
            } else {
                s_search_focused = false;
            }

            /* 2. Sidebar Navigation & Presets */
            if (mx <= 230) {
                if (my >= 115 && my <= 145) {
                    /* Inicio */
                    ui_set_search_query("trending");
                } else if (my >= 150 && my <= 180) {
                    /* Búsqueda */
                    s_search_focused = true;
                } else if (my >= 290 && my <= 470) {
                    /* Quick genre tags */
                    static const char *s_tags[] = {
                        "trending", "Daft Punk", "The Weeknd",
                        "Synthwave", "Lossless", "Electronic"
                    };
                    int tag_idx = (my - 290) / 30;
                    if (tag_idx >= 0 && tag_idx < 6) {
                        ui_set_search_query(s_tags[tag_idx]);
                    }
                }
                return 0;
            }

            /* 3. Catalog Track Selection */
            if (mx >= 245 && mx <= 1250 && my >= 150 && my < 150 + 10 * 44) {
                int clicked_row = s_scroll_offset + (my - 150) / 44;
                s_mouse_clicked_track = clicked_row;
                return 0;
            }

            /* 4. Now Playing Elevated Dock */
            if (my >= 625 && my <= 720) {
                /* Transport Buttons */
                if (mx >= 515 && mx <= 545) {
                    s_queued_action = INPUT_SHUFFLE;
                } else if (mx >= 560 && mx <= 590) {
                    s_queued_action = INPUT_PREV_TRACK;
                } else if (mx >= 605 && mx <= 655) {
                    s_queued_action = INPUT_PLAY_PAUSE;
                } else if (mx >= 660 && mx <= 690) {
                    s_queued_action = INPUT_NEXT_TRACK;
                } else if (mx >= 705 && mx <= 735) {
                    s_queued_action = INPUT_REPEAT;
                }
                /* Progress Bar Scrubber */
                else if (mx >= 420 && mx <= 840 && my >= 675 && my <= 695) {
                    s_dragging_scrubber = true;
                    float p = (float)(mx - 420) / 420.0f;
                    if (p < 0.0f) p = 0.0f;
                    if (p > 1.0f) p = 1.0f;
                    audio_seek_progress(p, (uint32_t)s_total_duration_cached);
                }
                /* Volume speaker (mute toggle) */
                else if (mx >= 1075 && mx <= 1100 && my >= 660 && my <= 686) {
                    platform_audio_set_volume((s_volume > 0.01f) ? 0.0f : 1.0f);
                }
                /* Volume track */
                else if (mx >= 1105 && mx <= 1195 && my >= 660 && my <= 686) {
                    s_dragging_volume = true;
                    float v = (float)(mx - 1105) / 90.0f;
                    if (v < 0.0f) v = 0.0f;
                    if (v > 1.0f) v = 1.0f;
                    platform_audio_set_volume(v);
                }
                return 0;
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            s_dragging_scrubber = false;
            s_dragging_volume = false;
            return 0;
        }

        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta > 0) {
                s_scroll_offset -= 2;
                if (s_scroll_offset < 0) s_scroll_offset = 0;
            } else {
                s_scroll_offset += 2;
                int max_scroll = (s_cached_track_count > 10) ? (s_cached_track_count - 10) : 0;
                if (s_scroll_offset > max_scroll) s_scroll_offset = max_scroll;
            }
            return 0;
        }

        case WM_CHAR: {
            if (s_search_focused) {
                if (wParam == '\b') {
                    ui_osk_input_char('\b');
                } else if (wParam == 13 || wParam == 27) {
                    s_search_focused = false;
                } else if (wParam >= 32) {
                    wchar_t wch = (wchar_t)wParam;
                    char u8[8] = {0};
                    int u8_len = WideCharToMultiByte(CP_UTF8, 0, &wch, 1, u8, sizeof(u8) - 1, NULL, NULL);
                    for (int k = 0; k < u8_len; k++) {
                        ui_osk_input_char(u8[k]);
                    }
                }
                return 0;
            } else {
                if (wParam == '/') {
                    s_search_focused = true;
                    return 0;
                }
            }
            break;
        }

        case WM_KEYDOWN: {
            if (s_search_focused) {
                if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
                    s_search_focused = false;
                    return 0;
                }
                if (wParam == VK_DOWN) {
                    s_search_focused = false;
                    s_queued_action = INPUT_DOWN;
                    return 0;
                }
                return 0;
            }

            switch (wParam) {
                case VK_UP:     s_queued_action = INPUT_UP; break;
                case VK_DOWN:   s_queued_action = INPUT_DOWN; break;
                case VK_LEFT:   s_queued_action = INPUT_PREV_TRACK; break;
                case VK_RIGHT:  s_queued_action = INPUT_NEXT_TRACK; break;
                case VK_RETURN: s_queued_action = INPUT_SELECT; break;
                case VK_SPACE:  s_queued_action = INPUT_PLAY_PAUSE; break;
                case 'S':
                case VK_OEM_2: /* '/' */
                    s_search_focused = true;
                    break;
                case 'N':       s_queued_action = INPUT_NEXT_TRACK; break;
                case 'P':       s_queued_action = INPUT_PREV_TRACK; break;
                case 'H':       s_queued_action = INPUT_SHUFFLE; break;
                case 'R':       s_queued_action = INPUT_REPEAT; break;
                case 'M':       platform_audio_set_volume((s_volume > 0.01f) ? 0.0f : 1.0f); break;
                case VK_ESCAPE:
                case 'Q':       s_queued_action = INPUT_QUIT; break;
                default: break;
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
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
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
        WinHttpSetTimeouts(s_http_session, 15000, 15000, 15000, 60000);
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

/* ── File I/O & Persistent Cache Abstraction (Win32) ─────────────── */
int platform_ensure_cache_dirs(void) {
    CreateDirectoryA("cache", NULL);
    CreateDirectoryA("cache\\tracks", NULL);
    CreateDirectoryA("cache\\covers", NULL);
    return 0;
}

bool platform_file_exists(const char *path) {
    if (!path || !path[0]) return false;
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

PlatformFileHandle platform_file_open_read(const char *path) {
    if (!path || !path[0]) return NULL;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    return (PlatformFileHandle)h;
}

PlatformFileHandle platform_file_open_write(const char *path) {
    if (!path || !path[0]) return NULL;
    platform_ensure_cache_dirs();
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    return (PlatformFileHandle)h;
}

size_t platform_file_read(PlatformFileHandle handle, void *buffer, size_t bytes) {
    if (!handle || !buffer || bytes == 0) return 0;
    DWORD bytes_read = 0;
    if (!ReadFile((HANDLE)handle, buffer, (DWORD)bytes, &bytes_read, NULL)) {
        return 0;
    }
    return (size_t)bytes_read;
}

size_t platform_file_write(PlatformFileHandle handle, const void *buffer, size_t bytes) {
    if (!handle || !buffer || bytes == 0) return 0;
    DWORD bytes_written = 0;
    if (!WriteFile((HANDLE)handle, buffer, (DWORD)bytes, &bytes_written, NULL)) {
        return 0;
    }
    return (size_t)bytes_written;
}

void platform_file_close(PlatformFileHandle handle) {
    if (handle) {
        CloseHandle((HANDLE)handle);
    }
}

size_t platform_file_size(const char *path) {
    if (!path || !path[0]) return 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    sz.QuadPart = 0;
    GetFileSizeEx(h, &sz);
    CloseHandle(h);
    return (size_t)sz.QuadPart;
}

void platform_get_cache_track_path(const char *track_id, char *out_path, size_t max_len) {
    if (!track_id || !out_path || max_len == 0) return;
    snprintf(out_path, max_len, "cache\\tracks\\%s.flac", track_id);
}

void platform_get_cache_cover_path(const char *track_id, char *out_path, size_t max_len) {
    if (!track_id || !out_path || max_len == 0) return;
    snprintf(out_path, max_len, "cache\\covers\\%s.bin", track_id);
}

/* ── Thread Implementation ───────────────────────────────────────── */
typedef struct {
    PlatformThreadFunc func;
    void *arg;
} PcThreadWrapper;

static DWORD WINAPI PcThreadEntry(LPVOID lpParam) {
    PcThreadWrapper *wrap = (PcThreadWrapper *)lpParam;
    if (wrap) {
        PlatformThreadFunc f = wrap->func;
        void *a = wrap->arg;
        free(wrap);
        if (f) f(a);
    }
    return 0;
}

PlatformThreadHandle platform_thread_create(PlatformThreadFunc func, void *arg) {
    PcThreadWrapper *wrap = (PcThreadWrapper *)malloc(sizeof(PcThreadWrapper));
    if (!wrap) return NULL;
    wrap->func = func;
    wrap->arg = arg;

    HANDLE h = CreateThread(NULL, 0, PcThreadEntry, wrap, 0, NULL);
    if (!h) {
        free(wrap);
        return NULL;
    }
    return (PlatformThreadHandle)h;
}

void platform_thread_join(PlatformThreadHandle thread) {
    if (!thread) return;
    HANDLE h = (HANDLE)thread;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}

/* ── WinHTTP GET & Streaming Implementation ───────────────────────── */
int platform_http_get_streaming(const char *url, PlatformHttpChunkCallback on_chunk, void *userdata) {
    if (!url || !on_chunk) return -1;

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

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        if (statusCode != 200 && statusCode != 206) {
            printf("[HTTP] GET %s returned HTTP %lu\n", url, statusCode);
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            return -7;
        }
    }

    /* Read stream chunks and forward to callback */
    BYTE chunk[64 * 1024];
    DWORD dwRead = 0;
    do {
        DWORD dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
            break;
        }
        if (dwSize == 0) break;

        DWORD to_read = (dwSize > sizeof(chunk)) ? sizeof(chunk) : dwSize;
        if (!WinHttpReadData(hRequest, chunk, to_read, &dwRead)) {
            break;
        }

        if (dwRead > 0) {
            if (!on_chunk(chunk, dwRead, userdata)) {
                /* Consumer aborted stream */
                break;
            }
        }
    } while (dwRead > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    return 0;
}

typedef struct {
    char   *buf;
    size_t  size;
    size_t  capacity;
} HttpAccumulator;

static bool http_accum_cb(const uint8_t *chunk, size_t chunk_len, void *userdata) {
    HttpAccumulator *acc = (HttpAccumulator *)userdata;
    if (acc->size + chunk_len + 1 > acc->capacity) {
        acc->capacity = (acc->size + chunk_len + 1) * 2;
        char *new_buf = (char *)realloc(acc->buf, acc->capacity);
        if (!new_buf) return false;
        acc->buf = new_buf;
    }
    memcpy(acc->buf + acc->size, chunk, chunk_len);
    acc->size += chunk_len;
    acc->buf[acc->size] = '\0';
    return true;
}

int platform_http_get(const char *url, char **response_out, size_t *size_out) {
    if (!url || !response_out || !size_out) return -1;
    *response_out = NULL;
    *size_out = 0;

    HttpAccumulator acc;
    acc.capacity = 64 * 1024;
    acc.size = 0;
    acc.buf = (char *)malloc(acc.capacity);
    if (!acc.buf) return -1;

    int ret = platform_http_get_streaming(url, http_accum_cb, &acc);
    if (ret != 0 || !acc.buf) {
        if (acc.buf) free(acc.buf);
        return ret;
    }

    *response_out = acc.buf;
    *size_out = acc.size;
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

    /* Wait if buffer is still playing (bounded spin with Sleep(1) only if needed) */
    if (hdr->dwFlags & WHDR_PREPARED) {
        int spin = 0;
        while (!(hdr->dwFlags & WHDR_DONE) && spin < 10) {
            Sleep(1);
            spin++;
        }
        if (hdr->dwFlags & WHDR_DONE) {
            waveOutUnprepareHeader(s_wave_out, hdr, sizeof(WAVEHDR));
        } else {
            /* Still busy; skip writing this tick to prevent UI stutter */
            return;
        }
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
    if (width < 1280 || height < 720) {
        width = 1280;
        height = 720;
    }
    s_win_width  = width;
    s_win_height = height;

    HINSTANCE hInstance = GetModuleHandle(NULL);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = PlatformWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"MonochromeWinClass";

    RegisterClassExW(&wc);

    RECT rc = { 0, 0, width, height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    int real_w = rc.right - rc.left;
    int real_h = rc.bottom - rc.top;

    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int pos_x = (screen_w - real_w) / 2;
    int pos_y = (screen_h - real_h) / 2;

    wchar_t wTitle[128] = {0};
    MultiByteToWideChar(CP_UTF8, 0, title ? title : "Monochrome Music Player", -1, wTitle, 128);

    s_hwnd = CreateWindowExW(
        0,
        L"MonochromeWinClass",
        wTitle,
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

    /* Create Typography Fonts (Unicode / ClearType) */
    s_font_title   = CreateFontW(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_font_sub     = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_font_heading = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_font_track   = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_font_meta    = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_font_mono    = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

    /* Create UI Brushes */
    s_br_bg        = CreateSolidBrush(RGB(10, 10, 12));
    s_br_sidebar   = CreateSolidBrush(RGB(17, 18, 22));
    s_br_row_even  = CreateSolidBrush(RGB(20, 22, 28));
    s_br_row_odd   = CreateSolidBrush(RGB(24, 26, 34));
    s_br_row_hover = CreateSolidBrush(RGB(34, 37, 48));
    s_br_highlight = CreateSolidBrush(RGB(42, 46, 61));
    s_br_accent    = CreateSolidBrush(RGB(255, 255, 255));
    s_br_dock      = CreateSolidBrush(RGB(17, 18, 22));
    s_br_bar_bg    = CreateSolidBrush(RGB(37, 40, 52));
    s_br_bar_fill  = CreateSolidBrush(RGB(255, 255, 255));
    s_br_card      = CreateSolidBrush(RGB(24, 26, 34));

    /* Create UI Pens */
    s_pen_border    = CreatePen(PS_SOLID, 1, RGB(37, 40, 52));
    s_pen_highlight = CreatePen(PS_SOLID, 1, RGB(100, 200, 255));
    s_pen_accent    = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    s_pen_dim       = CreatePen(PS_SOLID, 1, RGB(32, 34, 42));

    /* Load Cursors */
    s_cur_arrow     = LoadCursor(NULL, IDC_ARROW);
    s_cur_hand      = LoadCursor(NULL, IDC_HAND);
    s_cur_ibeam     = LoadCursor(NULL, IDC_IBEAM);

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
    if (s_br_sidebar)   DeleteObject(s_br_sidebar);
    if (s_br_row_even)  DeleteObject(s_br_row_even);
    if (s_br_row_odd)   DeleteObject(s_br_row_odd);
    if (s_br_row_hover) DeleteObject(s_br_row_hover);
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
    s_current_state = state;
    s_cached_track_count = track_count;
    s_total_duration_cached = (track) ? (float)track->duration_sec : 0.0f;

    if (!s_mem_dc || !s_window_open) return;

    /* Clamp scroll offset to keep selected cursor visible */
    if (cursor < s_scroll_offset) {
        s_scroll_offset = cursor;
    } else if (cursor >= s_scroll_offset + 10) {
        s_scroll_offset = cursor - 9;
    }
    int max_scroll = (track_count > 10) ? (track_count - 10) : 0;
    if (s_scroll_offset > max_scroll) s_scroll_offset = max_scroll;
    if (s_scroll_offset < 0) s_scroll_offset = 0;

    SetBkMode(s_mem_dc, TRANSPARENT);

    /* 1. Clear background canvas (#0A0A0C) */
    RECT r_all = { 0, 0, s_win_width, s_win_height };
    FillRect(s_mem_dc, &r_all, s_br_bg);

    /* 2. Left Sidebar (x: 0 -> 230, y: 0 -> 625) */
    RECT r_sb = { 0, 0, 230, 625 };
    FillRect(s_mem_dc, &r_sb, s_br_sidebar);

    SelectObject(s_mem_dc, s_pen_border);
    MoveToEx(s_mem_dc, 230, 0, NULL);
    LineTo(s_mem_dc, 230, 625);

    /* Logo & App Name */
    SelectObject(s_mem_dc, s_font_title);
    SetTextColor(s_mem_dc, RGB(245, 245, 247));
    TextOutW(s_mem_dc, 24, 20, L"◈ MONOCHROME", 12);

    SelectObject(s_mem_dc, s_font_sub);
    SetTextColor(s_mem_dc, RGB(110, 115, 133));
    TextOutW(s_mem_dc, 26, 48, L"LOSSLESS AUDIO ENGINE", 21);

    MoveToEx(s_mem_dc, 20, 75, NULL);
    LineTo(s_mem_dc, 210, 75);

    /* Navigation Menu */
    SelectObject(s_mem_dc, s_font_meta);
    SetTextColor(s_mem_dc, RGB(110, 115, 133));
    TextOutW(s_mem_dc, 25, 90, L"EXPLORAR", 8);

    static const wchar_t *s_nav_items[] = {
        L"◈  Inicio",
        L"🔍  Búsqueda",
        L"☰  Cola de Reproducción",
        L"♡  Biblioteca"
    };
    for (int i = 0; i < 4; i++) {
        int ny = 115 + i * 36;
        RECT r_item = { 15, ny, 215, ny + 32 };
        bool is_active = (i == 0 && !s_search_focused) || (i == 1 && s_search_focused);
        if (is_active) {
            FillRect(s_mem_dc, &r_item, s_br_highlight);
            FrameRect(s_mem_dc, &r_item, s_br_bar_bg);
        }
        SelectObject(s_mem_dc, s_font_sub);
        SetTextColor(s_mem_dc, is_active ? RGB(255, 255, 255) : RGB(160, 165, 181));
        RECT r_txt = { 25, ny, 210, ny + 32 };
        DrawTextW(s_mem_dc, s_nav_items[i], -1, &r_txt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    /* Quick Preset Genres */
    SelectObject(s_mem_dc, s_font_meta);
    SetTextColor(s_mem_dc, RGB(110, 115, 133));
    TextOutW(s_mem_dc, 25, 265, L"GÉNEROS RÁPIDOS", 15);

    static const wchar_t *s_tags[] = {
        L"• Trending", L"• Daft Punk", L"• The Weeknd",
        L"• Synthwave", L"• Lossless", L"• Electronic"
    };
    for (int i = 0; i < 6; i++) {
        int ty = 290 + i * 28;
        RECT r_tag = { 20, ty, 215, ty + 26 };
        SelectObject(s_mem_dc, s_font_sub);
        SetTextColor(s_mem_dc, RGB(160, 165, 181));
        DrawTextW(s_mem_dc, s_tags[i], -1, &r_tag, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    /* Diagnostics Card */
    RECT r_diag = { 15, 520, 215, 608 };
    FillRect(s_mem_dc, &r_diag, s_br_card);
    FrameRect(s_mem_dc, &r_diag, s_br_bar_bg);

    SelectObject(s_mem_dc, s_font_mono);
    SetTextColor(s_mem_dc, RGB(16, 185, 129));
    TextOutW(s_mem_dc, 25, 532, L"● TIDAL CDN ONLINE", 18);
    SetTextColor(s_mem_dc, RGB(160, 165, 181));
    TextOutW(s_mem_dc, 25, 555, L"DAC: 48k LPCM Stereo", 20);
    TextOutW(s_mem_dc, 25, 578, L"BUF: 8.0 MB XDR Ring", 20);

    /* 3. Top Header Bar (x: 230 -> 1280, y: 0 -> 75) */
    RECT r_head = { 230, 0, s_win_width, 75 };
    FillRect(s_mem_dc, &r_head, s_br_sidebar);

    SelectObject(s_mem_dc, s_pen_border);
    MoveToEx(s_mem_dc, 230, 75, NULL);
    LineTo(s_mem_dc, s_win_width, 75);

    /* Navigation history arrows */
    RECT r_prev_btn = { 245, 20, 275, 54 };
    RECT r_next_btn = { 285, 20, 315, 54 };
    FillRect(s_mem_dc, &r_prev_btn, s_br_card);
    FrameRect(s_mem_dc, &r_prev_btn, s_br_bar_bg);
    FillRect(s_mem_dc, &r_next_btn, s_br_card);
    FrameRect(s_mem_dc, &r_next_btn, s_br_bar_bg);

    SelectObject(s_mem_dc, s_font_heading);
    SetTextColor(s_mem_dc, RGB(160, 165, 181));
    DrawTextW(s_mem_dc, L"<", -1, &r_prev_btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(s_mem_dc, L">", -1, &r_next_btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* Search Input Box (x: 335 -> 780, y: 18 -> 56) */
    RECT r_search = { 335, 18, 780, 56 };
    FillRect(s_mem_dc, &r_search, s_br_card);
    FrameRect(s_mem_dc, &r_search, s_search_focused ? s_br_accent : s_br_bar_bg);

    const char *current_q = ui_get_search_query();
    if (current_q && current_q[0] != '\0') {
        wchar_t wq[128];
        MultiByteToWideChar(CP_UTF8, 0, current_q, -1, wq, 128);
        wchar_t display_q[160];
        if (s_search_focused && ((GetTickCount64() / 500) % 2 == 0)) {
            _snwprintf(display_q, 160, L"🔍 %ls|", wq);
        } else {
            _snwprintf(display_q, 160, L"🔍 %ls", wq);
        }
        SelectObject(s_mem_dc, s_font_track);
        SetTextColor(s_mem_dc, RGB(245, 245, 247));
        RECT r_qtext = { 348, 18, 745, 56 };
        DrawTextW(s_mem_dc, display_q, -1, &r_qtext, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        /* Clear button [×] */
        SelectObject(s_mem_dc, s_font_heading);
        SetTextColor(s_mem_dc, (s_hover_btn == 7) ? RGB(255, 255, 255) : RGB(160, 165, 181));
        RECT r_clear = { 752, 18, 778, 56 };
        DrawTextW(s_mem_dc, L"×", -1, &r_clear, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        SelectObject(s_mem_dc, s_font_sub);
        SetTextColor(s_mem_dc, RGB(110, 115, 133));
        RECT r_qtext = { 348, 18, 745, 56 };
        const wchar_t *ph = s_search_focused ? L"🔍 |" : L"🔍 Buscar canciones, artistas o álbumes... (Presiona '/' para escribir)";
        DrawTextW(s_mem_dc, ph, -1, &r_qtext, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    /* Badges on right header */
    SelectObject(s_mem_dc, s_font_mono);
    RECT r_badge1 = { 870, 22, 1045, 52 };
    FillRect(s_mem_dc, &r_badge1, s_br_card);
    FrameRect(s_mem_dc, &r_badge1, s_br_bar_bg);
    SetTextColor(s_mem_dc, RGB(255, 209, 102));
    DrawTextW(s_mem_dc, L"24-BIT / 96k · HI-RES", -1, &r_badge1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT r_badge2 = { 1060, 22, 1245, 52 };
    FillRect(s_mem_dc, &r_badge2, s_br_card);
    FrameRect(s_mem_dc, &r_badge2, s_br_bar_bg);
    SetTextColor(s_mem_dc, RGB(16, 185, 129));
    DrawTextW(s_mem_dc, L"● TIDAL LOSSLESS", -1, &r_badge2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* 4. Catalog Area (x: 230 -> 1280, y: 75 -> 625) */
    char cat_str[128];
    snprintf(cat_str, sizeof(cat_str), "CATÁLOGO: %s (%d canciones disponibles)",
             (current_q && current_q[0] != '\0') ? current_q : "Tendencias", track_count);
    SelectObject(s_mem_dc, s_font_heading);
    SetTextColor(s_mem_dc, RGB(245, 245, 247));
    gdi_text_out_utf8(s_mem_dc, 250, 94, cat_str);

    /* Column Headers at y: 125 */
    SelectObject(s_mem_dc, s_font_mono);
    SetTextColor(s_mem_dc, RGB(110, 115, 133));
    TextOutW(s_mem_dc, 255,  126, L"#", 1);
    TextOutW(s_mem_dc, 310,  126, L"TÍTULO", 6);
    TextOutW(s_mem_dc, 680,  126, L"ARTISTA", 7);
    TextOutW(s_mem_dc, 910,  126, L"ÁLBUM", 5);
    TextOutW(s_mem_dc, 1080, 126, L"CALIDAD", 7);
    TextOutW(s_mem_dc, 1200, 126, L"TIEMPO", 6);

    SelectObject(s_mem_dc, s_pen_dim);
    MoveToEx(s_mem_dc, 245, 144, NULL);
    LineTo(s_mem_dc, 1250, 144);

    /* Track List Rows (10 rows max visible) */
    int rows_to_draw = (track_count - s_scroll_offset > 10) ? 10 : (track_count - s_scroll_offset);
    if (rows_to_draw < 0) rows_to_draw = 0;

    for (int i = 0; i < rows_to_draw; i++) {
        int t_idx = s_scroll_offset + i;
        int row_y = 150 + i * 44;
        RECT r_row = { 245, row_y, 1250, row_y + 40 };

        bool is_sel = (t_idx == cursor);
        bool is_hover = (t_idx == s_hover_track);

        if (is_sel) {
            FillRect(s_mem_dc, &r_row, s_br_highlight);
            FrameRect(s_mem_dc, &r_row, s_br_accent);

            /* Vertical accent indicator */
            RECT r_strip = { 245, row_y, 249, row_y + 40 };
            FillRect(s_mem_dc, &r_strip, s_br_accent);
        } else if (is_hover) {
            FillRect(s_mem_dc, &r_row, s_br_row_hover);
        } else {
            FillRect(s_mem_dc, &r_row, (t_idx % 2 == 0) ? s_br_row_even : s_br_row_odd);
        }

        /* Number / Play indicator */
        SelectObject(s_mem_dc, s_font_mono);
        SetTextColor(s_mem_dc, is_sel ? RGB(255, 255, 255) : RGB(160, 165, 181));
        wchar_t num_buf[16];
        if (is_sel) _snwprintf(num_buf, 16, L"▶%2d", t_idx + 1);
        else        _snwprintf(num_buf, 16, L" %2d", t_idx + 1);
        TextOutW(s_mem_dc, 255, row_y + 11, num_buf, (int)wcslen(num_buf));

        /* Title */
        SelectObject(s_mem_dc, s_font_track);
        SetTextColor(s_mem_dc, is_sel ? RGB(255, 255, 255) : RGB(245, 245, 247));
        RECT r_title = { 310, row_y, 670, row_y + 40 };
        gdi_draw_text_utf8(s_mem_dc, tracks[t_idx].title, &r_title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        /* Artist */
        SelectObject(s_mem_dc, s_font_meta);
        SetTextColor(s_mem_dc, is_sel ? RGB(220, 225, 240) : RGB(160, 165, 181));
        RECT r_artist = { 680, row_y, 900, row_y + 40 };
        gdi_draw_text_utf8(s_mem_dc, tracks[t_idx].artist, &r_artist, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        /* Album */
        SetTextColor(s_mem_dc, is_sel ? RGB(180, 185, 200) : RGB(110, 115, 133));
        RECT r_album = { 910, row_y, 1070, row_y + 40 };
        gdi_draw_text_utf8(s_mem_dc, tracks[t_idx].album, &r_album, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        /* Quality Badge */
        SelectObject(s_mem_dc, s_font_mono);
        RECT r_qbadge = { 1080, row_y + 9, 1170, row_y + 31 };
        FillRect(s_mem_dc, &r_qbadge, s_br_card);
        FrameRect(s_mem_dc, &r_qbadge, s_br_bar_bg);
        SetTextColor(s_mem_dc, tracks[t_idx].is_hi_res ? RGB(255, 209, 102) : RGB(100, 200, 255));
        DrawTextW(s_mem_dc, tracks[t_idx].is_hi_res ? L"HI-RES" : L"FLAC", -1, &r_qbadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        /* Duration */
        char time_buf[16];
        uint32_t dur = tracks[t_idx].duration_sec;
        snprintf(time_buf, sizeof(time_buf), "%02u:%02u", dur / 60, dur % 60);
        SetTextColor(s_mem_dc, is_sel ? RGB(255, 255, 255) : RGB(160, 165, 181));
        gdi_text_out_utf8(s_mem_dc, 1200, row_y + 11, time_buf);
    }

    /* Scroll Status */
    if (track_count > 10) {
        char scroll_info[128];
        snprintf(scroll_info, sizeof(scroll_info),
                 "▲▼ Mostrando canciones %d-%d de %d  ·  Usa la rueda del ratón o flechas para desplazarte",
                 s_scroll_offset + 1,
                 (s_scroll_offset + 10 > track_count) ? track_count : (s_scroll_offset + 10),
                 track_count);
        SelectObject(s_mem_dc, s_font_sub);
        SetTextColor(s_mem_dc, RGB(110, 115, 133));
        gdi_text_out_utf8(s_mem_dc, 250, 595, scroll_info);
    }

    /* 5. Now Playing Elevated Dock (x: 0 -> 1280, y: 625 -> 720) */
    RECT r_dock = { 0, 625, s_win_width, s_win_height };
    FillRect(s_mem_dc, &r_dock, s_br_dock);

    SelectObject(s_mem_dc, s_pen_border);
    MoveToEx(s_mem_dc, 0, 625, NULL);
    LineTo(s_mem_dc, s_win_width, 625);

    /* Cover Art */
    RECT r_art = { 20, 633, 94, 707 };
    const CoverArtImage *cover = cover_art_get_current();
    if (cover && cover->ready) {
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = (LONG)cover->width;
        bmi.bmiHeader.biHeight = -(LONG)cover->height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        StretchDIBits(
            s_mem_dc,
            20, 633, 74, 74,
            0, 0, cover->width, cover->height,
            cover->pixels,
            &bmi,
            DIB_RGB_COLORS,
            SRCCOPY
        );
        FrameRect(s_mem_dc, &r_art, s_br_bar_bg);
    } else {
        FillRect(s_mem_dc, &r_art, s_br_card);
        FrameRect(s_mem_dc, &r_art, s_br_bar_bg);
        SelectObject(s_mem_dc, s_font_heading);
        SetTextColor(s_mem_dc, RGB(255, 255, 255));
        DrawTextW(s_mem_dc, L"♫", -1, &r_art, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    /* Now Playing Info */
    const char *now_t = (track && track->title[0] != '\0') ? track->title : "Sin pista seleccionada";
    const char *now_a = (track && track->artist[0] != '\0') ? track->artist : "Selecciona una pista para reproducir en streaming lossless";

    SelectObject(s_mem_dc, s_font_track);
    SetTextColor(s_mem_dc, RGB(245, 245, 247));
    RECT r_now_t = { 108, 637, 400, 661 };
    gdi_draw_text_utf8(s_mem_dc, now_t, &r_now_t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(s_mem_dc, s_font_meta);
    SetTextColor(s_mem_dc, RGB(160, 165, 181));
    RECT r_now_a = { 108, 663, 400, 685 };
    gdi_draw_text_utf8(s_mem_dc, now_a, &r_now_a, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(s_mem_dc, s_font_sub);
    SetTextColor(s_mem_dc, RGB(110, 115, 133));
    RECT r_now_q = { 108, 687, 400, 707 };
    DrawTextW(s_mem_dc, (track && track->is_hi_res) ? L"Hi-Res FLAC 24-Bit / 96 kHz" : L"Lossless FLAC 16-Bit / 44.1 kHz", -1, &r_now_q, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* Center Transport Controls (y: 634) */
    bool is_shuf = ui_get_shuffle_mode();
    SelectObject(s_mem_dc, s_font_sub);
    SetTextColor(s_mem_dc, is_shuf ? RGB(16, 185, 129) : ((s_hover_btn == 1) ? RGB(255, 255, 255) : RGB(160, 165, 181)));
    RECT r_shuf = { 515, 634, 545, 662 };
    DrawTextW(s_mem_dc, L"🔀", -1, &r_shuf, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SetTextColor(s_mem_dc, (s_hover_btn == 2) ? RGB(255, 255, 255) : RGB(160, 165, 181));
    RECT r_prev = { 560, 634, 590, 662 };
    DrawTextW(s_mem_dc, L"⏮", -1, &r_prev, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* Circular Play/Pause button */
    RECT r_pp = { 610, 628, 650, 668 };
    SelectObject(s_mem_dc, s_pen_border);
    SelectObject(s_mem_dc, (s_hover_btn == 3) ? s_br_highlight : s_br_card);
    Ellipse(s_mem_dc, 610, 628, 650, 668);
    SelectObject(s_mem_dc, s_font_heading);
    SetTextColor(s_mem_dc, RGB(255, 255, 255));
    const wchar_t *pp_icon = (pstate == PLAYBACK_PLAYING) ? L"⏸" : L"▶";
    DrawTextW(s_mem_dc, pp_icon, -1, &r_pp, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(s_mem_dc, s_font_sub);
    SetTextColor(s_mem_dc, (s_hover_btn == 4) ? RGB(255, 255, 255) : RGB(160, 165, 181));
    RECT r_next = { 660, 634, 690, 662 };
    DrawTextW(s_mem_dc, L"⏭", -1, &r_next, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int rep_mode = ui_get_repeat_mode();
    SetTextColor(s_mem_dc, (rep_mode != 0) ? RGB(100, 200, 255) : ((s_hover_btn == 5) ? RGB(255, 255, 255) : RGB(160, 165, 181)));
    RECT r_rep = { 705, 634, 735, 662 };
    DrawTextW(s_mem_dc, (rep_mode == 2) ? L"🔂" : L"🔁", -1, &r_rep, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* Playback Status above progress bar */
    SelectObject(s_mem_dc, s_font_mono);
    if (pstate == PLAYBACK_PLAYING) {
        SetTextColor(s_mem_dc, RGB(16, 185, 129));
        TextOutW(s_mem_dc, 420, 663, L"▶ EN REPRODUCCIÓN (48,000 Hz LPCM DAC)", 38);
    } else if (pstate == PLAYBACK_BUFFERING) {
        SetTextColor(s_mem_dc, RGB(100, 200, 255));
        TextOutW(s_mem_dc, 420, 663, L"◌ BUFFERING & STREAMING FLAC...", 31);
    } else if (pstate == PLAYBACK_PAUSED) {
        SetTextColor(s_mem_dc, RGB(245, 158, 11));
        TextOutW(s_mem_dc, 420, 663, L"⏸ EN PAUSA", 10);
    } else {
        SetTextColor(s_mem_dc, RGB(110, 115, 133));
        TextOutW(s_mem_dc, 420, 663, L"⏹ LISTO", 7);
    }

    /* Scrubber & Progress Bar */
    uint32_t total_sec = (track) ? track->duration_sec : 0;
    uint32_t cur_sec = (uint32_t)(progress * (float)total_sec);
    char cur_time_str[16];
    snprintf(cur_time_str, sizeof(cur_time_str), "%02u:%02u", cur_sec / 60, cur_sec % 60);
    SetTextColor(s_mem_dc, RGB(160, 165, 181));
    gdi_text_out_utf8(s_mem_dc, 365, 678, cur_time_str);

    RECT r_prog_bg = { 420, 683, 840, 688 };
    FillRect(s_mem_dc, &r_prog_bg, s_br_bar_bg);

    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    int fill_w = (int)(progress * 420.0f);
    RECT r_prog_fill = { 420, 683, 420 + fill_w, 688 };
    FillRect(s_mem_dc, &r_prog_fill, s_br_bar_fill);

    /* Scrubber Knob */
    int knob_x = 420 + fill_w;
    SelectObject(s_mem_dc, s_pen_border);
    SelectObject(s_mem_dc, s_br_accent);
    Ellipse(s_mem_dc, knob_x - 5, 685 - 5, knob_x + 5, 685 + 5);

    char total_time_str[16];
    snprintf(total_time_str, sizeof(total_time_str), "%02u:%02u", total_sec / 60, total_sec % 60);
    gdi_text_out_utf8(s_mem_dc, 850, 678, total_time_str);

    /* 16-Band Real-Time Audio Visualizer (x: 930 -> 1060, y: 642 -> 692) */
    float vis_bands[VIS_BANDS];
    audio_get_visualization(vis_bands, VIS_BANDS);
    for (int b = 0; b < VIS_BANDS; b++) {
        int bar_x = 930 + b * 8;
        int bar_h = (int)(vis_bands[b] * 45.0f);
        if (bar_h < 2) bar_h = 2;
        if (bar_h > 45) bar_h = 45;
        int bar_y = 690 - bar_h;
        RECT r_bar = { bar_x, bar_y, bar_x + 6, 690 };
        FillRect(s_mem_dc, &r_bar, s_br_accent);

        RECT r_dot = { bar_x, bar_y - 2, bar_x + 6, bar_y };
        FillRect(s_mem_dc, &r_dot, s_br_highlight);
    }

    /* Volume Slider (x: 1075 -> 1250, y: 660 -> 686) */
    SelectObject(s_mem_dc, s_font_sub);
    SetTextColor(s_mem_dc, RGB(160, 165, 181));
    RECT r_spk = { 1075, 660, 1100, 686 };
    DrawTextW(s_mem_dc, (s_volume > 0.01f) ? L"🔊" : L"🔇", -1, &r_spk, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT r_vol_bg = { 1105, 671, 1195, 675 };
    FillRect(s_mem_dc, &r_vol_bg, s_br_bar_bg);
    int vol_fill = (int)(s_volume * 90.0f);
    RECT r_vol_fill = { 1105, 671, 1105 + vol_fill, 675 };
    FillRect(s_mem_dc, &r_vol_fill, s_br_bar_fill);

    int vol_knob_x = 1105 + vol_fill;
    SelectObject(s_mem_dc, s_br_accent);
    Ellipse(s_mem_dc, vol_knob_x - 4, 673 - 4, vol_knob_x + 4, 673 + 4);

    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%d%%", (int)(s_volume * 100.0f));
    SelectObject(s_mem_dc, s_font_mono);
    SetTextColor(s_mem_dc, RGB(160, 165, 181));
    gdi_text_out_utf8(s_mem_dc, 1205, 664, vol_str);

    /* 6. Blit from memory DC to Window DC */
    HDC hdc = GetDC(s_hwnd);
    BitBlt(hdc, 0, 0, s_win_width, s_win_height, s_mem_dc, 0, 0, SRCCOPY);
    ReleaseDC(s_hwnd, hdc);
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

    /* 3. Check piped / redirected stdin for automated test harnesses */
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != NULL && hIn != INVALID_HANDLE_VALUE) {
        DWORD ftype = GetFileType(hIn);
        bool has_data = false;
        if (ftype == FILE_TYPE_PIPE) {
            DWORD bytesAvail = 0;
            if (PeekNamedPipe(hIn, NULL, 0, NULL, &bytesAvail, NULL) && bytesAvail > 0) {
                has_data = true;
            }
        } else if (ftype == FILE_TYPE_DISK) {
            has_data = true;
        }

        if (has_data) {
            char ch = 0;
            DWORD bytesRead = 0;
            if (ReadFile(hIn, &ch, 1, &bytesRead, NULL) && bytesRead > 0) {
                if (s_current_state == (int)STATE_SEARCH || s_search_focused) {
                    if (ch == '\r' || ch == '\n') {
                        ui_osk_set_cursor(3, 9); /* Move to GO key */
                        s_search_focused = false;
                        return INPUT_SELECT;
                    }
                    if (ch == 27) { s_search_focused = false; return INPUT_CANCEL; }
                    if (ch == 8) { ui_osk_input_char('\b'); return INPUT_NONE; }
                    if (ch >= 32 && ch < 127) {
                        ui_osk_input_char(ch);
                        return INPUT_NONE;
                    }
                } else {
                    switch (ch) {
                        case '\r':
                        case '\n': return INPUT_SELECT;
                        case ' ':  return INPUT_PLAY_PAUSE;
                        case 's': case 'S': case '/': return INPUT_ACTION_SEARCH;
                        case 'n': case 'N': return INPUT_NEXT_TRACK;
                        case 'p': case 'P': return INPUT_PREV_TRACK;
                        case 'h': case 'H': return INPUT_SHUFFLE;
                        case 'r': case 'R': return INPUT_REPEAT;
                        case 'q': case 'Q': case 27:  return INPUT_QUIT;
                        case 'w': case 'W': return INPUT_UP;
                        case 'z': case 'Z': return INPUT_DOWN;
                        default: break;
                    }
                }
            }
        }
    }

    /* 4. Fallback to console keyboard polling if console is attached */
    if (GetConsoleWindow() != NULL) {
        if (_kbhit()) {
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

            if (s_current_state == (int)STATE_SEARCH || s_search_focused) {
                switch (ch) {
                    case 13:
                        ui_osk_set_cursor(3, 9); /* Move to GO key */
                        s_search_focused = false;
                        return INPUT_SELECT;
                    case 27: s_search_focused = false; return INPUT_CANCEL;        /* ESC */
                    case 8:  ui_osk_input_char('\b'); return INPUT_NONE;
                    default:
                        if (ch >= 32 && ch < 127) {
                            ui_osk_input_char((char)ch);
                            return INPUT_NONE;
                        }
                        break;
                }
            } else {
                switch (ch) {
                    case 13: return INPUT_SELECT;        /* Enter */
                    case 27: return INPUT_QUIT;          /* ESC */
                    case ' ': return INPUT_PLAY_PAUSE;   /* Space */
                    case 's': case 'S': case '/': return INPUT_ACTION_SEARCH;
                    case 'n': case 'N': return INPUT_NEXT_TRACK;
                    case 'p': case 'P': return INPUT_PREV_TRACK;
                    case 'h': case 'H': return INPUT_SHUFFLE;
                    case 'r': case 'R': return INPUT_REPEAT;
                    case 'q': case 'Q': return INPUT_QUIT;
                    case 'w': case 'W': return INPUT_UP;
                    case 'z': case 'Z': return INPUT_DOWN;
                    default: return INPUT_NONE;
                }
            }
        }
    }

    return INPUT_NONE;
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
