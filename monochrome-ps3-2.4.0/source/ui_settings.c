#include "ui_settings.h"
#include <stdio.h>
#include <string.h>

#include "monochrome.h"
#include "app_state.h"
#include "gfx_engine.h"
#include "ui_theme.h"
#include "ui_shell.h"
#include "platform.h"
#include "http_client.h"
#include "net_hal.h"

static int s_sel = 0;

static const char *net_state_label(NetHalState st) {
    switch (st) {
        case NET_HAL_DISCONNECTED: return "DISCONNECTED";
        case NET_HAL_CONNECTING:   return "CONNECTING";
        case NET_HAL_IP_OBTAINED:  return "IP OBTAINED";
        case NET_HAL_ERROR:        return "ERROR";
        default:                   return "UNKNOWN";
    }
}

void ui_settings_init(void) {
    s_sel = 0;
}

void ui_settings_render(void) {
    int x0, y0, x1, y1;
    shell_get_main_area(&x0, &y0, &x1, &y1);
    int mw = x1 - x0;
    int mh = y1 - y0;
    (void)mh;
    int cx = x0 + 24;
    int cy = y0 + 16;
    int content_w = mw - 48;

    gfx_draw_text(cx, cy, "AJUSTES //", 15.0f, TH_ACCENT);
    cy += 36;

    int row_h = 44;

    /* Fila 0: Escalado de Zona Segura / Safe Area */
    bool foc0 = (s_sel == 0);
    gfx_draw_rect(cx, cy, content_w, row_h, foc0 ? TH_SURFACE_2 : TH_SURFACE);
    if (foc0) {
        gfx_draw_rect_outline(cx, cy, content_w, row_h, TH_BORDER_LT, 1.0f);
        gfx_draw_rect(cx, cy, 4, row_h, TH_ACCENT_LT);
    } else {
        gfx_draw_rect(cx, cy + row_h - 1, content_w, 1, TH_BORDER);
    }

    int pct = (int)(g_app_state.overscan_scale * 100.0f + 0.5f);
    char line[256];
    snprintf(line, sizeof(line), "Escalado de Zona Segura: < %d%% (%.2fx) >", pct, g_app_state.overscan_scale);
    gfx_draw_text(cx + 16, cy + 14, line, 14.0f, foc0 ? TH_TEXT : TH_TEXT_DIM);

    /* Barra visual en la derecha */
    int slider_w = 180;
    int slider_h = 8;
    int slider_x = cx + content_w - slider_w - 20;
    int slider_y = cy + (row_h - slider_h) / 2;
    gfx_draw_rect(slider_x, slider_y, slider_w, slider_h, TH_SURFACE);
    gfx_draw_rect_outline(slider_x, slider_y, slider_w, slider_h, TH_BORDER, 1.0f);
    float ratio = (g_app_state.overscan_scale - 0.75f) / 0.25f;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    int fill_w = (int)(ratio * (float)slider_w);
    if (fill_w > 0) {
        gfx_draw_rect(slider_x, slider_y, fill_w, slider_h, foc0 ? TH_ACCENT_LT : TH_ACCENT);
    }
    cy += row_h + 10;

    /* Fila 1: Timeout HTTP de Red */
    bool foc1 = (s_sel == 1);
    gfx_draw_rect(cx, cy, content_w, row_h, foc1 ? TH_SURFACE_2 : TH_SURFACE);
    if (foc1) {
        gfx_draw_rect_outline(cx, cy, content_w, row_h, TH_BORDER_LT, 1.0f);
        gfx_draw_rect(cx, cy, 4, row_h, TH_ACCENT_LT);
    } else {
        gfx_draw_rect(cx, cy + row_h - 1, content_w, 1, TH_BORDER);
    }
    snprintf(line, sizeof(line), "Timeout de Red (HTTP): < %u ms (%u s) >",
             g_app_state.http_timeout_ms, g_app_state.http_timeout_ms / 1000u);
    gfx_draw_text(cx + 16, cy + 14, line, 14.0f, foc1 ? TH_TEXT : TH_TEXT_DIM);
    cy += row_h + 10;

    /* Fila 2: Restablecer valores por defecto */
    bool foc2 = (s_sel == 2);
    gfx_draw_rect(cx, cy, content_w, row_h, foc2 ? TH_SURFACE_2 : TH_SURFACE);
    if (foc2) {
        gfx_draw_rect_outline(cx, cy, content_w, row_h, TH_BORDER_LT, 1.0f);
        gfx_draw_rect(cx, cy, 4, row_h, TH_ACCENT_LT);
    } else {
        gfx_draw_rect(cx, cy + row_h - 1, content_w, 1, TH_BORDER);
    }
    gfx_draw_text(cx + 16, cy + 14, "Restablecer Zona Segura al 100% (Pantalla Completa)", 14.0f, foc2 ? TH_TEXT : TH_TEXT_DIM);
    cy += row_h + 20;

    /* Info de sistema + telemetria en vivo (box estructurado) */
    NetHalTelemetry tel;
    net_hal_get_telemetry(&tel);

    int box_h = 248;
    gfx_draw_rect(cx, cy, content_w, box_h, TH_SURFACE);
    gfx_draw_rect_outline(cx, cy, content_w, box_h, TH_BORDER, 1.0f);

    gfx_draw_text(cx + 20, cy + 14, "DIAGNOSTICO DEL SISTEMA & TELEMETRIA // MONOCHROME PS3", 13.0f, TH_ACCENT);

    int sy = cy + 40;
    snprintf(line, sizeof(line), "BUILD SHA:      %s  |  COMPILACION: %s", APP_BUILD_SHA, APP_BUILD_TIME);
    gfx_draw_text(cx + 20, sy, line, 12.0f, TH_TEXT_DIM); sy += 22;

    snprintf(line, sizeof(line), "TITLE ID:       MONO00024 (CONTENT ID: UP0001-MONO00024_00)");
    gfx_draw_text(cx + 20, sy, line, 12.0f, TH_TEXT_DIM); sy += 22;

    snprintf(line, sizeof(line), "NETCTL:         %s  |  IP: %s%s",
             net_state_label(tel.state),
             tel.ip_str[0] ? tel.ip_str : "0.0.0.0",
             net_hal_is_menu_paused() ? "  [PAUSA XMB]" : "");
    gfx_draw_text_clipped(cx + 20, sy, (float)(content_w - 40), line, 12.0f,
                          (tel.state == NET_HAL_IP_OBTAINED) ? TH_ACCENT_LT : TH_TEXT_DIM);
    sy += 22;

    snprintf(line, sizeof(line), "DNS:            %s -> %s  |  h_errno: %d",
             tel.last_dns_host[0] ? tel.last_dns_host : "-",
             tel.last_dns_ip[0] ? tel.last_dns_ip : "-",
             tel.last_h_errno);
    gfx_draw_text_clipped(cx + 20, sy, (float)(content_w - 40), line, 12.0f, TH_TEXT_DIM); sy += 22;

    snprintf(line, sizeof(line), "API:            HTTP %d  |  errno: %d  |  Latencia: %u ms",
             tel.last_http_code, tel.last_net_errno, tel.last_api_latency_ms);
    gfx_draw_text_clipped(cx + 20, sy, (float)(content_w - 40), line, 12.0f, TH_TEXT_DIM); sy += 22;

    snprintf(line, sizeof(line), "STREAM:         %u kbps  |  Reconexiones: %u  |  Underruns: %u",
             tel.stream_kbps, tel.reconnect_count, tel.ring_underruns);
    gfx_draw_text_clipped(cx + 20, sy, (float)(content_w - 40), line, 12.0f, TH_TEXT_DIM); sy += 22;

    snprintf(line, sizeof(line), "HTTP TIMEOUT:   %u ms  |  SERVIDOR: tracks.monochrome.st:80", g_app_state.http_timeout_ms);
    gfx_draw_text(cx + 20, sy, line, 12.0f, TH_TEXT_DIM);

    gfx_draw_text(cx, y1 - 32, "[D-PAD IZQ/DER] AJUSTAR   [X] PREAJUSTES / ACCION   [O] MENU   [L1/R1] PESTAÑAS", 12.0f, TH_TEXT_DIM);
}

