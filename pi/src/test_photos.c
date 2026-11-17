/**
 * test_photos.c — Photos: the library (photo_lib.c), the decoder
 * (photo_decode.c), the chat inbox (photo_inbox.c) and the media URI
 * check (matrix_mxc_parse). No LVGL, no network: downloads go through a
 * fake fetch.
 *
 * Run: ./test_photos   (writes under $TMPDIR, cleans up after itself)
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "cJSON.h"
#include "matrix_api.h"
#include "photo_decode.h"
#include "photo_inbox.h"
#include "photo_lib.h"
#include "test_photo_fixture.h"

static int g_tests = 0;
static int g_failures = 0;

#define EXPECT(cond, msg) do { \
    g_tests++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

#define EXPECT_INT(got, want, msg) do { \
    long long _g = (long long)(got), _w = (long long)(want); \
    g_tests++; \
    if (_g != _w) { \
        fprintf(stderr, "FAIL [%s:%d] %s: got %lld, want %lld\n", \
                __FILE__, __LINE__, msg, _g, _w); \
        g_failures++; \
    } \
} while (0)

static char g_root[512];

/* ---- Helpers ------------------------------------------------------------- */

static void mkdir_p(const char *p) { mkdir(p, 0755); }

static void write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    if (len) fwrite(data, 1, len, f);
    fclose(f);
}

static void set_mtime(const char *path, time_t t)
{
    struct timeval tv[2] = { { t, 0 }, { t, 0 } };
    utimes(path, tv);
}

static void rm_rf(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[1024];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(p);
        else unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

static int count_hidden_tmp(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strncmp(e->d_name, ".incoming-", 10) == 0) n++;
    closedir(d);
    return n;
}

static unsigned char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n + 1);
    *len = fread(b, 1, (size_t)n, f);
    fclose(f);
    return b;
}

/* Pixel (x, y) of a decoded BGR buffer as RGB. */
static void px_rgb(const photo_pixels_t *p, int x, int y, int *r, int *g, int *b)
{
    const uint8_t *q = p->px + ((size_t)y * p->w + x) * 3;
    *b = q[0]; *g = q[1]; *r = q[2];
}

static bool is_red(int r, int g, int b) { return r > 170 && g < 110 && b < 110; }

/* ---- photo_lib ----------------------------------------------------------- */

static void test_lib_scan(void)
{
    char dir[600], p[700];
    snprintf(dir, sizeof(dir), "%s/lib", g_root);

    photo_lib_t *lib = photo_lib_open(dir);
    EXPECT(lib != NULL, "open on a missing folder works");
    EXPECT_INT(photo_lib_count(lib), 0, "missing folder: empty");
    EXPECT(!photo_lib_poll(lib), "missing folder: no change");

    mkdir_p(dir);
    snprintf(p, sizeof(p), "%s/old.jpg", dir);   write_file(p, "x", 1); set_mtime(p, 1000);
    snprintf(p, sizeof(p), "%s/new.PNG", dir);   write_file(p, "x", 1); set_mtime(p, 3000);
    snprintf(p, sizeof(p), "%s/mid.jpeg", dir);  write_file(p, "x", 1); set_mtime(p, 2000);
    snprintf(p, sizeof(p), "%s/b-tie.jpg", dir); write_file(p, "x", 1); set_mtime(p, 2000);
    snprintf(p, sizeof(p), "%s/.hidden.jpg", dir); write_file(p, "x", 1);
    snprintf(p, sizeof(p), "%s/.incoming-1.tmp", dir); write_file(p, "x", 1);
    snprintf(p, sizeof(p), "%s/notes.txt", dir); write_file(p, "x", 1);
    snprintf(p, sizeof(p), "%s/empty.jpg", dir); write_file(p, "", 0);
    snprintf(p, sizeof(p), "%s/folder.jpg", dir); mkdir_p(p);

    EXPECT(photo_lib_poll(lib), "the folder appeared: a change");
    EXPECT_INT(photo_lib_count(lib), 4, "four photos listed");
    const char *want[] = { "new.PNG", "mid.jpeg", "b-tie.jpg", "old.jpg" };
    for (int i = 0; i < 4 && i < photo_lib_count(lib); i++)
        EXPECT(strcmp(photo_lib_get(lib, i)->name, want[i]) == 0, "newest first, ties by name descending");
    EXPECT(photo_lib_get(lib, 4) == NULL, "out of range: NULL");
    EXPECT(photo_lib_get(lib, -1) == NULL, "negative: NULL");
    EXPECT_INT(photo_lib_find(lib, "old.jpg"), 3, "find");
    EXPECT_INT(photo_lib_find(lib, "nope.jpg"), -1, "find: missing");
    char path[1024];
    EXPECT(photo_lib_path(lib, 0, path, sizeof(path)), "path ok");
    snprintf(p, sizeof(p), "%s/new.PNG", dir);
    EXPECT(strcmp(path, p) == 0, "path is dir/name");

    unsigned gen = photo_lib_generation(lib);
    EXPECT(!photo_lib_rescan(lib), "rescan with nothing new: no change");
    EXPECT_INT(photo_lib_generation(lib), gen, "generation unchanged");

    /* A file grows in place (a copy finishing): seen by the periodic full rescan. */
    snprintf(p, sizeof(p), "%s/old.jpg", dir);
    write_file(p, "xyz", 3);
    set_mtime(p, 1000);
    bool changed = false;
    for (int i = 0; i < PHOTO_LIB_FULL_EVERY && !changed; i++) changed = photo_lib_poll(lib);
    EXPECT(changed, "a size change is found within PHOTO_LIB_FULL_EVERY polls");
    EXPECT(photo_lib_generation(lib) != gen, "generation bumped");

    /* Delete. */
    int i = photo_lib_find(lib, "mid.jpeg");
    EXPECT_INT(photo_lib_delete(lib, i), 0, "delete ok");
    snprintf(p, sizeof(p), "%s/mid.jpeg", dir);
    EXPECT(access(p, F_OK) != 0, "the file is gone from disk");
    EXPECT_INT(photo_lib_count(lib), 3, "three left");
    EXPECT_INT(photo_lib_find(lib, "mid.jpeg"), -1, "not listed");
    EXPECT_INT(photo_lib_delete(lib, 7), -1, "delete out of range fails");

    /* Someone removed a file behind our back: delete fails, the list heals. */
    snprintf(p, sizeof(p), "%s/new.PNG", dir);
    unlink(p);
    EXPECT_INT(photo_lib_delete(lib, 0), -1, "delete of a vanished file fails");
    EXPECT_INT(photo_lib_count(lib), 2, "...and it's off the list");

    /* Names. */
    EXPECT(photo_lib_name_ok("a.jpg") && photo_lib_name_ok("B.JPEG") && photo_lib_name_ok("c.Png"), "name_ok: photos");
    EXPECT(!photo_lib_name_ok(".a.jpg") && !photo_lib_name_ok("a.gif") && !photo_lib_name_ok("jpg") &&
           !photo_lib_name_ok(".jpg") && !photo_lib_name_ok("") && !photo_lib_name_ok(NULL), "name_ok: others");
    photo_lib_close(lib);
}

