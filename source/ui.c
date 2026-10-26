/*
 * Monochrome PS3 - UI & Application Logic (PSL1GHT)
 * Pure dynamic data from Monochrome REST API (tracks.monochrome.st)
 * Strictly zero hardcoded mock metadata (Rule: metadatos SOLO de la API)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "monochrome.h"
#include "platform.h"
#include "ui.h"
#include "http_client.h"
#include "audio_engine.h"
#include "cover_art.h"

/* Real search results directly from tracks.monochrome.st */
static TrackMetadata s_display_tracks[MAX_SEARCH_RESULTS];
static int s_display_count = 0;
static int s_cursor = 0;

/* Playback state */
static int s_current_track_idx = -1;
static PlaybackState s_playback_state = PLAYBACK_STOPPED;
static float s_playback_progress = 0.0f;
static RepeatMode s_repeat_mode = REPEAT_ALL;
static bool s_shuffle = false;

/* Search state */
static char s_search_query[64] = "daft";
static int  s_search_len = 4;
static int  s_osk_row = 1;
static int  s_osk_col = 0;

static SearchStatus s_search_status = SEARCH_STATUS_IDLE;
static int s_last_http_code = 0;
static int s_last_errno = 0;

/* Screen adjustment mode */
static bool s_screen_adjust_mode = false;
static int s_adjust_offset_x = 0;
static int s_adjust_offset_y = 0;
static float s_adjust_scale = 1.0f;
static float s_adjust_font_scale = 1.0f;

/* Menu state */
static bool s_menu_open = false;
static int s_menu_cursor = 0;
static bool s_exit_requested = false;

#define MENU_ITEMS 4
static const char *s_menu_items[] = {
    "Screen Adjust",
    "Network Settings",
    "System Info",
    "Exit"
};

/* Network settings */
static int s_network_timeout = 30;  /* seconds */
static int s_max_retries = 8;

/* System info display */
static bool s_show_system_info = false;
static bool s_show_network_settings = false;
static int s_network_cursor = 0;