void ui_settings_handle_input(InputAction act) {
    switch (act) {
        case ACT_UP:
            if (s_sel > 0) s_sel--;
            break;
        case ACT_DOWN:
            if (s_sel < 2) s_sel++;
            break;
        case ACT_LEFT: {
            if (s_sel == 0) {
                float sc = g_app_state.overscan_scale - 0.01f;
                if (sc < 0.75f) sc = 0.75f;
                g_app_state.overscan_scale = (float)((int)(sc * 100.0f + 0.5f)) / 100.0f;
                platform_set_screen_adjust(g_app_state.overscan_x, g_app_state.overscan_y, g_app_state.overscan_scale);
                app_state_save_config();
                platform_save_screen_config();
            } else if (s_sel == 1) {
                uint32_t t = (g_app_state.http_timeout_ms > 1000u) ? (g_app_state.http_timeout_ms - 1000u) : 5000u;
                if (t < 5000u) t = 5000u;
                g_app_state.http_timeout_ms = t;
                http_set_timeout_ms((int)t);
                app_state_save_config();
            }
            break;
        }
        case ACT_RIGHT: {
            if (s_sel == 0) {
                float sc = g_app_state.overscan_scale + 0.01f;
                if (sc > 1.00f) sc = 1.00f;
                g_app_state.overscan_scale = (float)((int)(sc * 100.0f + 0.5f)) / 100.0f;
                platform_set_screen_adjust(g_app_state.overscan_x, g_app_state.overscan_y, g_app_state.overscan_scale);
                app_state_save_config();
                platform_save_screen_config();
            } else if (s_sel == 1) {
                uint32_t t = g_app_state.http_timeout_ms + 1000u;
                if (t > 30000u) t = 30000u;
                g_app_state.http_timeout_ms = t;
                http_set_timeout_ms((int)t);
                app_state_save_config();
            }
            break;
        }
        case ACT_CROSS: {
            if (s_sel == 0) {
                /* Ciclar preajustes comunes: 100% -> 95% -> 90% -> 85% -> 80% -> 100% */
                int pct = (int)(g_app_state.overscan_scale * 100.0f + 0.5f);
                if (pct >= 98) pct = 95;
                else if (pct >= 93) pct = 90;
                else if (pct >= 88) pct = 85;
                else if (pct >= 83) pct = 80;
                else pct = 100;
                g_app_state.overscan_scale = (float)pct / 100.0f;
                platform_set_screen_adjust(g_app_state.overscan_x, g_app_state.overscan_y, g_app_state.overscan_scale);
                app_state_save_config();
                platform_save_screen_config();
            } else if (s_sel == 1) {
                /* Ciclar presets: 5s -> 10s -> 15s -> 20s -> 30s -> 5s */
                uint32_t t = g_app_state.http_timeout_ms;
                if (t < 5000u) t = 5000u;
                else if (t < 10000u) t = 10000u;
                else if (t < 15000u) t = 15000u;
                else if (t < 20000u) t = 20000u;
                else if (t < 30000u) t = 30000u;
                else t = 5000u;
                g_app_state.http_timeout_ms = t;
                http_set_timeout_ms((int)t);
                app_state_save_config();
            } else if (s_sel == 2) {
                g_app_state.overscan_scale = 1.0f;
                g_app_state.overscan_x = 0;
                g_app_state.overscan_y = 0;
                platform_set_screen_adjust(0, 0, 1.0f);
                app_state_save_config();
                platform_save_screen_config();
            }
            break;
        }
        default: break;
    }
}