static void test_lib_trailing_slash(void)
{
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/lib//", g_root);
    photo_lib_t *lib = photo_lib_open(dir);
    char want[600];
    snprintf(want, sizeof(want), "%s/lib", g_root);
    EXPECT(strcmp(photo_lib_dir(lib), want) == 0, "trailing slashes dropped");
    photo_lib_close(lib);
}

/* ---- photo_decode -------------------------------------------------------- */

static void test_decode_basics(void)
{
    static const uint8_t jpg[] = { 0xFF, 0xD8, 0xFF, 0xE0 };
    static const uint8_t png[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    static const uint8_t gif[] = { 'G', 'I', 'F', '8', '9', 'a' };
    EXPECT_INT(photo_format_sniff(jpg, sizeof(jpg)), PHOTO_FMT_JPEG, "sniff jpeg");
    EXPECT_INT(photo_format_sniff(png, sizeof(png)), PHOTO_FMT_PNG, "sniff png");
    EXPECT_INT(photo_format_sniff(gif, sizeof(gif)), PHOTO_FMT_NONE, "sniff gif");
    EXPECT_INT(photo_format_sniff(png, 4), PHOTO_FMT_NONE, "sniff short png");
    EXPECT_INT(photo_format_sniff(NULL, 0), PHOTO_FMT_NONE, "sniff NULL");

    int w, h;
    photo_fit_size(3000, 1000, 1168, 600, PHOTO_FIT_CONTAIN, &w, &h);
    EXPECT(w == 1168 && h == 389, "contain: a 3:1 panorama is letterboxed");
    photo_fit_size(1000, 2000, 1168, 600, PHOTO_FIT_CONTAIN, &w, &h);
    EXPECT(w == 300 && h == 600, "contain: a portrait is pillarboxed");
    photo_fit_size(400, 300, 1168, 600, PHOTO_FIT_CONTAIN, &w, &h);
    EXPECT(w == 800 && h == 600, "contain: small photos grow to fit");
    photo_fit_size(3000, 1000, 256, 192, PHOTO_FIT_COVER, &w, &h);
    EXPECT(w == 256 && h == 192, "cover: the box");

    /* EXIF */
    static const uint8_t le[] = { 'E','x','i','f',0,0, 'I','I',42,0, 8,0,0,0, 1,0,
                                  0x12,0x01, 3,0, 1,0,0,0, 6,0,0,0, 0,0,0,0 };
    EXPECT_INT(photo_exif_orientation(le, sizeof(le)), 6, "exif little-endian 6");
    EXPECT_INT(photo_exif_orientation(le, 20), 1, "exif truncated: 1");
    uint8_t bad[sizeof(le)];
    memcpy(bad, le, sizeof(le));
    bad[24] = 9;
    EXPECT_INT(photo_exif_orientation(bad, sizeof(bad)), 1, "exif out of range: 1");
    bad[24] = 6; bad[14] = 0xFF; bad[15] = 0xFF; bad[16] = 0x13;   /* 65535 entries, none 0x0112 */
    EXPECT_INT(photo_exif_orientation(bad, sizeof(bad)), 1, "exif huge count: 1, no overrun");
    EXPECT_INT(photo_exif_orientation((const uint8_t *)"JFIF", 4), 1, "not exif: 1");

    /* Bad files. */
    char p[700];
    photo_pixels_t px;
    snprintf(p, sizeof(p), "%s/missing.jpg", g_root);
    EXPECT(!photo_decode(p, 100, 100, PHOTO_FIT_COVER, &px), "missing file: false");
    snprintf(p, sizeof(p), "%s/garbage.jpg", g_root);
    write_file(p, "hello, not a photo", 18);
    EXPECT(!photo_decode(p, 100, 100, PHOTO_FIT_COVER, &px), "garbage: false");
    write_file(p, "\xFF\xD8\xFF\xE0\x00\x10JFIF", 10);
    EXPECT(!photo_decode(p, 100, 100, PHOTO_FIT_COVER, &px), "truncated jpeg header: false");
    write_file(p, "\x89PNG\r\n\x1a\n\x00\x00", 10);
    EXPECT(!photo_decode(p, 100, 100, PHOTO_FIT_COVER, &px), "truncated png: false");
    EXPECT(px.px == NULL, "nothing allocated on failure");
}

static void test_decode_jpeg(void)
{
    char p[700];
    photo_pixels_t px;
    int r, g, b;
    snprintf(p, sizeof(p), "%s/land.jpg", g_root);
    if (!photo_fixture_jpeg(p, 1600, 1200, 1, 1)) {
        printf("  (no libjpeg: JPEG decode tests skipped)\n");
        return;
    }
    EXPECT(photo_decode_supported(PHOTO_FMT_JPEG), "jpeg supported");
    EXPECT(photo_decode(p, 256, 192, PHOTO_FIT_COVER, &px), "decode jpeg cover");
    EXPECT(px.w == 256 && px.h == 192, "cover: exactly the box");
    px_rgb(&px, 2, 100, &r, &g, &b);
    EXPECT(is_red(r, g, b), "the left stripe is on the left");
    px_rgb(&px, 200, 190, &r, &g, &b);
    EXPECT(g > r && g > b, "the bottom is hill green");
    photo_pixels_free(&px);
    EXPECT(px.px == NULL, "free clears");

    snprintf(p, sizeof(p), "%s/pano.jpg", g_root);
    photo_fixture_jpeg(p, 1500, 500, 2, 1);
    EXPECT(photo_decode(p, 1168, 600, PHOTO_FIT_CONTAIN, &px), "decode panorama contain");
    EXPECT(px.w == 1168 && px.h == 389, "panorama: whole width, shape kept");
    photo_pixels_free(&px);

    /* Portrait stored sideways (EXIF 6, as phones do). */
    snprintf(p, sizeof(p), "%s/portrait.jpg", g_root);
    photo_fixture_jpeg(p, 300, 400, 3, 6);
    EXPECT(photo_decode(p, 600, 600, PHOTO_FIT_CONTAIN, &px), "decode exif-6 portrait");
    EXPECT(px.w == 450 && px.h == 600, "exif 6: shown upright (taller than wide)");
    px_rgb(&px, 3, 300, &r, &g, &b);
    EXPECT(is_red(r, g, b), "exif 6: the stripe is back on the left");
    px_rgb(&px, 440, 300, &r, &g, &b);
    EXPECT(!is_red(r, g, b), "exif 6: not on the right");
    photo_pixels_free(&px);

    for (int o = 2; o <= 8; o++) {
        snprintf(p, sizeof(p), "%s/orient%d.jpg", g_root, o);
        photo_fixture_jpeg(p, 160, 120, 4, o);
        bool ok = photo_decode(p, 160, 120, PHOTO_FIT_COVER, &px);
        EXPECT(ok, "decode every orientation");
        if (!ok) continue;
        px_rgb(&px, 2, 60, &r, &g, &b);
        bool left = is_red(r, g, b);
        px_rgb(&px, 157, 60, &r, &g, &b);
        EXPECT(left && !is_red(r, g, b), "every orientation: stripe on the left");
        px_rgb(&px, 80, 118, &r, &g, &b);
        EXPECT(g > b, "every orientation: hills at the bottom");
        photo_pixels_free(&px);
    }

    /* Small photo in a big box: grows (nearest pixel), no crash. */
    snprintf(p, sizeof(p), "%s/tiny.jpg", g_root);
    photo_fixture_jpeg(p, 8, 6, 5, 1);
    EXPECT(photo_decode(p, 256, 192, PHOTO_FIT_COVER, &px) && px.w == 256, "tiny jpeg scales up");
    photo_pixels_free(&px);

    /* Truncated after the header: still decodes (grey bottom), no crash. */
    size_t len;
    snprintf(p, sizeof(p), "%s/land.jpg", g_root);
    unsigned char *all = read_all(p, &len);
    snprintf(p, sizeof(p), "%s/half.jpg", g_root);
    write_file(p, (const char *)all, len / 2);
    free(all);
    EXPECT(photo_decode(p, 256, 192, PHOTO_FIT_COVER, &px), "half a jpeg decodes");
    photo_pixels_free(&px);
}

static void test_decode_png(void)
{
    char p[700];
    photo_pixels_t px;
    int r, g, b;
    snprintf(p, sizeof(p), "%s/a.png", g_root);
    if (!photo_fixture_png(p, 400, 300, 6, true)) {
        printf("  (no libpng: PNG decode tests skipped)\n");
        return;
    }
    EXPECT(photo_decode(p, 200, 150, PHOTO_FIT_COVER, &px), "decode png");
    EXPECT(px.w == 200 && px.h == 150, "png: the box");
    px_rgb(&px, 100, 5, &r, &g, &b);
    EXPECT(abs(r - 0xD9) < 4 && abs(g - 0xE3) < 4 && abs(b - 0xED) < 4,
           "transparent pixels land on the page colour");
    px_rgb(&px, 2, 140, &r, &g, &b);
    EXPECT(is_red(r, g, b), "png: the stripe on the left");
    photo_pixels_free(&px);
}

/* ---- matrix_mxc_parse ---------------------------------------------------- */

static void test_mxc(void)
{
    char s[64], m[64];
    EXPECT(matrix_mxc_parse("mxc://example.org:8448/AbC_12-x", s, sizeof(s), m, sizeof(m)) &&
           !strcmp(s, "example.org:8448") && !strcmp(m, "AbC_12-x"), "mxc ok");
    EXPECT(!matrix_mxc_parse("mxc://example.org/../../etc", s, sizeof(s), m, sizeof(m)), "mxc: path in id");
    EXPECT(!matrix_mxc_parse("mxc://example.org/a?b=c", s, sizeof(s), m, sizeof(m)), "mxc: query");
    EXPECT(!matrix_mxc_parse("mxc://ex/ample.org/a", s, sizeof(s), m, sizeof(m)), "mxc: slash in id");
    EXPECT(!matrix_mxc_parse("mxc://a b/c", s, sizeof(s), m, sizeof(m)), "mxc: space");
    EXPECT(!matrix_mxc_parse("mxc:///abc", s, sizeof(s), m, sizeof(m)), "mxc: no server");
    EXPECT(!matrix_mxc_parse("mxc://server/", s, sizeof(s), m, sizeof(m)), "mxc: no id");
    EXPECT(!matrix_mxc_parse("https://server/abc", s, sizeof(s), m, sizeof(m)), "mxc: not mxc");
    EXPECT(!matrix_mxc_parse("mxc://server/abcdef", s, 4, m, sizeof(m)), "mxc: server too long for buffer");
}

/* ---- photo_inbox --------------------------------------------------------- */

static cJSON *image_event(const char *eid, const char *url, const char *mime, double size, double ts)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "type", "m.room.message");
    if (eid) cJSON_AddStringToObject(e, "event_id", eid);
    cJSON_AddStringToObject(e, "sender", "@mom:localhost");
    if (ts > 0) cJSON_AddNumberToObject(e, "origin_server_ts", ts);
    cJSON *c = cJSON_AddObjectToObject(e, "content");
    cJSON_AddStringToObject(c, "msgtype", "m.image");
    cJSON_AddStringToObject(c, "body", "../../IMG_0001.jpg");
    if (url) cJSON_AddStringToObject(c, "url", url);
    if (mime || size >= 0) {
        cJSON *i = cJSON_AddObjectToObject(c, "info");
        if (mime) cJSON_AddStringToObject(i, "mimetype", mime);
        if (size >= 0) cJSON_AddNumberToObject(i, "size", size);
        cJSON_AddNumberToObject(i, "w", 1600);
        cJSON_AddNumberToObject(i, "h", 1200);
    }
    return e;
}

