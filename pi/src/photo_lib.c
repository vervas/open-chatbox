/**
 * photo_lib.c — The photo library (see photo_lib.h).
 */

#include "photo_lib.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

struct photo_lib {
    char     dir[512];
    photo_t *items;          /* PHOTO_LIB_MAX slots, malloc'd once */
    photo_t *scratch;        /* the same, where a rescan lists the folder */
    int      count;
    unsigned gen;
    bool     dir_seen;       /* the folder's mtime below is valid */
    int64_t  dir_sec, dir_nsec;
    int      polls;          /* since the last full rescan */
#ifdef CHATBOX_TESTING
    unsigned scans;
#endif
};

/* ---- Helpers ------------------------------------------------------------- */

static void stat_mtime(const struct stat *st, int64_t *sec, int64_t *nsec)
{
#if defined(__APPLE__)
    *sec  = (int64_t)st->st_mtimespec.tv_sec;
    *nsec = (int64_t)st->st_mtimespec.tv_nsec;
#else
    *sec  = (int64_t)st->st_mtim.tv_sec;
    *nsec = (int64_t)st->st_mtim.tv_nsec;
#endif
}

bool photo_lib_name_ok(const char *name)
{
    if (!name || !name[0] || name[0] == '.') return false;
    size_t len = strlen(name);
    if (len >= PHOTO_NAME_MAX) return false;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return false;
    return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 ||
           strcasecmp(dot, ".png") == 0;
}

/* Newest first; equal mtimes by name, descending. */
static int cmp_photo(const void *a, const void *b)
{
    const photo_t *x = a, *y = b;
    if (x->mtime != y->mtime) return x->mtime > y->mtime ? -1 : 1;
    return -strcmp(x->name, y->name);
}

static bool same_list(const photo_t *a, int na, const photo_t *b, int nb)
{
    if (na != nb) return false;
    for (int i = 0; i < na; i++) {
        if (a[i].mtime != b[i].mtime || a[i].size != b[i].size ||
            strcmp(a[i].name, b[i].name) != 0)
            return false;
    }
    return true;
}

/* ---- Scan ---------------------------------------------------------------- */

bool photo_lib_rescan(photo_lib_t *lib)
{
    if (!lib) return false;
    lib->polls = 0;
#ifdef CHATBOX_TESTING
    lib->scans++;
#endif

    struct stat dst;
    if (stat(lib->dir, &dst) == 0) {
        stat_mtime(&dst, &lib->dir_sec, &lib->dir_nsec);
        lib->dir_seen = true;
    } else {
        lib->dir_seen = false;
    }

    /* Scan into the scratch list; keep the old one if nothing changed. More
     * than PHOTO_LIB_MAX files: the list is sorted with the extra ones
     * dropped as they come (the oldest so far is replaced). */
    photo_t *scratch = lib->scratch;
    int n = 0;

    DIR *d = opendir(lib->dir);
    if (d) {
        struct dirent *e;
        char path[1024];
        while ((e = readdir(d)) != NULL) {
            if (!photo_lib_name_ok(e->d_name)) continue;
            int len = snprintf(path, sizeof(path), "%s/%s", lib->dir, e->d_name);
            if (len < 0 || (size_t)len >= sizeof(path)) continue;
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
            photo_t p;
            memset(&p, 0, sizeof(p));
            snprintf(p.name, sizeof(p.name), "%s", e->d_name);
            p.mtime = st.st_mtime;
            p.size  = (int64_t)st.st_size;
            if (n < PHOTO_LIB_MAX) {
                scratch[n++] = p;
            } else {
                /* Full: replace the oldest if this one is newer. */
                int old = 0;
                for (int i = 1; i < n; i++)
                    if (cmp_photo(&scratch[i], &scratch[old]) > 0) old = i;
                if (cmp_photo(&p, &scratch[old]) < 0) scratch[old] = p;
            }
        }
        closedir(d);
    }
    qsort(scratch, (size_t)n, sizeof(photo_t), cmp_photo);

    bool changed = !same_list(scratch, n, lib->items, lib->count);
    if (changed) {
        /* Swap the lists: no copy, and the old one is the next scratch. */
        lib->scratch = lib->items;
        lib->items = scratch;
        lib->count = n;
        lib->gen++;
    }
    return changed;
}

