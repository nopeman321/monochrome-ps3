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

static AlbumMetadata s_async_albums[MAX_SEARCH_RESULTS];
static int s_async_album_count = 0;
static ArtistMetadata s_async_artists[MAX_SEARCH_RESULTS];
static int s_async_artist_count = 0;
static PlaylistMetadata s_async_playlists[MAX_SEARCH_RESULTS];
static int s_async_playlist_count = 0;

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
    stage6_log("[NET] Subsistema de red desmarcado para salida");
    s_net_inited = false;
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
             "GET /search?q=%s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "User-Agent: Monochrome-PS3/1.0 (PSL1GHT; CellOS)\r\n"
             "Accept: application/json\r\n"
             "Accept-Encoding: identity\r\n"
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

    /* Parse Releases / Albums */
    s_async_album_count = 0;
    if (j_releases && cJSON_IsArray(j_releases)) {
        int n_rel = cJSON_GetArraySize(j_releases);
        for (int i = 0; i < n_rel && s_async_album_count < MAX_SEARCH_RESULTS; i++) {
            cJSON *item = cJSON_GetArrayItem(j_releases, i);
            if (!item) continue;
            AlbumMetadata *alb = &s_async_albums[s_async_album_count];
            memset(alb, 0, sizeof(AlbumMetadata));

            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "releaseId");
            if (jid && cJSON_IsString(jid)) strncpy(alb->id, jid->valuestring, sizeof(alb->id) - 1);

            cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(item, "title");
            if (jtitle && cJSON_IsString(jtitle)) strncpy(alb->title, jtitle->valuestring, sizeof(alb->title) - 1);

            cJSON *jnames = cJSON_GetObjectItemCaseSensitive(item, "artistNames");
            if (jnames && cJSON_IsArray(jnames) && cJSON_GetArraySize(jnames) > 0) {
                cJSON *fa = cJSON_GetArrayItem(jnames, 0);
                if (fa && cJSON_IsString(fa)) strncpy(alb->artist, fa->valuestring, sizeof(alb->artist) - 1);
            }

            cJSON *jart = cJSON_GetObjectItemCaseSensitive(item, "artwork");
            if (jart && cJSON_IsString(jart)) strncpy(alb->cover_url, jart->valuestring, sizeof(alb->cover_url) - 1);

            cJSON *jtype = cJSON_GetObjectItemCaseSensitive(item, "releaseType");
            if (jtype && cJSON_IsString(jtype)) strncpy(alb->type, jtype->valuestring, sizeof(alb->type) - 1);
            else strcpy(alb->type, "ALBUM");

            cJSON *jdate = cJSON_GetObjectItemCaseSensitive(item, "releaseDate");
            if (jdate && cJSON_IsString(jdate) && strlen(jdate->valuestring) >= 4) {
                strncpy(alb->year, jdate->valuestring, 4);
                alb->year[4] = '\0';
            }

            s_async_album_count++;
        }
    }

    /* Parse Artists */
    s_async_artist_count = 0;
    cJSON *j_artists = cJSON_GetObjectItemCaseSensitive(root, "artists");
    if (j_artists && cJSON_IsArray(j_artists)) {
        int n_art = cJSON_GetArraySize(j_artists);
        for (int i = 0; i < n_art && s_async_artist_count < MAX_SEARCH_RESULTS; i++) {
            cJSON *item = cJSON_GetArrayItem(j_artists, i);
            if (!item) continue;
            ArtistMetadata *art = &s_async_artists[s_async_artist_count];
            memset(art, 0, sizeof(ArtistMetadata));

            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "artistId");
            if (jid && cJSON_IsString(jid)) strncpy(art->id, jid->valuestring, sizeof(art->id) - 1);

            cJSON *jname = cJSON_GetObjectItemCaseSensitive(item, "name");
            if (!jname) jname = cJSON_GetObjectItemCaseSensitive(item, "displayName");
            if (jname && cJSON_IsString(jname)) strncpy(art->name, jname->valuestring, sizeof(art->name) - 1);

            cJSON *javatar = cJSON_GetObjectItemCaseSensitive(item, "avatar");
            if (javatar && cJSON_IsString(javatar)) strncpy(art->avatar_url, javatar->valuestring, sizeof(art->avatar_url) - 1);

            cJSON *jbio = cJSON_GetObjectItemCaseSensitive(item, "bio");
            if (jbio && cJSON_IsString(jbio)) strncpy(art->bio, jbio->valuestring, sizeof(art->bio) - 1);

            s_async_artist_count++;
        }
    }

    /* Parse Playlists */
    s_async_playlist_count = 0;
    cJSON *j_playlists = cJSON_GetObjectItemCaseSensitive(root, "playlists");
    if (j_playlists && cJSON_IsArray(j_playlists)) {
        int n_play = cJSON_GetArraySize(j_playlists);
        for (int i = 0; i < n_play && s_async_playlist_count < MAX_SEARCH_RESULTS; i++) {
            cJSON *item = cJSON_GetArrayItem(j_playlists, i);
            if (!item) continue;
            PlaylistMetadata *pl = &s_async_playlists[s_async_playlist_count];
            memset(pl, 0, sizeof(PlaylistMetadata));

            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "playlistId");
            if (jid && cJSON_IsString(jid)) strncpy(pl->id, jid->valuestring, sizeof(pl->id) - 1);

            cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(item, "title");
            if (jtitle && cJSON_IsString(jtitle)) strncpy(pl->title, jtitle->valuestring, sizeof(pl->title) - 1);

            cJSON *jowner = cJSON_GetObjectItemCaseSensitive(item, "ownerId");
            if (jowner && cJSON_IsString(jowner)) strncpy(pl->curator, jowner->valuestring, sizeof(pl->curator) - 1);

            cJSON *jdesc = cJSON_GetObjectItemCaseSensitive(item, "description");
            if (jdesc && cJSON_IsString(jdesc)) strncpy(pl->description, jdesc->valuestring, sizeof(pl->description) - 1);

            cJSON *jtc = cJSON_GetObjectItemCaseSensitive(item, "trackCount");
            if (jtc && cJSON_IsNumber(jtc)) pl->track_count = (uint32_t)jtc->valueint;

            s_async_playlist_count++;
        }
    }

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