static void test_inbox_parse(void)
{
    photo_event_t ev;
    cJSON *e = image_event("$a:hs", "mxc://hs/abc", "image/jpeg", 1234, 1700000000123.0);
    EXPECT(photo_inbox_parse(e, &ev), "parse m.image");
    EXPECT(!strcmp(ev.event_id, "$a:hs") && !strcmp(ev.mxc, "mxc://hs/abc"), "parse: id and url");
    EXPECT(!strcmp(ev.mimetype, "image/jpeg") && ev.size == 1234 && ev.w == 1600 && ev.h == 1200, "parse: info");
    EXPECT(ev.ts == 1700000000123LL, "parse: ts");
    cJSON_Delete(e);

    e = image_event("$a:hs", "mxc://hs/abc", NULL, -1, 0);
    EXPECT(photo_inbox_parse(e, &ev) && ev.size == -1 && ev.mimetype[0] == 0 && ev.ts == 0, "parse: no info");
    cJSON_Delete(e);

    e = image_event(NULL, "mxc://hs/abc", NULL, -1, 0);
    EXPECT(!photo_inbox_parse(e, &ev), "no event_id: not a photo");
    cJSON_Delete(e);
    e = image_event("$a", NULL, NULL, -1, 0);
    EXPECT(!photo_inbox_parse(e, &ev), "no url (e.g. encrypted, content.file): not a photo");
    cJSON_Delete(e);
    e = image_event("$a", "https://evil/x.jpg", NULL, -1, 0);
    EXPECT(!photo_inbox_parse(e, &ev), "http url: not a photo");
    cJSON_Delete(e);
    e = image_event("$a", "mxc://hs/abc", NULL, -1, 0);
    cJSON_ReplaceItemInObject(cJSON_GetObjectItem(e, "content"), "msgtype", cJSON_CreateString("m.text"));
    EXPECT(!photo_inbox_parse(e, &ev), "m.text: not a photo");
    cJSON_Delete(e);
    e = image_event("$a", "mxc://hs/abc", NULL, -1, 0);
    cJSON_ReplaceItemInObject(e, "type", cJSON_CreateString("m.sticker"));
    EXPECT(!photo_inbox_parse(e, &ev), "m.sticker: not a photo");
    cJSON_Delete(e);
    e = image_event("$a", "mxc://hs/abc", NULL, -1, 0);
    cJSON_ReplaceItemInObject(cJSON_GetObjectItem(e, "content"), "url", cJSON_CreateNumber(5));
    EXPECT(!photo_inbox_parse(e, &ev), "url not a string: not a photo");
    cJSON_Delete(e);
    char longid[400];
    memset(longid, 'x', sizeof(longid) - 1);
    longid[0] = '$';
    longid[sizeof(longid) - 1] = 0;
    e = image_event(longid, "mxc://hs/abc", NULL, -1, 0);
    EXPECT(!photo_inbox_parse(e, &ev), "absurdly long id: refused, not truncated");
    cJSON_Delete(e);
    EXPECT(!photo_inbox_parse(NULL, &ev), "NULL event");
    cJSON *arr = cJSON_CreateArray();
    EXPECT(!photo_inbox_parse(arr, &ev), "not an object");
    cJSON_Delete(arr);
}

