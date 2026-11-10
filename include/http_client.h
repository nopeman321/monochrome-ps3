#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "monochrome.h"

typedef enum {
    SEARCH_STATUS_IDLE = 0,
    SEARCH_STATUS_BUSY,
    SEARCH_STATUS_OK,
    SEARCH_STATUS_NO_RESULTS,
    SEARCH_STATUS_ERROR
} SearchStatus;

int  http_net_init(void);
void http_net_shutdown(void);

int  http_search_sync(const char *query, TrackMetadata *out_tracks, int max_tracks, int *out_http_code, int *out_errno);
int  http_fetch_album_tracks_sync(const char *release_id, const char *album_title, TrackMetadata *out_tracks, int max_tracks, int *out_http_code, int *out_errno);

void http_search_async_start(const char *query);
void http_fetch_album_async_start(const char *release_id, const char *album_title);
SearchStatus http_search_async_poll(TrackMetadata *out_tracks, int *out_count, int max_tracks, int *out_http_code, int *out_errno);
const char  *http_search_get_active_query(void);

int http_search_get_albums(AlbumMetadata *out_albums, int max_albums);
int http_search_get_artists(ArtistMetadata *out_artists, int max_artists);
int http_search_get_playlists(PlaylistMetadata *out_playlists, int max_playlists);

int  http_get_timeout_ms(void);
void http_set_timeout_ms(int timeout_ms);
int  http_get_max_retries(void);
void http_set_max_retries(int max_retries);

#endif /* HTTP_CLIENT_H */