static const char *s_osk[4][10] = {
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
    { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" },
    { "A", "S", "D", "F", "G", "H", "J", "K", "L", "-" },
    { "Z", "X", "C", "V", "B", "N", "M", "SPC", "DEL", "GO" }
};

const char *ui_get_state_name(AppState state)
{
    switch (state) {
        case STATE_HOME:    return "STATE_HOME (Biblioteca)";
        case STATE_SEARCH:  return "STATE_SEARCH (Teclado OSK)";
        case STATE_PLAYING: return "STATE_PLAYING (Reproduciendo)";
        case STATE_QUEUE:   return "STATE_QUEUE (Cola de Reproduccion)";
        default:            return "STATE_UNKNOWN";
    }
}

bool ui_should_exit(void)
{
    return s_exit_requested;
}

/* Helper: Play track and load cover art */
static void play_track_with_cover(int track_idx)
{
    if (track_idx < 0 || track_idx >= s_display_count) return;
    
    TrackMetadata *t = &s_display_tracks[track_idx];
    
    /* Start audio playback */
    audio_engine_play_track(t->id, t->title);
    
    /* Load cover art asynchronously (non-blocking) */
    if (t->cover_url[0] != '\0') {
        cover_art_load(t->cover_url);
    } else {
        cover_art_clear();
    }
}

void ui_render_splash(void)
{
    int sw = platform_get_width();
    int sh = platform_get_height();

    const char *banner[] = {
        "+-----------------------------------------------------------------------------+",
        "|  __  __  ___  _  _  ___   ____ _  _ ___  ___  __  __ ___                    |",
        "| |  \\/  |/ _ \\| \\| |/ _ \\ / ___| || | _ \\/ _ \\|  \\/  | __|                   |",
        "| | |\\/| | (_) | .` | (_) | |   | __ |   / (_) | |\\/| | _|                    |",
        "| |_|  |_|\\___/|_|\\_|\\___/ \\____|_||_|_|_\\\\___/|_|  |_|___|                   |",
        "+-----------------------------------------------------------------------------+",
        NULL
    };

    uint64_t t_start = platform_get_time_usec();
    /* Display for 2.2 seconds or until any button is pressed */
    while ((platform_get_time_usec() - t_start) < 2200000ULL) {
        platform_clear(0x00000000);

        /* Minimalist technical borders */
        platform_draw_text(60, 80, "+ [ CORE // MONOCHROME AUDIO ENGINE ] +", 0x00C83838, 2);
        platform_draw_text_r(sw - 60, 80, "LOC: 0x40100000 // CELL B.E.", 0x00666666, 1);

        int line_h = 28;
        int y_art = (sh / 2) - 100;
        for (int i = 0; banner[i] != NULL; i++) {
            platform_draw_text(60, y_art + (i * line_h), banner[i], 0xFFFFFFFF, 2);
        }

        platform_draw_text(60, y_art + 190, ":: 48kHz / 32-bit FLOAT LPCM RESAMPLER :: LOW-LATENCY STREAMING ::", 0x00C83838, 1);
        platform_draw_text(60, y_art + 215, ":: METALHEART MINIMALIST PS3 KERNEL INITIALIZED ::", 0x00888888, 1);
        platform_draw_text_r(sw - 60, sh - 60, "[ PRESS ANY BUTTON TO SKIP ]", 0x00555555, 1);

        platform_flip();

        InputAction act = platform_poll_input();
        if (act != ACT_NONE) break;
        usleep(16000);
    }
}

void ui_init(void)
{
    memset(s_display_tracks, 0, sizeof(s_display_tracks));
    s_display_count = 0;
    s_cursor = 0;
    s_current_track_idx = -1;
    s_playback_state = PLAYBACK_STOPPED;
    s_playback_progress = 0.0f;

    /* Initialize Network */
    http_net_init();

    /* Launch initial real background search for "daft" (no auto-play on startup) */
    strncpy(s_search_query, "daft", sizeof(s_search_query) - 1);
    s_search_len = 4;
    http_search_async_start(s_search_query);
    s_search_status = SEARCH_STATUS_BUSY;
}

AppState ui_handle_input(AppState current, InputAction act)
{
    if (act == ACT_NONE) return current;

    /* ── MENU STATE ── */
    if (s_menu_open) {
        switch (act) {
            case ACT_UP:
                if (s_menu_cursor > 0) s_menu_cursor--;
                break;
            case ACT_DOWN:
                if (s_menu_cursor < MENU_ITEMS - 1) s_menu_cursor++;
                break;
            case ACT_CROSS:
                /* Execute menu action */
                if (s_menu_cursor == 0) {
                    /* Screen Adjust */
                    s_menu_open = false;
                    s_screen_adjust_mode = true;
                    platform_get_screen_adjust(&s_adjust_offset_x, &s_adjust_offset_y, &s_adjust_scale);
                    s_adjust_font_scale = platform_get_font_scale();
                } else if (s_menu_cursor == 1) {
                    /* Network Settings */
                    s_menu_open = false;
                    /* TODO: Implement network settings UI */
                } else if (s_menu_cursor == 2) {
                    /* System Info */
                    s_menu_open = false;
                    s_show_system_info = true;
                } else if (s_menu_cursor == 3) {
                    /* Exit */
                    s_exit_requested = true;
                    s_menu_open = false;
                }
                break;
            case ACT_CIRCLE:
            case ACT_START:
                /* Close menu */
                s_menu_open = false;
                break;
            default:
                break;
        }
        return current;
    }

    /* ── SCREEN ADJUST MODE ── */
    if (s_screen_adjust_mode) {
        switch (act) {
            case ACT_UP:
                s_adjust_offset_y -= 1;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                break;
            case ACT_DOWN:
                s_adjust_offset_y += 1;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                break;
            case ACT_LEFT:
                s_adjust_offset_x -= 1;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                break;
            case ACT_RIGHT:
                s_adjust_offset_x += 1;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                break;
            case ACT_L2: /* Decrease scale (fine step 0.005) */
                s_adjust_scale -= 0.005f;
                if (s_adjust_scale < 0.5f) s_adjust_scale = 0.5f;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                break;
            case ACT_R2: /* Increase scale (fine step 0.005) */
                s_adjust_scale += 0.005f;
                if (s_adjust_scale > 1.5f) s_adjust_scale = 1.5f;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                break;
            case ACT_L1: /* Decrease font scale (fine step 0.02) */
                s_adjust_font_scale -= 0.02f;
                if (s_adjust_font_scale < 0.5f) s_adjust_font_scale = 0.5f;
                platform_set_font_scale(s_adjust_font_scale);
                break;
            case ACT_R1: /* Increase font scale (fine step 0.02) */
                s_adjust_font_scale += 0.02f;
                if (s_adjust_font_scale > 2.0f) s_adjust_font_scale = 2.0f;
                platform_set_font_scale(s_adjust_font_scale);
                break;
            case ACT_START: /* Save and exit */
                platform_set_font_scale(s_adjust_font_scale);
                platform_save_screen_config();
                s_screen_adjust_mode = false;
                break;
            case ACT_SELECT: /* Reset to defaults */
                s_adjust_offset_x = 0;
                s_adjust_offset_y = 0;
                s_adjust_scale = 1.0f;
                s_adjust_font_scale = 1.0f;
                platform_set_screen_adjust(s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
                platform_set_font_scale(s_adjust_font_scale);
                break;
            case ACT_CIRCLE: /* Cancel without saving */
                s_screen_adjust_mode = false;
                break;
            default:
                break;
        }
        return current;
    }

    /* ── SYSTEM INFO MODE ── */
    if (s_show_system_info) {
        switch (act) {
            case ACT_CIRCLE:
            case ACT_START:
                s_show_system_info = false;
                break;
            default:
                break;
        }
        return current;
    }

    /* ── NETWORK SETTINGS MODE ── */
    if (s_show_network_settings) {
        switch (act) {
            case ACT_UP:
                s_network_cursor = (s_network_cursor - 1 + 2) % 2;
                break;
            case ACT_DOWN:
                s_network_cursor = (s_network_cursor + 1) % 2;
                break;
            case ACT_LEFT:
                if (s_network_cursor == 0) { /* Timeout */
                    int timeout = http_get_timeout_ms() / 1000;
                    if (timeout > 5) {
                        timeout -= 5;
                        http_set_timeout_ms(timeout * 1000);
                    }
                } else if (s_network_cursor == 1) { /* Max retries */
                    int retries = http_get_max_retries();
                    if (retries > 1) {
                        http_set_max_retries(retries - 1);
                    }
                }
                break;
            case ACT_RIGHT:
                if (s_network_cursor == 0) { /* Timeout */
                    int timeout = http_get_timeout_ms() / 1000;
                    if (timeout < 60) {
                        timeout += 5;
                        http_set_timeout_ms(timeout * 1000);
                    }
                } else if (s_network_cursor == 1) { /* Max retries */
                    int retries = http_get_max_retries();
                    if (retries < 15) {
                        http_set_max_retries(retries + 1);
                    }
                }
                break;
            case ACT_CIRCLE:
            case ACT_START:
                s_show_network_settings = false;
                break;
            default:
                break;
        }
        return current;
    }

    /* Open menu with START */
    if (act == ACT_START) {
        s_menu_open = true;
        s_menu_cursor = 0;
        return current;
    }

    /* ── SEARCH OSK STATE ── */
    if (current == STATE_SEARCH) {
        switch (act) {
            case ACT_UP:
                s_osk_row = (s_osk_row - 1 + 4) % 4;
                break;
            case ACT_DOWN:
                s_osk_row = (s_osk_row + 1) % 4;
                break;
            case ACT_LEFT:
                s_osk_col = (s_osk_col - 1 + 10) % 10;
                break;
            case ACT_RIGHT:
                s_osk_col = (s_osk_col + 1) % 10;
                break;
            case ACT_CROSS: {
                const char *k = s_osk[s_osk_row][s_osk_col];
                if (strcmp(k, "GO") == 0) {
                    if (s_search_len > 0) {
                        /* Trigger real async HTTP search */
                        s_display_count = 0;
                        s_cursor = 0;
                        http_search_async_start(s_search_query);
                        s_search_status = SEARCH_STATUS_BUSY;
                    }
                    return STATE_HOME;
                } else if (strcmp(k, "DEL") == 0) {
                    if (s_search_len > 0) {
                        s_search_query[--s_search_len] = '\0';
                    }
                } else if (strcmp(k, "SPC") == 0) {
                    if (s_search_len < (int)sizeof(s_search_query) - 1) {
                        s_search_query[s_search_len++] = ' ';
                        s_search_query[s_search_len] = '\0';
                    }
                } else {
                    if (s_search_len < (int)sizeof(s_search_query) - 1) {
                        s_search_query[s_search_len++] = k[0];
                        s_search_query[s_search_len] = '\0';
                    }
                }
                break;
            }
            case ACT_TRIANGLE: /* Quick delete in OSK */
                if (s_search_len > 0) {
                    s_search_query[--s_search_len] = '\0';
                }
                break;
            case ACT_CIRCLE: /* Cancel search */
                return STATE_HOME;
            default:
                break;
        }
        return current;
    }

    /* ── NOW PLAYING STATE ── */
    if (current == STATE_PLAYING) {
        switch (act) {
            case ACT_CROSS:
                /* If error (format not supported), skip to next track */
                if (audio_engine_has_error()) {
                    if (s_display_count > 0) {
                        s_current_track_idx = (s_current_track_idx + 1) % s_display_count;
                        s_playback_progress = 0.0f;
                        play_track_with_cover(s_current_track_idx);
                    }
                } else if (s_playback_state == PLAYBACK_PLAYING) {
                    s_playback_state = PLAYBACK_PAUSED;
                    audio_engine_pause();
                } else {
                    s_playback_state = PLAYBACK_PLAYING;
                    audio_engine_resume();
                }
                break;
            case ACT_CIRCLE:
                return STATE_HOME;
            case ACT_L1: /* Prev track in search results */
                if (s_display_count > 0) {
                    if (s_current_track_idx > 0) s_current_track_idx--;
                    else s_current_track_idx = s_display_count - 1;
                    s_playback_progress = 0.0f;
                    play_track_with_cover(s_current_track_idx);
                }
                break;
            case ACT_R1: /* Next track in search results */
                if (s_display_count > 0) {
                    s_current_track_idx = (s_current_track_idx + 1) % s_display_count;
                    s_playback_progress = 0.0f;
                    play_track_with_cover(s_current_track_idx);
                }
                break;
            case ACT_UP:
                audio_engine_set_volume(audio_engine_get_volume() + 0.05f);
                break;
            case ACT_DOWN:
                audio_engine_set_volume(audio_engine_get_volume() - 0.05f);
                break;
            case ACT_LEFT:
                s_playback_progress -= 0.05f;
                if (s_playback_progress < 0.0f) s_playback_progress = 0.0f;
                audio_engine_seek(s_playback_progress);
                break;
            case ACT_RIGHT:
                s_playback_progress += 0.05f;
                if (s_playback_progress > 1.0f) s_playback_progress = 1.0f;
                audio_engine_seek(s_playback_progress);
                break;
            case ACT_L2:
                s_repeat_mode = (RepeatMode)((s_repeat_mode + 1) % 3);
                break;
            case ACT_SELECT:
                s_shuffle = !s_shuffle;
                break;
            case ACT_TRIANGLE:
                return STATE_QUEUE;
            default:
                break;
        }
        return current;
    }

    /* ── QUEUE STATE ── */
    if (current == STATE_QUEUE) {
        switch (act) {
            case ACT_CIRCLE:
            case ACT_TRIANGLE:
                return STATE_HOME;
            case ACT_CROSS:
                if (s_display_count > 0 && s_cursor >= 0 && s_cursor < s_display_count) {
                    s_current_track_idx = s_cursor;
                    s_playback_state = PLAYBACK_PLAYING;
                    s_playback_progress = 0.0f;
                    play_track_with_cover(s_current_track_idx);
                    return STATE_PLAYING;
                }
                break;
            case ACT_UP:
                if (s_cursor > 0) s_cursor--;
                break;
            case ACT_DOWN:
                if (s_cursor + 1 < s_display_count) s_cursor++;
                break;
            default:
                break;
        }
        return current;
    }

    /* ── HOME STATE (Default) ── */
    switch (act) {
        case ACT_UP:
            if (s_display_count > 0) {
                if (s_cursor > 0) s_cursor--;
                else s_cursor = s_display_count - 1;
            }
            break;
        case ACT_DOWN:
            if (s_display_count > 0) {
                if (s_cursor + 1 < s_display_count) s_cursor++;
                else s_cursor = 0;
            }
            break;
        case ACT_CROSS:
            if (s_display_count > 0 && s_cursor >= 0 && s_cursor < s_display_count) {
                s_current_track_idx = s_cursor;
                s_playback_state = PLAYBACK_PLAYING;
                s_playback_progress = 0.0f;
                /* Play the EXACT ID selected and load cover */
                play_track_with_cover(s_current_track_idx);
                return STATE_PLAYING;
            }
            break;
        case ACT_SQUARE:
            return STATE_SEARCH;
        case ACT_TRIANGLE:
            return STATE_QUEUE;
        default:
            break;
    }

    return current;
}

/* ── Screen Renderers ── */

static void render_ascii_metalheart_background(int sw, int sh)
{
    /* Dark subtle metalheart technical markings (pure black 0x00000000 dominates) */
    platform_draw_text(25, 62, "+ [SYS: AUDIO_CORE // PPU_DMA]", 0x00222222, 1);
    platform_draw_text_r(sw - 25, 62, "STATUS: ONLINE // 48kHz LPCM +", 0x00222222, 1);

    platform_draw_text(35, sh / 3, "+ .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  +", 0x00151515, 1);
    platform_draw_text(35, (sh * 2) / 3, "+ .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  .  +", 0x00151515, 1);

    platform_draw_text(25, sh - 28, "// MONOCHROME.ST // LOSSLESS FLAC // CELL-BE //", 0x001C1C1C, 1);
    platform_draw_text_r(sw - 25, sh - 28, "LOC: 0x40100000 // BUFFER: 48MB //", 0x001C1C1C, 1);
}

static void render_header(int sw, const char *right_hint)
{
    platform_draw_rect(0, 0, sw, 50, 0x000F121B);
    platform_draw_rect(0, 50, sw, 2, 0xFFFFFFFF);
    platform_draw_text(30, 14, "MONOCHROME", 0xFFFFFFFF, 3);
    platform_draw_text(250, 20, APP_DISPLAY_VERSION, 0x00808080, 2);
    if (right_hint) {
        platform_draw_text_r(sw - 30, 18, right_hint, 0x00A0A0A0, 2);
    }
}

static void render_now_playing_mini_bar(int sw, int sh)
{
    int bar_y = sh - 90;
    int bar_h = 50;

    platform_draw_rect(0, bar_y, sw, bar_h, 0x00131B28);
    platform_draw_rect(0, bar_y, sw, 1, 0xFFFFFFFF);

    if (s_current_track_idx >= 0 && s_current_track_idx < s_display_count) {
        TrackMetadata *t = &s_display_tracks[s_current_track_idx];
        const char *icon = (s_playback_state == PLAYBACK_PLAYING) ? ">" : "||";
        platform_draw_text(30, bar_y + 15, icon, 0x00C83838, 3);

        char info[260];
        snprintf(info, sizeof(info), "%s - %s", t->title, t->artist);
        platform_draw_text(65, bar_y + 12, info, 0xFFFFFFFF, 2);

        char id_str[80];
        snprintf(id_str, sizeof(id_str), "ID: %s", t->id);
        platform_draw_text(65, bar_y + 32, id_str, 0xFFFFFFFF, 1);

        char dac[64];
        snprintf(dac, sizeof(dac), "%s | %uHz %dbit", t->is_hi_res ? "HI-RES" : "FLAC", t->sample_rate, t->bit_depth);
        platform_draw_text_r(sw - 30, bar_y + 16, dac, t->is_hi_res ? 0xFFFFFFFF : 0x00C83838, 2);
    } else {
        platform_draw_text(30, bar_y + 16, "-- Ningun tema seleccionado (Pulsa X en la lista) --", 0x00808080, 2);
    }

    float real_p = audio_engine_get_progress();
    if (real_p > 0.0f) {
        s_playback_progress = real_p;
    }
    int prog_w = (int)(sw * s_playback_progress);
    platform_draw_rect(0, bar_y + bar_h - 3, prog_w, 3, 0xFFFFFFFF);
}

static void render_footer(int sw, int sh, const char *hints)
{
    platform_draw_rect(0, sh - 40, sw, 40, 0x000F121B);
    platform_draw_text(30, sh - 28, hints, 0x00B0BEC5, 2);
}

static void render_home_screen(int sw, int sh)
{
    render_header(sw, "[X] Reproducir  [Cuadrado] Buscar  [Triangulo] Cola");

    /* Subheader with dynamic search status */
    platform_draw_rect(0, 52, sw, 32, 0x00141824);

    if (s_search_status == SEARCH_STATUS_BUSY) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Buscando '%s' en tracks.monochrome.st...", s_search_query);
        platform_draw_text(30, 58, msg, 0x00C83838, 2);
    } else if (s_search_status == SEARCH_STATUS_NO_RESULTS) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Sin resultados para '%s' (Pulsa [] para buscar de nuevo)", s_search_query);
        platform_draw_text(30, 58, msg, 0x00C83838, 2);
    } else if (s_search_status == SEARCH_STATUS_ERROR) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Error de red al consultar API (HTTP %d, errno %d)", s_last_http_code, s_last_errno);
        platform_draw_text(30, 58, msg, 0x00C83838, 2);
    } else {
        char f_info[128];
        snprintf(f_info, sizeof(f_info), "Resultados API: \"%s\" (%d temas reales recibidos)", s_search_query, s_display_count);
        platform_draw_text(30, 58, f_info, 0xFFFFFFFF, 2);
        platform_draw_text_r(sw - 30, 58, "[ONLINE: tracks.monochrome.st]", 0x00C83838, 2);
    }

    /* Track list */
    int list_y = 90;
    int row_h = 55;
    int max_rows = (sh - 90 - 100) / row_h;

    if (s_display_count == 0) {
        if (s_search_status == SEARCH_STATUS_BUSY) {
            platform_draw_text(sw / 2 - 200, sh / 2 - 30, "Conectando a Monochrome API...", 0xFFFFFFFF, 3);
            platform_draw_text(sw / 2 - 180, sh / 2 + 10, "Descargando lista de pistas por HTTP...", 0xFFFFFFFF, 2);
        } else if (s_search_status == SEARCH_STATUS_NO_RESULTS) {
            platform_draw_text(sw / 2 - 160, sh / 2, "No se encontraron pistas", 0x00C83838, 3);
        } else if (s_search_status == SEARCH_STATUS_ERROR) {
            platform_draw_text(sw / 2 - 180, sh / 2, "Error de conexion con el servidor", 0x00C83838, 3);
        }
    } else {
        for (int i = 0; i < max_rows && i < s_display_count; i++) {
            int ry = list_y + i * row_h;
            int is_sel = (i == s_cursor);
            TrackMetadata *t = &s_display_tracks[i];

            if (is_sel) {
                platform_draw_rect(20, ry, sw - 40, row_h - 4, 0x00243450);
                platform_draw_rect(20, ry, 6, row_h - 4, 0xFFFFFFFF);
                platform_draw_text(36, ry + 16, ">", 0xFFFFFFFF, 3);
            } else {
                platform_draw_rect(20, ry, sw - 40, row_h - 4, (i % 2 == 0) ? 0x00111722 : 0x00161E2C);
            }

            char num[8];
            snprintf(num, sizeof(num), "%02d.", i + 1);
            platform_draw_text(60, ry + 16, num, is_sel ? 0xFFFFFFFF : 0x00707070, 2);

            platform_draw_text(110, ry + 10, t->title, is_sel ? 0xFFFFFFFF : 0x00E0E0E0, 2);

            char subline[256];
            snprintf(subline, sizeof(subline), "%s  |  [ID: %s]", t->artist, t->id);
            platform_draw_text(110, ry + 32, subline, is_sel ? 0xFFFFFFFF : 0x00808080, 1);

            char dur[16];
            snprintf(dur, sizeof(dur), "%d:%02d", t->duration_sec / 60, t->duration_sec % 60);
            platform_draw_text_r(sw - 120, ry + 18, dur, 0x00909090, 2);

            const char *badge = t->is_hi_res ? "HI-RES" : "FLAC";
            platform_draw_text_r(sw - 40, ry + 18, badge, t->is_hi_res ? 0xFFFFFFFF : 0x00C83838, 2);
        }
    }

    render_now_playing_mini_bar(sw, sh);
    render_footer(sw, sh, "D-PAD: Mover  |  X: Reproducir Stream  |  []: Buscar  |  /\\: Cola  |  START: Salir");
}