static void test_inbox_filename(void)
{
    char a[PHOTO_INBOX_NAME_MAX], b[PHOTO_INBOX_NAME_MAX];
    photo_inbox_filename("$abc:hs", 1700000000123LL, "jpg", a);
    EXPECT(strncmp(a, "20231114-221320-", 16) == 0, "name starts with the UTC send time");
    EXPECT(strlen(a) == 16 + 16 + 4, "name length");
    EXPECT(!strcmp(a + strlen(a) - 4, ".jpg"), ".jpg");
    photo_inbox_filename("$abc:hs", 1700000000123LL, "jpg", b);
    EXPECT(!strcmp(a, b), "deterministic");
    photo_inbox_filename("$abd:hs", 1700000000123LL, "jpg", b);
    EXPECT(strcmp(a, b) != 0, "another event, another name");
    photo_inbox_filename("../../etc/passwd/$x", 0, "png", b);
    bool safe = true;
    for (const char *c = b; *c; c++)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || *c == '-' || *c == '.')) safe = false;
    EXPECT(safe && !strchr(b, '/') && strstr(b, "..") == NULL, "only [0-9a-z-.]: no path from the event");
    EXPECT(!strcmp(b + strlen(b) - 4, ".png") && !strncmp(b, "19700101-000000-", 16), "no ts: the epoch; .png");
    photo_inbox_filename("$x", 1, "gif", b);
    EXPECT(!strcmp(b + strlen(b) - 4, ".jpg"), "unknown ext → .jpg");
    EXPECT(photo_inbox_hash("$a") != photo_inbox_hash("$b"), "hash differs");
}

