/*
 * Monochrome PS3 - HTTP Client & Async Search Pipeline (PSL1GHT)
 * Real REST API client for tracks.monochrome.st (plain HTTP port 80)
 */

#include <ppu-lv2.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#include <sysmodule/sysmodule.h>
#include <net/net.h>
#include <net/netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/thread.h>

#include "monochrome.h"
#include "http_client.h"
#include "cJSON.h"

#define STAGE_LOG_PATH "/dev_hdd0/tmp/mono_stage6.log"
#define API_HOST "tracks.monochrome.st"
#define API_PORT 80
#define RECV_BUF_SIZE (256 * 1024)

static int g_http_timeout_ms = 30000;  /* 30 seconds default */
static int g_http_max_retries = 8;

static void stage6_log(const char *fmt, ...)
{
    FILE *f = fopen(STAGE_LOG_PATH, "a");
    if (f) {
        time_t t = time(NULL);
        fprintf(f, "[%ld] ", (long)t);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fflush(f);
        fclose(f);
    }
}

static uint64_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static bool s_net_inited = false;

int http_net_init(void)
{
    if (s_net_inited) return 0;
    stage6_log("=== MONOCHROME STAGE 6: INICIALIZANDO RED ===");
    sysModuleLoad(SYSMODULE_NET);
    netInitialize();
    s_net_inited = true;
    stage6_log("[NET] Subsistema de red inicializado (libnet)");
    return 0;
}

void http_net_shutdown(void)
{
    if (!s_net_inited) return;
    stage6_log("[NET] Cerrando subsistema de red...");
    netDeinitialize();
    sysModuleUnload(SYSMODULE_NET);
    s_net_inited = false;
    stage6_log("[NET] Red cerrada limpiamente");
}

