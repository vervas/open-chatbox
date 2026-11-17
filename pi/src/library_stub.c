/**
 * library_stub.c — library.h without SQLite: a reader that never finds a
 * library. Linked into chatbox (and test_keyboard) instead of library.c
 * when CMake can't find sqlite3, so the kiosk still builds; music then
 * shows "No music yet" forever. CMake warns when this is used.
 */

#include "library.h"

#include <stdio.h>
#include <string.h>

struct library {
    int unused;
};

static struct library g_stub_lib;

library_t *library_open(const char *db_path, const char *art_dir)
{
    (void)art_dir;
    if (!db_path) return NULL;
    fprintf(stderr, "library: built without SQLite, no music library (%s)\n", db_path);
    return &g_stub_lib;
}

void library_close(library_t *lib)                     { (void)lib; }
bool library_poll(library_t *lib)                      { (void)lib; return false; }
library_state_t library_state(const library_t *lib)    { (void)lib; return LIBRARY_MISSING; }
int64_t library_generation(const library_t *lib)       { (void)lib; return -1; }

int library_load_artists(library_t *lib, library_artists_t *out)
{
    (void)lib;
    out->count = out->total = 0;
    return LIBRARY_ERR;
}

int library_load_albums(library_t *lib, int64_t artist_id, library_albums_t *out)
{
    (void)lib; (void)artist_id;
    out->count = out->total = 0;
    return LIBRARY_ERR;
}

int library_load_all_albums(library_t *lib, library_all_albums_t *out)
{
    (void)lib;
    out->count = out->total = 0;
    return LIBRARY_ERR;
}

int library_load_tracks(library_t *lib, int64_t album_id, library_tracks_t *out)
{
    (void)lib; (void)album_id;
    out->count = out->total = 0;
    return LIBRARY_ERR;
}

int library_load_artist_tracks(library_t *lib, int64_t artist_id,
                               library_artist_tracks_t *out)
{
    (void)lib; (void)artist_id;
    out->count = out->total = 0;
    return LIBRARY_ERR;
}

int library_track_get(library_t *lib, int64_t track_id, library_track_info_t *out)
{
    (void)lib; (void)track_id;
    memset(out, 0, sizeof(*out));
    out->duration_ms = -1;
    return LIBRARY_ERR;
}

bool library_art_path(const library_t *lib, int64_t art_id, int size,
                      char *out, size_t out_size)
{
    (void)lib; (void)art_id; (void)size;
    if (out && out_size) out[0] = '\0';
    return false;
}

static int find(const void *items, size_t stride, int count, int64_t id, int fallback)
{
    if (count <= 0) return -1;
    for (int i = 0; i < count; i++)
        if (*(const int64_t *)((const char *)items + (size_t)i * stride) == id) return i;
    if (fallback < 0) return 0;
    return fallback >= count ? count - 1 : fallback;
}

int library_artists_find(const library_artists_t *l, int64_t id, int fallback_index)
{
    return find(l->items, sizeof(l->items[0]), l->count, id, fallback_index);
}

int library_albums_find(const library_albums_t *l, int64_t id, int fallback_index)
{
    return find(l->items, sizeof(l->items[0]), l->count, id, fallback_index);
}

int library_all_albums_find(const library_all_albums_t *l, int64_t id, int fallback_index)
{
    return find(l->items, sizeof(l->items[0]), l->count, id, fallback_index);
}

int library_tracks_find(const library_tracks_t *l, int64_t id, int fallback_index)
{
    return find(l->items, sizeof(l->items[0]), l->count, id, fallback_index);
}

int library_artist_tracks_find(const library_artist_tracks_t *l, int64_t id,
                               int fallback_index)
{
    return find(l->items, sizeof(l->items[0]), l->count, id, fallback_index);
}

bool library_utf8_copy(char *dst, size_t n, const char *src)
{
    if (!dst || n == 0) return false;
    if (!src) src = "";
    size_t len = strlen(src);
    if (len < n) {
        memcpy(dst, src, len + 1);
        return false;
    }
    size_t cut = n - 1;
    while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80) cut--;
    memcpy(dst, src, cut);
    dst[cut] = '\0';
    return true;
}