/* Fake fetch: answers from a script. */
static struct {
    int    calls;
    bool   legacy_seen;
    long   http;
    int    status;
    const unsigned char *data;
    size_t len;
    bool   defer;                 /* keep the callback for later */
    photo_inbox_media_cb_t cb;
    void  *ud;
} g_fake;

static void fake_fetch(const char *mxc, bool legacy, size_t max, photo_inbox_media_cb_t cb, void *ud)
{
    (void)mxc; (void)max;
    g_fake.calls++;
    if (legacy) g_fake.legacy_seen = true;
    if (g_fake.defer) { g_fake.cb = cb; g_fake.ud = ud; return; }
    cb(g_fake.status == MATRIX_OK ? g_fake.data : NULL, g_fake.status == MATRIX_OK ? g_fake.len : 0,
       g_fake.http, g_fake.status, ud);
}

static struct { int calls; photo_status_t last; char id[256]; } g_cb;

static void inbox_cb(const char *eid, photo_status_t st, void *ud)
{
    (void)ud;
    g_cb.calls++;
    g_cb.last = st;
    snprintf(g_cb.id, sizeof(g_cb.id), "%s", eid);
}

static void test_inbox_save(void)
{
    char dir[600], path[1024];
    snprintf(dir, sizeof(dir), "%s/inbox-save", g_root);
    photo_inbox_init(dir, NULL, NULL);
    struct stat st;
    EXPECT(stat(dir, &st) == 0 && S_ISDIR(st.st_mode), "init creates the folder");

    static const unsigned char gif[] = "GIF89a....";
    EXPECT_INT(photo_inbox_save("$g", 1, gif, sizeof(gif), path, sizeof(path)), PHOTO_ST_REFUSED, "gif refused");
    EXPECT_INT(photo_inbox_save("$e", 1, gif, 0, path, sizeof(path)), PHOTO_ST_REFUSED, "empty refused");
    static const unsigned char jpg[] = { 0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3 };
    EXPECT_INT(photo_inbox_save("$big", 1, jpg, PHOTO_INBOX_MAX_BYTES + 1, path, sizeof(path)),
               PHOTO_ST_REFUSED, "too big refused (before reading the bytes)");

    EXPECT_INT(photo_inbox_save("$j", 1700000000000LL, jpg, sizeof(jpg), path, sizeof(path)), PHOTO_ST_READY, "jpeg saved");
    EXPECT(strstr(path, ".jpg") != NULL, "jpeg → .jpg");
    EXPECT(stat(path, &st) == 0 && st.st_size == (off_t)sizeof(jpg), "all bytes written");
    EXPECT(st.st_mtime == 1700000000, "mtime = the send time");
    EXPECT_INT(count_hidden_tmp(dir), 0, "no temp file left");
    static const unsigned char png[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0 };
    EXPECT_INT(photo_inbox_save("$p", 1700000001000LL, png, sizeof(png), path, sizeof(path)), PHOTO_ST_READY, "png saved");
    EXPECT(strstr(path, ".png") != NULL, "png → .png (the bytes decide, not the event)");

    photo_lib_t *lib = photo_lib_open(dir);
    EXPECT_INT(photo_lib_count(lib), 2, "the library lists both");
    EXPECT(strstr(photo_lib_get(lib, 0)->name, ".png") != NULL, "newest first by send time");
    photo_lib_close(lib);

    /* Unwritable folder. */
    photo_inbox_init("/proc/definitely/not/here", NULL, NULL);
    EXPECT_INT(photo_inbox_save("$x", 1, jpg, sizeof(jpg), path, sizeof(path)), PHOTO_ST_FAILED, "unwritable: FAILED");
    photo_inbox_deinit();
}