static enum {
    ASYNC_REQ_SEARCH = 0,
    ASYNC_REQ_ALBUM,
    ASYNC_REQ_ARTIST
} s_async_req_type = ASYNC_REQ_SEARCH;
static char s_async_release_id[64] = "";
static char s_async_album_title[128] = "";
static char s_async_artist_id[64] = "";
static char s_async_artist_name[128] = "";

static char s_async_query[128] = "";
static TrackMetadata s_async_results[MAX_SEARCH_RESULTS];
static int s_async_result_count = 0;
static int s_async_http_code = 0;
static int s_async_errno = 0;
static volatile SearchStatus s_async_status = SEARCH_STATUS_IDLE;

/* ── Fetch Album Tracks by Release ID (ordered 1..N) ── */
int http_fetch_album_tracks_sync(const char *release_id, const char *album_title,
                                TrackMetadata *out_tracks, int max_tracks,
                                int *out_http_code, int *out_errno)
{
    if (out_http_code) *out_http_code = 0;
    if (out_errno) *out_errno = 0;
    if (!release_id || release_id[0] == '\0') return -1;

    stage6_log("[ALBUM INICIO] Release ID: '%s', Titulo: '%s'", release_id, album_title ? album_title : "");

    /* 1. DNS Resolution */
    struct hostent *he = gethostbyname(API_HOST);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        if (out_errno) *out_errno = errno;
        stage6_log("[ERROR] DNS fallo para %s (errno: %d)", API_HOST, errno);
        return -1;
    }

    struct in_addr api_ip;
    memcpy(&api_ip, he->h_addr_list[0], sizeof(struct in_addr));

    /* 2. Create Socket */
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        if (out_errno) *out_errno = errno;
        stage6_log("[ERROR] socket() fallo (errno: %d)", errno);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = g_http_timeout_ms / 1000;
    tv.tv_usec = (g_http_timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* 3. Connect */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(API_PORT);
    addr.sin_addr = api_ip;

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (out_errno) *out_errno = errno;
        stage6_log("[ERROR] connect() fallo a %s:%d (errno: %d)", API_HOST, API_PORT, errno);
        close(sock);
        return -1;
    }

    /* 4. Send GET /releases/<id> */
    char req[512];
    snprintf(req, sizeof(req),
             "GET /releases/%s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "User-Agent: Monochrome-PS3/1.0 (PSL1GHT; CellOS)\r\n"
             "Accept: application/json\r\n"
             "Accept-Encoding: identity\r\n"
             "Connection: close\r\n\r\n",
             release_id, API_HOST);

    if (send(sock, req, strlen(req), 0) <= 0) {
        if (out_errno) *out_errno = errno;
        stage6_log("[ERROR] send() fallo (errno: %d)", errno);
        close(sock);
        return -1;
    }

    /* 5. Receive HTTP Response */
    char *resp_buf = (char *)malloc(RECV_BUF_SIZE);
    if (!resp_buf) {
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

    /* 6. Parse HTTP status */
    int http_code = 0;
    if (sscanf(resp_buf, "HTTP/%*s %d", &http_code) == 1) {
        if (out_http_code) *out_http_code = http_code;
    }
    if (http_code != 200) {
        stage6_log("[ALBUM ERROR] HTTP %d para release %s", http_code, release_id);
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

    cJSON *root = cJSON_Parse(json_start);
    if (!root) {
        stage6_log("[ALBUM ERROR] cJSON_Parse fallo para release %s (snippet: %.80s)", release_id, json_start);
        free(resp_buf);
        return -1;
    }

    char album_art[256] = "";
    cJSON *j_art = cJSON_GetObjectItemCaseSensitive(root, "artwork");
    if (j_art && cJSON_IsString(j_art)) {
        strncpy(album_art, j_art->valuestring, sizeof(album_art) - 1);
    }

    char default_album_title[128] = "";
    cJSON *j_atitle = cJSON_GetObjectItemCaseSensitive(root, "title");
    if (j_atitle && cJSON_IsString(j_atitle)) {
        strncpy(default_album_title, j_atitle->valuestring, sizeof(default_album_title) - 1);
    } else if (album_title && album_title[0]) {
        strncpy(default_album_title, album_title, sizeof(default_album_title) - 1);
    }

    cJSON *j_tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    int parsed_count = 0;
    if (j_tracks && cJSON_IsArray(j_tracks)) {
        int n = cJSON_GetArraySize(j_tracks);
        for (int i = 0; i < n && parsed_count < max_tracks; i++) {
            cJSON *item = cJSON_GetArrayItem(j_tracks, i);
            if (!item) continue;
            TrackMetadata *t = &out_tracks[parsed_count];
            memset(t, 0, sizeof(TrackMetadata));

            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "trackId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (jid && cJSON_IsString(jid)) strncpy(t->id, jid->valuestring, sizeof(t->id) - 1);

            cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(item, "title");
            if (jtitle && cJSON_IsString(jtitle)) strncpy(t->title, jtitle->valuestring, sizeof(t->title) - 1);

            cJSON *jartists = cJSON_GetObjectItemCaseSensitive(item, "artists");
            if (jartists && cJSON_IsArray(jartists) && cJSON_GetArraySize(jartists) > 0) {
                cJSON *fa = cJSON_GetArrayItem(jartists, 0);
                cJSON *faname = cJSON_GetObjectItemCaseSensitive(fa, "name");
                if (faname && cJSON_IsString(faname)) strncpy(t->artist, faname->valuestring, sizeof(t->artist) - 1);
            }

            strncpy(t->album, default_album_title, sizeof(t->album) - 1);

            cJSON *jdur = cJSON_GetObjectItemCaseSensitive(item, "duration");
            if (jdur && cJSON_IsNumber(jdur)) {
                t->duration_sec = (uint32_t)(jdur->valuedouble / 1000.0);
                if (t->duration_sec == 0) t->duration_sec = (uint32_t)(jdur->valueint / 1000);
            } else {
                t->duration_sec = 210;
            }

            cJSON *jtart = cJSON_GetObjectItemCaseSensitive(item, "artwork");
            if (jtart && cJSON_IsString(jtart) && jtart->valuestring[0]) {
                strncpy(t->cover_url, jtart->valuestring, sizeof(t->cover_url) - 1);
            } else if (album_art[0]) {
                strncpy(t->cover_url, album_art, sizeof(t->cover_url) - 1);
            }

            t->sample_rate = 44100;
            t->bit_depth = 16;
            t->is_hi_res = false;

            parsed_count++;
        }
    }

    cJSON_Delete(root);
    free(resp_buf);

    stage6_log("[ALBUM RESULTADO] Release ID: %s | Titulo: \"%s\" | Pistas ordenadas: %d",
               release_id, default_album_title, parsed_count);

    return parsed_count;
}

/* ── Fetch Artist Top Tracks by Artist ID ── */
int http_fetch_artist_tracks_sync(const char *artist_id, const char *artist_name,
                                  TrackMetadata *out_tracks, int max_tracks,
                                  int *out_http_code, int *out_errno)
{
    if (out_http_code) *out_http_code = 0;
    if (out_errno) *out_errno = 0;
    if (!artist_id || artist_id[0] == '\0') return -1;

    stage6_log("[ARTIST INICIO] Artist ID: '%s', Nombre: '%s'", artist_id, artist_name ? artist_name : "");

    /* 1. DNS Resolution */
    struct hostent *he = gethostbyname(API_HOST);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        if (out_errno) *out_errno = errno;
        stage6_log("[ERROR] DNS fallo para %s (errno: %d)", API_HOST, errno);
        return -1;
    }

    struct in_addr api_ip;
    memcpy(&api_ip, he->h_addr_list[0], sizeof(struct in_addr));

    /* 2. Create Socket */
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        if (out_errno) *out_errno = errno;
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = g_http_timeout_ms / 1000;
    tv.tv_usec = (g_http_timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* 3. Connect */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(API_PORT);
    addr.sin_addr = api_ip;

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (out_errno) *out_errno = errno;
        close(sock);
        return -1;
    }

    /* 4. Send GET /artists/<id> */
    char req[512];
    snprintf(req, sizeof(req),
             "GET /artists/%s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "User-Agent: Monochrome-PS3/1.0 (PSL1GHT; CellOS)\r\n"
             "Accept: application/json\r\n"
             "Accept-Encoding: identity\r\n"
             "Connection: close\r\n\r\n",
             artist_id, API_HOST);

    if (send(sock, req, strlen(req), 0) <= 0) {
        if (out_errno) *out_errno = errno;
        close(sock);
        return -1;
    }

    /* 5. Receive HTTP Response */
    char *resp_buf = (char *)malloc(RECV_BUF_SIZE);
    if (!resp_buf) {
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

    /* 6. Parse HTTP status */
    int http_code = 0;
    if (sscanf(resp_buf, "HTTP/%*s %d", &http_code) == 1) {
        if (out_http_code) *out_http_code = http_code;
    }
    if (http_code != 200) {
        stage6_log("[ARTIST ERROR] HTTP %d para artist %s", http_code, artist_id);
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

    cJSON *root = cJSON_Parse(json_start);
    if (!root) {
        stage6_log("[ARTIST ERROR] cJSON_Parse fallo para artist %s", artist_id);
        free(resp_buf);
        return -1;
    }

    char default_artist_name[128] = "";
    cJSON *j_name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (j_name && cJSON_IsString(j_name)) {
        strncpy(default_artist_name, j_name->valuestring, sizeof(default_artist_name) - 1);
    } else if (artist_name && artist_name[0]) {
        strncpy(default_artist_name, artist_name, sizeof(default_artist_name) - 1);
    }

    cJSON *j_tracks = cJSON_GetObjectItemCaseSensitive(root, "topTracks");
    int parsed_count = 0;
    if (j_tracks && cJSON_IsArray(j_tracks)) {
        int n = cJSON_GetArraySize(j_tracks);
        for (int i = 0; i < n && parsed_count < max_tracks; i++) {
            cJSON *item = cJSON_GetArrayItem(j_tracks, i);
            if (!item) continue;
            TrackMetadata *t = &out_tracks[parsed_count];
            memset(t, 0, sizeof(TrackMetadata));

            cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "trackId");
            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (jid) {
                if (cJSON_IsString(jid)) strncpy(t->id, jid->valuestring, sizeof(t->id) - 1);
                else if (cJSON_IsNumber(jid)) snprintf(t->id, sizeof(t->id), "%.0f", jid->valuedouble);
            }

            cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(item, "title");
            if (jtitle && cJSON_IsString(jtitle)) strncpy(t->title, jtitle->valuestring, sizeof(t->title) - 1);

            strncpy(t->artist, default_artist_name, sizeof(t->artist) - 1);
            strncpy(t->album, "Top Tracks", sizeof(t->album) - 1);

            cJSON *jdur = cJSON_GetObjectItemCaseSensitive(item, "duration");
            if (jdur && cJSON_IsNumber(jdur)) {
                t->duration_sec = (uint32_t)(jdur->valuedouble / 1000.0);
                if (t->duration_sec == 0) t->duration_sec = (uint32_t)(jdur->valueint / 1000);
            } else {
                t->duration_sec = 210;
            }

            cJSON *jtart = cJSON_GetObjectItemCaseSensitive(item, "artwork");
            if (jtart && cJSON_IsString(jtart) && jtart->valuestring[0]) {
                strncpy(t->cover_url, jtart->valuestring, sizeof(t->cover_url) - 1);
            }

            t->sample_rate = 44100;
            t->bit_depth = 16;
            t->is_hi_res = false;
            parsed_count++;
        }
    }

    cJSON_Delete(root);
    free(resp_buf);

    stage6_log("[ARTIST RESULTADO] Artist ID: %s | Nombre: \"%s\" | Top tracks: %d",
               artist_id, default_artist_name, parsed_count);

    return parsed_count;
}

static void search_worker_func(void *arg)
{
    (void)arg;
    int code = 0, err = 0;
    int count = 0;

    if (s_async_req_type == ASYNC_REQ_ALBUM) {
        count = http_fetch_album_tracks_sync(s_async_release_id, s_async_album_title, s_async_results, MAX_SEARCH_RESULTS, &code, &err);
    } else if (s_async_req_type == ASYNC_REQ_ARTIST) {
        count = http_fetch_artist_tracks_sync(s_async_artist_id, s_async_artist_name, s_async_results, MAX_SEARCH_RESULTS, &code, &err);
    } else {
        count = http_search_sync(s_async_query, s_async_results, MAX_SEARCH_RESULTS, &code, &err);
    }

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

    s_async_req_type = ASYNC_REQ_SEARCH;
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

void http_fetch_album_async_start(const char *release_id, const char *album_title)
{
    if (s_search_thread_active) {
        return; /* Already busy */
    }

    s_async_req_type = ASYNC_REQ_ALBUM;
    strncpy(s_async_release_id, release_id ? release_id : "", sizeof(s_async_release_id) - 1);
    s_async_release_id[sizeof(s_async_release_id) - 1] = '\0';
    strncpy(s_async_album_title, album_title ? album_title : "", sizeof(s_async_album_title) - 1);
    s_async_album_title[sizeof(s_async_album_title) - 1] = '\0';

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

void http_fetch_artist_async_start(const char *artist_id, const char *artist_name)
{
    if (s_search_thread_active) {
        return; /* Already busy */
    }

    s_async_req_type = ASYNC_REQ_ARTIST;
    strncpy(s_async_artist_id, artist_id ? artist_id : "", sizeof(s_async_artist_id) - 1);
    s_async_artist_id[sizeof(s_async_artist_id) - 1] = '\0';
    strncpy(s_async_artist_name, artist_name ? artist_name : "", sizeof(s_async_artist_name) - 1);
    s_async_artist_name[sizeof(s_async_artist_name) - 1] = '\0';

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

int http_search_get_albums(AlbumMetadata *out_albums, int max_albums)
{
    int n = (s_async_album_count < max_albums) ? s_async_album_count : max_albums;
    if (out_albums && n > 0) memcpy(out_albums, s_async_albums, n * sizeof(AlbumMetadata));
    return n;
}

int http_search_get_artists(ArtistMetadata *out_artists, int max_artists)
{
    int n = (s_async_artist_count < max_artists) ? s_async_artist_count : max_artists;
    if (out_artists && n > 0) memcpy(out_artists, s_async_artists, n * sizeof(ArtistMetadata));
    return n;
}

int http_search_get_playlists(PlaylistMetadata *out_playlists, int max_playlists)
{
    int n = (s_async_playlist_count < max_playlists) ? s_async_playlist_count : max_playlists;
    if (out_playlists && n > 0) memcpy(out_playlists, s_async_playlists, n * sizeof(PlaylistMetadata));
    return n;
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