static void render_playing_screen(int sw, int sh, long frame)
{
    render_header(sw, "REPRODUCIENDO (STREAMING LOSSLESS)");

    if (s_current_track_idx < 0 || s_current_track_idx >= s_display_count) {
        platform_draw_text(sw / 2 - 150, sh / 2, "Ningun tema en reproduccion", 0xFFFFFFFF, 2);
        render_footer(sw, sh, "O: Volver a la lista");
        return;
    }

    TrackMetadata *t = &s_display_tracks[s_current_track_idx];

    /* Card Left: Cover Art Box */
    int cover_x = 80;
    int cover_y = 100;
    int cover_size = 320;
    platform_draw_rect(cover_x - 3, cover_y - 3, cover_size + 6, cover_size + 6, 0xFFFFFFFF);
    platform_draw_rect(cover_x, cover_y, cover_size, cover_size, 0x001A2436);

    /* Draw cover art if loaded, otherwise show placeholder */
    if (cover_art_is_ready()) {
        platform_draw_texture(cover_art_get_texture(), 
                             cover_art_get_width(), cover_art_get_height(),
                             cover_x, cover_y, cover_size, cover_size);
    } else {
        platform_draw_rect(cover_x + 30, cover_y + 30, cover_size - 60, cover_size - 60, 0x000F1520);
        platform_draw_text(cover_x + 60, cover_y + 130, "MONOCHROME", 0xFFFFFFFF, 3);
        platform_draw_text(cover_x + 100, cover_y + 170, "CARGANDO...", 0x00C83838, 2);
    }

    /* Card Right: Track Details */
    int info_x = cover_x + cover_size + 60;
    int info_y = 110;

    platform_draw_text(info_x, info_y, t->title, 0xFFFFFFFF, 3);
    platform_draw_text(info_x, info_y + 35, t->artist, 0xFFFFFFFF, 2);
    if (t->album[0]) {
        platform_draw_text(info_x, info_y + 65, t->album, 0x0090A4AE, 2);
    }

    /* Track ID verification box */
    char id_box[256];
    snprintf(id_box, sizeof(id_box), "TRACK ID: %s (http://tracks.monochrome.st/track/%s)", t->id, t->id);
    platform_draw_rect(info_x, info_y + 95, 600, 24, 0x00101824);
    platform_draw_text(info_x + 10, info_y + 101, id_box, 0xFFFFFFFF, 1);

    /* Audio spec badges */
    char dac_spec[128];
    snprintf(dac_spec, sizeof(dac_spec), "FORMATO: LOSSLESS FLAC STREAM  |  SALIDA: 48000 Hz Float Stereo");
    platform_draw_rect(info_x, info_y + 130, 600, 32, 0x00141C2B);
    platform_draw_text(info_x + 15, info_y + 138, dac_spec, 0x00C83838, 2);

    /* 16-Band Visualizer */
    int vis_x = info_x;
    int vis_y = info_y + 200;
    platform_draw_text(vis_x, vis_y - 25, "ANALIZADOR DE ESPECTRO REAL-TIME (16 BANDAS):", 0x00808080, 1);
    float vis_bands[VIS_BANDS];
    audio_engine_get_vis(vis_bands, VIS_BANDS);
    for (int b = 0; b < VIS_BANDS; b++) {
        int bar_h = (int)(vis_bands[b] * 55.0f) + 4;
        if (bar_h > 58) bar_h = 58;
        int bar_x = vis_x + b * 28;
        int bar_y_pos = vis_y + 60 - bar_h;
        platform_draw_rect(bar_x, bar_y_pos, 20, bar_h, 0xFFFFFFFF);
        platform_draw_rect(bar_x, bar_y_pos - 3, 20, 2, 0xFFFFFFFF);
    }

    /* Live Volume & Audio Status */
    char vol_info[64];
    snprintf(vol_info, sizeof(vol_info), "VOLUMEN: %d%%  |  D-PAD ARRIBA/ABAJO: Cambiar  |  UNDERRUNS: %u",
             (int)(audio_engine_get_volume() * 100.0f), audio_engine_get_underruns());
    platform_draw_text(vis_x, vis_y + 75, vol_info, 0x00C83838, 1);

    /* Status message (shows errors like non-FLAC format) */
    const char *status_msg = audio_engine_get_status_message();
    if (status_msg && status_msg[0] != '\0') {
        int status_y = vis_y + 95;
        uint32_t status_color = audio_engine_has_error() ? 0x00C83838 : 0xFFFFFFFF;
        platform_draw_text(vis_x, status_y, status_msg, status_color, 1);
    }

    /* Progress bar */
    int bar_x = 80;
    int bar_y = sh - 150;
    int bar_w = sw - 160;
    int bar_h = 10;
    platform_draw_rect(bar_x, bar_y, bar_w, bar_h, 0x001C1C1C);

    float real_p = audio_engine_get_progress();
    if (real_p > 0.0f) {
        s_playback_progress = real_p;
    }
    int fill_w = (int)(bar_w * s_playback_progress);
    platform_draw_rect(bar_x, bar_y, fill_w, bar_h, 0xFFFFFFFF);

    /* Progress times */
    int cur_sec = (int)(t->duration_sec * s_playback_progress);
    char cur_t[16], tot_t[16];
    snprintf(cur_t, sizeof(cur_t), "%d:%02d", cur_sec / 60, cur_sec % 60);
    snprintf(tot_t, sizeof(tot_t), "%d:%02d", t->duration_sec / 60, t->duration_sec % 60);
    platform_draw_text(bar_x, bar_y + 18, cur_t, 0x00E0E0E0, 2);
    platform_draw_text_r(bar_x + bar_w, bar_y + 18, tot_t, 0x00E0E0E0, 2);

    const char *state_str = (s_playback_state == PLAYBACK_PLAYING) ? "[PLAYING]" : "[PAUSED]";
    platform_draw_text(bar_x + (bar_w / 2) - 50, bar_y + 18, state_str, (s_playback_state == PLAYBACK_PLAYING) ? 0x00C83838 : 0x00C83838, 2);

    render_footer(sw, sh, "X: Pausa/Reanudar  |  < >: Seek  |  L1/R1: Skip  |  O: Volver a Lista");
}