static void test_inbox_flow(void)
{
    char dir[600], path[1024], jp[700];
    snprintf(dir, sizeof(dir), "%s/inbox-flow", g_root);
    snprintf(jp, sizeof(jp), "%s/src.jpg", g_root);
    size_t jlen = 0;
    unsigned char *jbytes = NULL;
    if (photo_fixture_jpeg(jp, 64, 48, 7, 1)) jbytes = read_all(jp, &jlen);
    if (!jbytes) {   /* no libjpeg: any bytes that sniff as JPEG */
        static unsigned char fake[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 0 };
        jbytes = malloc(sizeof(fake));
        memcpy(jbytes, fake, sizeof(fake));
        jlen = sizeof(fake);
    }

    memset(&g_cb, 0, sizeof(g_cb));
    memset(&g_fake, 0, sizeof(g_fake));
    photo_inbox_init(dir, inbox_cb, NULL);
    photo_inbox_test_set_fetch(fake_fetch);

    /* Success. */
    g_fake.status = MATRIX_OK; g_fake.http = 200; g_fake.data = jbytes; g_fake.len = jlen;
    cJSON *e = image_event("$one:hs", "mxc://hs/one", "image/jpeg", (double)jlen, 1700000000000.0);
    EXPECT_INT(photo_inbox_status("$one:hs", 1700000000000LL, NULL, 0), PHOTO_ST_UNKNOWN, "before: unknown");
    photo_inbox_offer(e);
    EXPECT_INT(photo_inbox_status("$one:hs", 1700000000000LL, NULL, 0), PHOTO_ST_LOADING, "offered: loading");
    photo_inbox_offer(e);
    EXPECT_INT(photo_inbox_test_pending(), 1, "offered twice: queued once");
    photo_inbox_tick(100);
    EXPECT_INT(g_fake.calls, 1, "one download");
    EXPECT_INT(g_cb.calls, 1, "told once");
    EXPECT_INT(g_cb.last, PHOTO_ST_READY, "ready");
    EXPECT_INT(photo_inbox_status("$one:hs", 1700000000000LL, path, sizeof(path)), PHOTO_ST_READY, "status ready");
    EXPECT(access(path, R_OK) == 0, "the file is there");
    EXPECT(photo_inbox_seen("$one:hs"), "seen");
    photo_inbox_offer(e);
    photo_inbox_tick(200);
    EXPECT_INT(g_fake.calls, 1, "offered again after saving: no download");

    /* Deleted in the Photos app: not fetched again, now or after a reboot. */
    unlink(path);
    EXPECT_INT(photo_inbox_status("$one:hs", 1700000000000LL, NULL, 0), PHOTO_ST_GONE, "deleted: gone");
    photo_inbox_init(dir, inbox_cb, NULL);                  /* "reboot" */
    photo_inbox_test_set_fetch(fake_fetch);
    photo_inbox_offer(e);
    photo_inbox_tick(300);
    EXPECT_INT(g_fake.calls, 1, "after a reboot the seen list still knows it");
    EXPECT_INT(photo_inbox_status("$one:hs", 1700000000000LL, NULL, 0), PHOTO_ST_GONE, "gone after reboot");
    cJSON_Delete(e);

    /* Saved before but the seen list was lost: the file is found, no download. */
    e = image_event("$two:hs", "mxc://hs/two", NULL, -1, 1700000100000.0);
    photo_inbox_save("$two:hs", 1700000100000LL, jbytes, jlen, NULL, 0);
    photo_inbox_offer(e);
    photo_inbox_tick(400);
    EXPECT_INT(g_fake.calls, 1, "already on disk: no download");
    EXPECT(photo_inbox_seen("$two:hs"), "...and now seen");
    cJSON_Delete(e);

    /* Not JPEG/PNG by its mimetype: refused without a download. */
    int calls = g_fake.calls;
    e = image_event("$gif:hs", "mxc://hs/gif", "image/gif", 100, 1700000200000.0);
    photo_inbox_offer(e);
    photo_inbox_tick(500);
    EXPECT_INT(g_fake.calls, calls, "gif: no download");
    EXPECT_INT(g_cb.last, PHOTO_ST_REFUSED, "gif refused");
    EXPECT_INT(photo_inbox_status("$gif:hs", 1700000200000LL, NULL, 0), PHOTO_ST_REFUSED, "gif status refused");
    cJSON_Delete(e);

    /* Too big by its info.size: refused. */
    e = image_event("$huge:hs", "mxc://hs/huge", "image/jpeg", 100e6, 1700000300000.0);
    photo_inbox_offer(e);
    EXPECT_INT(photo_inbox_status("$huge:hs", 1700000300000LL, NULL, 0), PHOTO_ST_REFUSED, "too big refused");
    cJSON_Delete(e);

    /* The bytes aren't a photo (the mimetype lied). */
    static const unsigned char html[] = "<html>nope</html>";
    g_fake.data = html; g_fake.len = sizeof(html);
    e = image_event("$lie:hs", "mxc://hs/lie", "image/jpeg", 10, 1700000400000.0);
    photo_inbox_offer(e);
    photo_inbox_tick(600);
    EXPECT_INT(photo_inbox_status("$lie:hs", 1700000400000LL, NULL, 0), PHOTO_ST_REFUSED, "not a photo: refused");
    EXPECT(photo_inbox_seen("$lie:hs"), "refused ones are seen (never retried)");
    cJSON_Delete(e);

    /* No authenticated media (404): the legacy endpoint, then success. */
    g_fake.data = jbytes; g_fake.len = jlen;
    g_fake.status = MATRIX_ERR_HTTP; g_fake.http = 404;
    e = image_event("$old:hs", "mxc://hs/old", NULL, -1, 1700000500000.0);
    photo_inbox_offer(e);
    photo_inbox_tick(700);
    EXPECT(!g_fake.legacy_seen, "first try: the new endpoint");
    g_fake.status = MATRIX_OK; g_fake.http = 200;
    photo_inbox_tick(701);
    EXPECT(g_fake.legacy_seen, "then the legacy endpoint");
    EXPECT_INT(photo_inbox_status("$old:hs", 1700000500000LL, NULL, 0), PHOTO_ST_READY, "saved via legacy");
    cJSON_Delete(e);

    /* 403 / 401: the token or the server's set-up, which a grown-up can fix.
     * Retried like a network failure, never written to the seen list. */
    g_fake.status = MATRIX_ERR_HTTP; g_fake.http = 403;
    e = image_event("$403:hs", "mxc://hs/f", NULL, -1, 1700000600000.0);
    photo_inbox_offer(e);
    photo_inbox_tick(800);
    EXPECT_INT(photo_inbox_status("$403:hs", 1700000600000LL, NULL, 0), PHOTO_ST_LOADING,
               "403: still loading (retried)");
    EXPECT(!photo_inbox_seen("$403:hs"), "403: not in the seen list");
    g_fake.http = 401;
    photo_inbox_tick(800 + 2000);
    EXPECT_INT(photo_inbox_status("$403:hs", 1700000600000LL, NULL, 0), PHOTO_ST_LOADING,
               "401: still loading (retried)");
    EXPECT(!photo_inbox_seen("$403:hs"), "401: not in the seen list");
    photo_inbox_init(dir, inbox_cb, NULL);                  /* "reboot" */
    photo_inbox_test_set_fetch(fake_fetch);
    EXPECT(!photo_inbox_seen("$403:hs"), "401/403: not in the seen list on disk either");
    g_fake.status = MATRIX_OK; g_fake.http = 200;           /* the token is fixed */
    photo_inbox_offer(e);
    photo_inbox_tick(900);
    EXPECT_INT(photo_inbox_status("$403:hs", 1700000600000LL, NULL, 0), PHOTO_ST_READY,
               "after the fix it is fetched and saved");
    cJSON_Delete(e);

    /* 404 on both endpoints: the media is gone for good. */
    g_fake.status = MATRIX_ERR_HTTP; g_fake.http = 404;
    e = image_event("$404:hs", "mxc://hs/g", NULL, -1, 1700000650000.0);
    photo_inbox_offer(e);
    photo_inbox_tick(950);
    photo_inbox_tick(951);
    EXPECT_INT(photo_inbox_status("$404:hs", 1700000650000LL, NULL, 0), PHOTO_ST_REFUSED,
               "404 on the legacy endpoint too: refused");
    EXPECT(photo_inbox_seen("$404:hs"), "...and seen");
    cJSON_Delete(e);

    /* Transient: backoff, retried for as long as it takes (like a text). */
    g_fake.status = MATRIX_ERR_TRANSPORT; g_fake.http = 0;
    calls = g_fake.calls;
    e = image_event("$net:hs", "mxc://hs/n", NULL, -1, 1700000700000.0);
    photo_inbox_offer(e);
    photo_inbox_tick(1000);
    EXPECT_INT(g_fake.calls, calls + 1, "first try");
    photo_inbox_tick(1500);
    EXPECT_INT(g_fake.calls, calls + 1, "backing off: no try before 2 s");
    EXPECT_INT(photo_inbox_status("$net:hs", 1700000700000LL, NULL, 0), PHOTO_ST_LOADING, "still loading while retrying");
    uint32_t t = 1000;
    int told = g_cb.calls;
    for (int i = 0; i < 30; i++) { t += 61000; photo_inbox_tick(t); }    /* half an hour down */
    EXPECT_INT(g_fake.calls, calls + 31, "retried every minute at most, never given up");
    EXPECT_INT(photo_inbox_status("$net:hs", 1700000700000LL, NULL, 0), PHOTO_ST_FAILED,
               "after PHOTO_INBOX_TRIES the bubble says it didn't arrive");
    EXPECT_INT(g_cb.calls, told + 1, "told once that it's late");
    EXPECT(!photo_inbox_seen("$net:hs"), "late ones are not seen");
    t += 30000; photo_inbox_tick(t);
    EXPECT_INT(g_fake.calls, calls + 31, "the cap: no try before 60 s");
    g_fake.status = MATRIX_OK; g_fake.http = 200;           /* the network is back */
    t += 31000; photo_inbox_tick(t);
    EXPECT_INT(photo_inbox_status("$net:hs", 1700000700000LL, NULL, 0), PHOTO_ST_READY,
               "it arrives once the network is back");
    EXPECT(g_cb.last == PHOTO_ST_READY && g_cb.calls == told + 2, "...and the bubble is told");
    cJSON_Delete(e);

    /* One download at a time: a second photo waits for the first. */
    g_fake.defer = true;
    calls = g_fake.calls;
    cJSON *a = image_event("$qa:hs", "mxc://hs/qa", NULL, -1, 1700000800000.0);
    cJSON *b = image_event("$qb:hs", "mxc://hs/qb", NULL, -1, 1700000900000.0);
    photo_inbox_offer(a);
    photo_inbox_offer(b);
    t += 1000; photo_inbox_tick(t);
    t += 1000; photo_inbox_tick(t);
    EXPECT_INT(g_fake.calls, calls + 1, "one in flight");
    g_fake.cb(jbytes, jlen, 200, MATRIX_OK, g_fake.ud);
    t += 1000; photo_inbox_tick(t);
    EXPECT_INT(g_fake.calls, calls + 2, "the next starts after it");
    g_fake.cb(jbytes, jlen, 200, MATRIX_OK, g_fake.ud);
    EXPECT_INT(photo_inbox_status("$qb:hs", 1700000900000LL, NULL, 0), PHOTO_ST_READY, "both saved");
    /* A late callback for an unknown serial is ignored. */
    g_fake.cb(jbytes, jlen, 200, MATRIX_OK, (void *)(uintptr_t)999999);
    cJSON_Delete(a);
    cJSON_Delete(b);

    /* A non-photo event offered: nothing. */
    cJSON *txt = cJSON_Parse("{\"type\":\"m.room.message\",\"event_id\":\"$t\",\"content\":{\"msgtype\":\"m.text\",\"body\":\"hi\"}}");
    photo_inbox_offer(txt);
    EXPECT_INT(photo_inbox_status("$t", 0, NULL, 0), PHOTO_ST_UNKNOWN, "text: unknown");
    cJSON_Delete(txt);

    EXPECT_INT(count_hidden_tmp(dir), 0, "no temp files left");
    photo_inbox_test_set_fetch(NULL);
    photo_inbox_deinit();
    free(jbytes);
}