/* ── Synchronous HTTP Search ── */
int http_search_sync(const char *query, TrackMetadata *out_tracks, int max_tracks, int *out_http_code, int *out_errno)
{
    if (out_http_code) *out_http_code = 0;
    if (out_errno) *out_errno = 0;

    if (!query || query[0] == '\0') query = "daft";
    stage6_log("[BUSQUEDA INICIO] Query: '%s'", query);

    /* 1. URL Encode Query (spaces -> %20) */
    char enc_query[256];
    size_t eq_len = 0;
    for (size_t i = 0; query[i] && eq_len + 4 < sizeof(enc_query); i++) {
        if (query[i] == ' ') {
            enc_query[eq_len++] = '%';
            enc_query[eq_len++] = '2';
            enc_query[eq_len++] = '0';
        } else {
            enc_query[eq_len++] = query[i];
        }
    }
    enc_query[eq_len] = '\0';

    /* 2. DNS Resolution */
    uint64_t t_dns_start = get_time_ms();
    struct hostent *he = gethostbyname(API_HOST);
    uint64_t t_dns_end = get_time_ms();

    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        if (out_errno) *out_errno = errno;
        stage6_log("[ERROR] DNS fallo para %s (errno: %d, tiempo: %llu ms)",
                   API_HOST, errno, (unsigned long long)(t_dns_end - t_dns_start));
        return -1;
    }

    char ip_str[64] = "0.0.0.0";
    struct in_addr **addr_list = (struct in_addr **)he->h_addr_list;
    if (addr_list[0]) {
        strncpy(ip_str, inet_ntoa(*addr_list[0]), sizeof(ip_str) - 1);
    }
    stage6_log("[DNS] '%s' -> IP: %s (%llu ms)",
               API_HOST, ip_str, (unsigned long long)(t_dns_end - t_dns_start));

    /* 3. TCP Connect */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        if (out_errno) *out_errno = errno;
        stage6_log("[ERROR] socket(AF_INET) fallo (errno: %d)", errno);
        return -1;
    }

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(API_PORT);
    memcpy(&saddr.sin_addr, addr_list[0], sizeof(struct in_addr));

    uint64_t t_conn_start = get_time_ms();
    int ret = connect(sock, (struct sockaddr *)&saddr, sizeof(saddr));
    uint64_t t_conn_end = get_time_ms();

    if (ret != 0) {
        if (out_errno) *out_errno = errno;
        stage6_log("[ERROR] connect a %s:80 fallo (errno: %d, tiempo: %llu ms)",
                   ip_str, errno, (unsigned long long)(t_conn_end - t_conn_start));
        close(sock);
        return -1;
    }
    stage6_log("[TCP] Conectado a %s:80 (%llu ms)",
               ip_str, (unsigned long long)(t_conn_end - t_conn_start));

    /* 4. Send HTTP Request */
    char req[512];
    snprintf(req, sizeof(req),
             "GET /search?q=%s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: Monochrome-PS3/1.0 (PSL1GHT; CellOS)\r\n"
             "Accept: application/json\r\n"
             "Connection: close\r\n\r\n",
             enc_query, API_HOST);

    if (send(sock, req, strlen(req), 0) <= 0) {
        if (out_errno) *out_errno = errno;
        stage6_log("[ERROR] send HTTP request fallo (errno: %d)", errno);
        close(sock);
        return -1;
    }

    /* 5. Receive HTTP Response */
    char *resp_buf = (char *)malloc(RECV_BUF_SIZE);
    if (!resp_buf) {
        stage6_log("[ERROR] malloc fallo (%d bytes)", RECV_BUF_SIZE);
        close(sock);
        return -1;
    }

    size_t total_recv = 0;
    while (total_recv + 1 < RECV_BUF_SIZE) {
        ssize_t n = recv(sock, resp_buf + total_recv, RECV_BUF_SIZE - total_recv - 1, 0);
        if (n > 0) {
            total_recv += (size_t)n;
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EINTR) continue;
            break;
        }
    }
    resp_buf[total_recv] = '\0';
    close(sock);

    uint64_t t_req_end = get_time_ms();
    uint64_t total_time = t_req_end - t_dns_start;

    /* 6. Parse Status Code */
    int http_code = 0;
    sscanf(resp_buf, "HTTP/1.1 %d", &http_code);
    if (http_code == 0) sscanf(resp_buf, "HTTP/1.0 %d", &http_code);
    if (out_http_code) *out_http_code = http_code;

    if (http_code != 200) {
        stage6_log("[HTTP ERROR] Codigo HTTP %d para query '%s' (%llu ms)",
                   http_code, query, (unsigned long long)total_time);
        free(resp_buf);
        return -1;
    }

    /* 7. Locate JSON Body */
    char *body = strstr(resp_buf, "\r\n\r\n");
    if (!body) body = strstr(resp_buf, "\n\n");
    if (body) {
        body = (body[1] == '\n') ? body + 2 : body + 4;
    } else {
        body = resp_buf;
    }

    char *first_brace = strchr(body, '{');
    char *first_bracket = strchr(body, '[');
    char *json_start = body;
    if (first_brace && (!first_bracket || first_brace < first_bracket)) json_start = first_brace;
    else if (first_bracket) json_start = first_bracket;

    /* 8. Parse JSON */
    cJSON *root = cJSON_Parse(json_start);
    if (!root) {
        stage6_log("[JSON ERROR] cJSON_Parse fallo");
        free(resp_buf);
        return -1;
    }

    /* Parse release titles for album names */
    cJSON *j_releases = cJSON_GetObjectItemCaseSensitive(root, "releases");

    cJSON *j_tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    if (!j_tracks) j_tracks = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!j_tracks && cJSON_IsArray(root)) j_tracks = root;

    int parsed_count = 0;
    if (j_tracks && cJSON_IsArray(j_tracks)) {
        int n = cJSON_GetArraySize(j_tracks);

        for (int i = 0; i < n && parsed_count < max_tracks; i++) {
            cJSON *item = cJSON_GetArrayItem(j_tracks, i);
            if (!item) continue;

            TrackMetadata *t = &out_tracks[parsed_count];
            memset(t, 0, sizeof(TrackMetadata));

            /* ID */
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "trackId");
            if (jid && cJSON_IsString(jid)) {
                strncpy(t->id, jid->valuestring, sizeof(t->id) - 1);
            }

            /* Title */
            cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(item, "title");
            if (jtitle && cJSON_IsString(jtitle)) {
                strncpy(t->title, jtitle->valuestring, sizeof(t->title) - 1);
            }

            /* Artist */
            cJSON *jnames = cJSON_GetObjectItemCaseSensitive(item, "artistNames");
            if (jnames && cJSON_IsArray(jnames) && cJSON_GetArraySize(jnames) > 0) {
                cJSON *first_a = cJSON_GetArrayItem(jnames, 0);
                if (first_a && cJSON_IsString(first_a)) {
                    strncpy(t->artist, first_a->valuestring, sizeof(t->artist) - 1);
                }
            } else {
                cJSON *ja = cJSON_GetObjectItemCaseSensitive(item, "artist");
                if (ja && cJSON_IsString(ja)) {
                    strncpy(t->artist, ja->valuestring, sizeof(t->artist) - 1);
                }
            }

            /* Album from releaseId match */
            cJSON *jrel_id = cJSON_GetObjectItemCaseSensitive(item, "releaseId");
            if (jrel_id && cJSON_IsString(jrel_id) && j_releases && cJSON_IsArray(j_releases)) {
                int n_rel = cJSON_GetArraySize(j_releases);
                for (int r = 0; r < n_rel; r++) {
                    cJSON *rel = cJSON_GetArrayItem(j_releases, r);
                    cJSON *rid = cJSON_GetObjectItemCaseSensitive(rel, "id");
                    if (!rid) rid = cJSON_GetObjectItemCaseSensitive(rel, "releaseId");
                    if (rid && cJSON_IsString(rid) && strcmp(rid->valuestring, jrel_id->valuestring) == 0) {
                        cJSON *rtitle = cJSON_GetObjectItemCaseSensitive(rel, "title");
                        if (rtitle && cJSON_IsString(rtitle)) {
                            strncpy(t->album, rtitle->valuestring, sizeof(t->album) - 1);
                        }
                        break;
                    }
                }
            }

            /* Duration (milliseconds from API -> seconds) */
            cJSON *jdur = cJSON_GetObjectItemCaseSensitive(item, "duration");
            if (jdur && cJSON_IsNumber(jdur)) {
                t->duration_sec = (uint32_t)(jdur->valuedouble / 1000.0);
                if (t->duration_sec == 0) t->duration_sec = (uint32_t)(jdur->valueint / 1000);
            } else {
                t->duration_sec = 210;
            }

            /* Artwork URL */
            cJSON *jartwork = cJSON_GetObjectItemCaseSensitive(item, "artwork");
            if (jartwork && cJSON_IsString(jartwork)) {
                strncpy(t->cover_url, jartwork->valuestring, sizeof(t->cover_url) - 1);
            } else {
                t->cover_url[0] = '\0';
            }

            t->sample_rate = 44100;
            t->bit_depth = 16;
            t->is_hi_res = false;

            parsed_count++;
        }
    }

    cJSON_Delete(root);
    free(resp_buf);

    stage6_log("[BUSQUEDA RESULTADO] Query: '%s' | HTTP: %d | Resultados: %d | Tiempo: %llu ms",
               query, http_code, parsed_count, (unsigned long long)total_time);

    for (int i = 0; i < parsed_count; i++) {
        stage6_log("  - [%d] ID: %s | Titulo: \"%s\" | Artista: %s (%u:%02u)",
                   i + 1, out_tracks[i].id, out_tracks[i].title, out_tracks[i].artist,
                   out_tracks[i].duration_sec / 60, out_tracks[i].duration_sec % 60);
    }

    return parsed_count;
}