bool photo_lib_poll(photo_lib_t *lib)
{
    if (!lib) return false;
    if (++lib->polls >= PHOTO_LIB_FULL_EVERY) return photo_lib_rescan(lib);

    struct stat st;
    bool seen = stat(lib->dir, &st) == 0;
    int64_t sec = 0, nsec = 0;
    if (seen) stat_mtime(&st, &sec, &nsec);
    if (seen == lib->dir_seen && (!seen || (sec == lib->dir_sec && nsec == lib->dir_nsec)))
        return false;
    return photo_lib_rescan(lib);
}

/* ---- API ----------------------------------------------------------------- */

photo_lib_t *photo_lib_open(const char *dir)
{
    photo_lib_t *lib = calloc(1, sizeof(*lib));
    if (!lib) return NULL;
    lib->items = calloc(PHOTO_LIB_MAX, sizeof(photo_t));
    lib->scratch = calloc(PHOTO_LIB_MAX, sizeof(photo_t));
    if (!lib->items || !lib->scratch) {
        free(lib->items);
        free(lib->scratch);
        free(lib);
        return NULL;
    }
    snprintf(lib->dir, sizeof(lib->dir), "%s", dir ? dir : "");
    /* No trailing slash: paths are "<dir>/<name>". */
    size_t len = strlen(lib->dir);
    while (len > 1 && lib->dir[len - 1] == '/') lib->dir[--len] = '\0';
    photo_lib_rescan(lib);
    lib->gen = 1;
    return lib;
}

void photo_lib_close(photo_lib_t *lib)
{
    if (!lib) return;
    free(lib->items);
    free(lib->scratch);
    free(lib);
}

const char *photo_lib_dir(const photo_lib_t *lib)
{
    return lib ? lib->dir : "";
}

int photo_lib_count(const photo_lib_t *lib)
{
    return lib ? lib->count : 0;
}

const photo_t *photo_lib_get(const photo_lib_t *lib, int i)
{
    if (!lib || i < 0 || i >= lib->count) return NULL;
    return &lib->items[i];
}

bool photo_lib_path(const photo_lib_t *lib, int i, char *out, size_t n)
{
    const photo_t *p = photo_lib_get(lib, i);
    if (!p || !out || n == 0) return false;
    int len = snprintf(out, n, "%s/%s", lib->dir, p->name);
    return len > 0 && (size_t)len < n;
}

int photo_lib_find(const photo_lib_t *lib, const char *name)
{
    if (!lib || !name) return -1;
    for (int i = 0; i < lib->count; i++)
        if (strcmp(lib->items[i].name, name) == 0) return i;
    return -1;
}

unsigned photo_lib_generation(const photo_lib_t *lib)
{
    return lib ? lib->gen : 0;
}

int photo_lib_delete(photo_lib_t *lib, int i)
{
    char path[1024];
    if (!photo_lib_path(lib, i, path, sizeof(path))) {
        errno = EINVAL;
        return -1;
    }
    int rc = unlink(path);
    int err = errno;
    if (rc != 0)
        fprintf(stderr, "photos: cannot delete %s: %s\n", path, strerror(err));
    else
        fprintf(stderr, "photos: deleted %s\n", path);
    photo_lib_rescan(lib);
    errno = err;
    return rc == 0 ? 0 : -1;
}

#ifdef CHATBOX_TESTING
unsigned photo_lib_test_scans(const photo_lib_t *lib)
{
    return lib ? lib->scans : 0;
}
#endif