static void render_search_screen(int sw, int sh)
{
    render_header(sw, "BUSQUEDA ONLINE EN TIEMPO REAL");

    int card_w = 800;
    int card_h = 480;
    int card_x = (sw - card_w) / 2;
    int card_y = 80;

    platform_draw_rect(card_x - 3, card_y - 3, card_w + 6, card_h + 6, 0xFFFFFFFF);
    platform_draw_rect(card_x, card_y, card_w, card_h, 0x00141C2B);

    /* Query Box */
    platform_draw_rect(card_x + 30, card_y + 25, card_w - 60, 48, 0x000F1520);
    platform_draw_rect(card_x + 30, card_y + 25, card_w - 60, 1, 0xFFFFFFFF);

    char q_display[80];
    snprintf(q_display, sizeof(q_display), "Buscar en Monochrome: %s_", s_search_query);
    platform_draw_text(card_x + 45, card_y + 38, q_display, 0xFFFFFFFF, 2);

    /* OSK Grid */
    int key_w = 64;
    int key_h = 55;
    int grid_x = card_x + 45;
    int grid_y = card_y + 100;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 10; c++) {
            int kx = grid_x + c * (key_w + 8);
            int ky = grid_y + r * (key_h + 10);
            bool is_sel = (r == s_osk_row && c == s_osk_col);

            uint32_t bg = is_sel ? 0xFFFFFFFF : 0x001E2B3E;
            uint32_t fg = is_sel ? 0x00000000 : 0x00E0E0E0;

            platform_draw_rect(kx, ky, key_w, key_h, bg);
            if (is_sel) {
                platform_draw_rect(kx, ky, key_w, 2, 0xFFFFFFFF);
            }

            const char *label = s_osk[r][c];
            int text_scale = (strlen(label) > 1) ? 2 : 3;
            int text_offset_x = (strlen(label) == 3) ? 10 : (strlen(label) == 2) ? 16 : 22;
            platform_draw_text(kx + text_offset_x, ky + 16, label, fg, text_scale);
        }
    }

    platform_draw_text(card_x + 45, card_y + card_h - 40, "D-PAD: Navegar  |  X: Pulsar  |  /\\: Borrar  |  GO: Buscar en vivo  |  O: Cancelar", 0x00A0A0A0, 1);
    render_footer(sw, sh, "Escribe una palabra o artista y pulsa GO para buscar en la API");
}