static void test_inbox_seen_cap(void)
{
    char dir[600], id[64];
    snprintf(dir, sizeof(dir), "%s/inbox-seen", g_root);
    photo_inbox_init(dir, NULL, NULL);
    photo_inbox_test_set_fetch(fake_fetch);
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.status = MATRIX_ERR_HTTP; g_fake.http = 410;      /* each one refused → seen */
    for (int i = 0; i < PHOTO_INBOX_SEEN_MAX + 10; i++) {
        snprintf(id, sizeof(id), "$s%d", i);
        cJSON *e = image_event(id, "mxc://hs/x", NULL, -1, 1000.0);
        photo_inbox_offer(e);
        photo_inbox_tick((uint32_t)i);
        cJSON_Delete(e);
    }
    EXPECT(photo_inbox_seen("$s2057"), "newest seen");
    EXPECT(!photo_inbox_seen("$s0"), "oldest forgotten past the cap");
    photo_inbox_init(dir, NULL, NULL);
    EXPECT(photo_inbox_seen("$s2057") && !photo_inbox_seen("$s0"), "the file agrees after a reload");
    photo_inbox_test_set_fetch(NULL);
    photo_inbox_deinit();
}

int main(void)
{
    const char *t = getenv("TMPDIR");
    snprintf(g_root, sizeof(g_root), "%s/test_photos.%d", (t && *t) ? t : "/tmp", (int)getpid());
    rm_rf(g_root);
    mkdir_p(g_root);

    printf("=== photos unit tests ===\n");
    test_lib_scan();
    test_lib_trailing_slash();
    test_decode_basics();
    test_decode_jpeg();
    test_decode_png();
    test_mxc();
    test_inbox_parse();
    test_inbox_filename();
    test_inbox_save();
    test_inbox_flow();
    test_inbox_seen_cap();

    rm_rf(g_root);
    printf("\n%d tests, %d failures\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