/* ── Asynchronous Background Search Worker ── */
static sys_ppu_thread_t s_search_thread_id;
static volatile bool s_search_thread_active = false;

static char s_async_query[128] = "";
static TrackMetadata s_async_results[MAX_SEARCH_RESULTS];
static int s_async_result_count = 0;
static int s_async_http_code = 0;
static int s_async_errno = 0;
static volatile SearchStatus s_async_status = SEARCH_STATUS_IDLE;

static void search_worker_func(void *arg)
{
    (void)arg;
    int code = 0, err = 0;
    int count = http_search_sync(s_async_query, s_async_results, MAX_SEARCH_RESULTS, &code, &err);

    s_async_http_code = code;
    s_async_errno = err;

    if (count > 0) {
        s_async_result_count = count;
        s_async_status = SEARCH_STATUS_OK;
    } else if (count == 0) {
        s_async_result_count = 0;
        s_async_status = SEARCH_STATUS_NO_RESULTS;
    } else {
        s_async_result_count = 0;
        s_async_status = SEARCH_STATUS_ERROR;
    }

    s_search_thread_active = false;
    sysThreadExit(0);
}

void http_search_async_start(const char *query)
{
    if (s_search_thread_active) {
        return; /* Already busy */
    }

    strncpy(s_async_query, query ? query : "daft", sizeof(s_async_query) - 1);
    s_async_status = SEARCH_STATUS_BUSY;
    s_search_thread_active = true;

    int ret = sysThreadCreate(&s_search_thread_id, search_worker_func, NULL, 1500,
                              0x20000, THREAD_JOINABLE, "mono_search");
    if (ret != 0) {
        stage6_log("[ERROR] sysThreadCreate(mono_search) fallo: %d", ret);
        s_search_thread_active = false;
        s_async_status = SEARCH_STATUS_ERROR;
    }
}

SearchStatus http_search_async_poll(TrackMetadata *out_tracks, int *out_count, int max_tracks, int *out_http_code, int *out_errno)
{
    if (out_http_code) *out_http_code = s_async_http_code;
    if (out_errno) *out_errno = s_async_errno;

    if (s_async_status == SEARCH_STATUS_OK) {
        int to_copy = (s_async_result_count < max_tracks) ? s_async_result_count : max_tracks;
        if (out_tracks && to_copy > 0) {
            memcpy(out_tracks, s_async_results, to_copy * sizeof(TrackMetadata));
        }
        if (out_count) *out_count = to_copy;
        s_async_status = SEARCH_STATUS_IDLE;
        return SEARCH_STATUS_OK;
    }

    return s_async_status;
}

const char *http_search_get_active_query(void)
{
    return s_async_query;
}

int http_get_timeout_ms(void)
{
    return g_http_timeout_ms;
}

void http_set_timeout_ms(int timeout_ms)
{
    if (timeout_ms < 5000) timeout_ms = 5000;
    if (timeout_ms > 120000) timeout_ms = 120000;
    g_http_timeout_ms = timeout_ms;
}

int http_get_max_retries(void)
{
    return g_http_max_retries;
}

void http_set_max_retries(int max_retries)
{
    if (max_retries < 1) max_retries = 1;
    if (max_retries > 20) max_retries = 20;
    g_http_max_retries = max_retries;
}