static void render_queue_screen(int sw, int sh)
{
    render_header(sw, "COLA DE REPRODUCCION (QUEUE)");

    platform_draw_rect(0, 52, sw, 32, 0x00141824);
    platform_draw_text(30, 58, "Pistas en cola de streaming:", 0xFFFFFFFF, 2);

    int list_y = 95;
    int row_h = 60;
    for (int i = 0; i < s_display_count; i++) {
        int ry = list_y + i * row_h;
        TrackMetadata *t = &s_display_tracks[i];
        bool is_now = (i == s_current_track_idx);

        if (is_now) {
            platform_draw_rect(30, ry, sw - 60, row_h - 6, 0x001B4D2E);
            platform_draw_text(45, ry + 20, "STREAMING >", 0x00C83838, 2);
        } else {
            platform_draw_rect(30, ry, sw - 60, row_h - 6, 0x00141C2B);
            char q_num[16];
            snprintf(q_num, sizeof(q_num), "#%d", i + 1);
            platform_draw_text(45, ry + 20, q_num, 0x00757575, 2);
        }

        platform_draw_text(180, ry + 12, t->title, 0xFFFFFFFF, 2);

        char sub[256];
        snprintf(sub, sizeof(sub), "%s | ID: %s", t->artist, t->id);
        platform_draw_text(180, ry + 34, sub, 0x00909090, 1);

        char dur[16];
        snprintf(dur, sizeof(dur), "%d:%02d", t->duration_sec / 60, t->duration_sec % 60);
        platform_draw_text_r(sw - 50, ry + 20, dur, 0x00E0E0E0, 2);
    }

    render_now_playing_mini_bar(sw, sh);
    render_footer(sw, sh, "O: Volver  |  X: Saltar a esta pista  |  START: Salir");
}

void ui_render(AppState state, long frame)
{
    /* Poll background async search */
    int count = 0, hcode = 0, herrn = 0;
    SearchStatus st = http_search_async_poll(s_display_tracks, &count, MAX_SEARCH_RESULTS, &hcode, &herrn);
    if (st == SEARCH_STATUS_OK) {
        s_display_count = count;
        s_search_status = SEARCH_STATUS_OK;
        s_last_http_code = hcode;
        s_cursor = 0;
    } else if (st == SEARCH_STATUS_NO_RESULTS) {
        s_display_count = 0;
        s_search_status = SEARCH_STATUS_NO_RESULTS;
        s_last_http_code = hcode;
    } else if (st == SEARCH_STATUS_ERROR) {
        s_display_count = 0;
        s_search_status = SEARCH_STATUS_ERROR;
        s_last_http_code = hcode;
        s_last_errno = herrn;
    }

    int sw = platform_get_width();
    int sh = platform_get_height();

    platform_clear(0x00000000);

    /* Minimalist metalheart ASCII background */
    render_ascii_metalheart_background(sw, sh);

    switch (state) {
        case STATE_HOME:
            render_home_screen(sw, sh);
            break;
        case STATE_SEARCH:
            render_search_screen(sw, sh);
            break;
        case STATE_PLAYING:
            render_playing_screen(sw, sh, frame);
            break;
        case STATE_QUEUE:
            render_queue_screen(sw, sh);
            break;
        default:
            break;
    }

    /* Show error message overlay if track is not available in HiFi format */
    if (audio_engine_has_error() && s_playback_state == PLAYBACK_PLAYING) {
        const char *err_msg = audio_engine_get_status_message();
        if (err_msg && strstr(err_msg, "no disponible en FLAC")) {
            /* Draw error banner at bottom of screen */
            platform_draw_rect(0, sh - 60, sw, 60, 0xE0000000);
            platform_draw_text(20, sh - 40, "No disponible en calidad HiFi (FLAC)", 0x00C83838, 2);
            platform_draw_text(20, sh - 20, "Presiona X para intentar siguiente track", 0x00CCCCCC, 1);
        }
    }

    /* Screen adjustment overlay */
    if (s_screen_adjust_mode) {
        /* Draw semi-transparent overlay */
        platform_draw_rect(0, 0, sw, 100, 0x80000000);
        
        /* Draw instructions */
        char buf[128];
        snprintf(buf, sizeof(buf), "SCREEN ADJUST");
        platform_draw_text(20, 15, buf, 0xFFFFFF, 2);
        
        snprintf(buf, sizeof(buf), "D-Pad=Move | L2/R2=Scale | L1/R1=Font | START=Save | SELECT=Reset");
        platform_draw_text(20, 40, buf, 0xFFFFFFFF, 1);
        
        snprintf(buf, sizeof(buf), "Offset: (%d, %d) | Scale: %.2f | Font: %.2f",
                 s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale, s_adjust_font_scale);
        platform_draw_text(20, 65, buf, 0xFFFF00, 2);
    }

    /* Menu overlay */
    if (s_menu_open) {
        /* Draw semi-transparent background */
        int menu_w = 400;
        int menu_h = 300;  /* Increased for 4 items */
        int menu_x = (sw - menu_w) / 2;
        int menu_y = (sh - menu_h) / 2;
        
        platform_draw_rect(menu_x, menu_y, menu_w, menu_h, 0xE0000000);
        platform_draw_rect(menu_x + 2, menu_y + 2, menu_w - 4, menu_h - 4, 0x202030);
        
        /* Draw title */
        platform_draw_text(menu_x + 20, menu_y + 20, "MENU", 0xFFFFFFFF, 3);
        
        /* Draw menu items */
        for (int i = 0; i < MENU_ITEMS; i++) {
            uint32_t color = (i == s_menu_cursor) ? 0x00C83838 : 0xFFFFFF;
            char item_text[64];
            snprintf(item_text, sizeof(item_text), "> %s", s_menu_items[i]);
            platform_draw_text(menu_x + 40, menu_y + 80 + (i * 40), item_text, color, 2);
        }
        
        /* Draw instructions */
        platform_draw_text(menu_x + 20, menu_y + menu_h - 30, "D-Pad=Navigate | X=Select | O=Close", 0x808080, 1);
    }

    /* System Info overlay */
    if (s_show_system_info) {
        /* Draw semi-transparent background */
        int info_w = 600;
        int info_h = 400;
        int info_x = (sw - info_w) / 2;
        int info_y = (sh - info_h) / 2;
        
        platform_draw_rect(info_x, info_y, info_w, info_h, 0xE0000000);
        platform_draw_rect(info_x + 2, info_y + 2, info_w - 4, info_h - 4, 0x202030);
        
        /* Draw title */
        platform_draw_text(info_x + 20, info_y + 20, "SYSTEM INFO", 0xFFFFFFFF, 3);
        
        /* Draw system information */
        char buf[256];
        int y_offset = info_y + 80;
        
        snprintf(buf, sizeof(buf), "App: Monochrome PS3 %s", APP_DISPLAY_VERSION);
        platform_draw_text(info_x + 40, y_offset, buf, 0xFFFFFF, 2);
        y_offset += 40;
        
        snprintf(buf, sizeof(buf), "Title ID: %s", APP_TITLEID);
        platform_draw_text(info_x + 40, y_offset, buf, 0xFFFFFF, 2);
        y_offset += 40;
        
        snprintf(buf, sizeof(buf), "Screen: %dx%d", sw, sh);
        platform_draw_text(info_x + 40, y_offset, buf, 0xFFFFFF, 2);
        y_offset += 40;
        
        snprintf(buf, sizeof(buf), "Adjust: offset=(%d, %d) scale=%.2f", 
                 s_adjust_offset_x, s_adjust_offset_y, s_adjust_scale);
        platform_draw_text(info_x + 40, y_offset, buf, 0xFFFFFF, 2);
        y_offset += 40;
        
        snprintf(buf, sizeof(buf), "Network timeout: %ds", s_network_timeout);
        platform_draw_text(info_x + 40, y_offset, buf, 0xFFFFFF, 2);
        y_offset += 40;
        
        snprintf(buf, sizeof(buf), "Max retries: %d", s_max_retries);
        platform_draw_text(info_x + 40, y_offset, buf, 0xFFFFFF, 2);
        y_offset += 40;
        
        /* Draw instructions */
        platform_draw_text(info_x + 20, info_y + info_h - 30, "Press O or START to close", 0x808080, 1);
    }

    /* Network Settings overlay */
    if (s_show_network_settings) {
        /* Draw semi-transparent background */
        int net_w = 600;
        int net_h = 300;
        int net_x = (sw - net_w) / 2;
        int net_y = (sh - net_h) / 2;
        
        platform_draw_rect(net_x, net_y, net_w, net_h, 0xE0000000);
        platform_draw_rect(net_x + 2, net_y + 2, net_w - 4, net_h - 4, 0x202030);
        
        /* Draw title */
        platform_draw_text(net_x + 20, net_y + 20, "NETWORK SETTINGS", 0xFFFFFFFF, 3);
        
        /* Draw settings */
        char buf[256];
        int y_offset = net_y + 80;
        
        int timeout = http_get_timeout_ms() / 1000;
        int retries = http_get_max_retries();
        
        /* Timeout setting */
        uint32_t color0 = (s_network_cursor == 0) ? 0x00C83838 : 0xFFFFFF;
        snprintf(buf, sizeof(buf), "> Timeout: %d seconds  (LEFT/RIGHT to adjust)", timeout);
        platform_draw_text(net_x + 40, y_offset, buf, color0, 2);
        y_offset += 50;
        
        /* Max retries setting */
        uint32_t color1 = (s_network_cursor == 1) ? 0x00C83838 : 0xFFFFFF;
        snprintf(buf, sizeof(buf), "> Max retries: %d  (LEFT/RIGHT to adjust)", retries);
        platform_draw_text(net_x + 40, y_offset, buf, color1, 2);
        y_offset += 50;
        
        /* Instructions */
        platform_draw_text(net_x + 20, net_y + net_h - 50, "D-Pad=Navigate & Adjust", 0x808080, 1);
        platform_draw_text(net_x + 20, net_y + net_h - 30, "Press O or START to save & close", 0x808080, 1);
    }
}
