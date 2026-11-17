/**
 * photo_lib.h — The photo library: one folder of pictures, newest first
 * (docs/photos.md).
 *
 * The library is the folder itself (photos_dir, /srv/photos on the Pi):
 * no database, no albums. A scan lists the folder's regular files named
 * *.jpg, *.jpeg or *.png (any case), skipping dotfiles (chat's downloads
 * are written as hidden temp files and renamed in, photo_inbox.c) and
 * empty files, and sorts them newest first by mtime (ties: by name,
 * descending, so the order is stable).
 *
 * Partial files: a file still being copied in by hand is listed like any
 * other; it may not decode yet (the UI then shows its placeholder) and
 * the next scan sees its size and mtime change and reports the library as
 * changed, so it is decoded again once complete.
 *
 * Changes are found by photo_lib_poll(): cheap when nothing happened (one
 * stat of the folder), a full rescan when the folder's mtime moved (a file
 * added, removed or renamed) and every PHOTO_LIB_FULL_EVERY polls anyway
 * (a file whose content changed in place). A rescan only counts as a change
 * when what it found differs (names, sizes, mtimes).
 *
 * Pure C, no LVGL. Threading: one thread (the LVGL thread in the kiosk).
 */

#ifndef CHATBOX_PHOTO_LIB_H
#define CHATBOX_PHOTO_LIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define PHOTO_LIB_MAX        5000   /* photos listed; the oldest beyond are left out */
#define PHOTO_NAME_MAX         96   /* longer names are skipped */
#define PHOTO_LIB_FULL_EVERY   10   /* polls between unconditional rescans */

typedef struct {
    char    name[PHOTO_NAME_MAX];   /* file name inside the folder */
    time_t  mtime;
    int64_t size;
} photo_t;

typedef struct photo_lib photo_lib_t;

/* Open the library on `dir` and scan it. A missing folder is not an error:
 * the library is empty until it appears. NULL only on OOM. */
photo_lib_t *photo_lib_open(const char *dir);
void         photo_lib_close(photo_lib_t *lib);

const char *photo_lib_dir(const photo_lib_t *lib);

/* Photos, newest first. photo_lib_get returns NULL out of range. */
int            photo_lib_count(const photo_lib_t *lib);
const photo_t *photo_lib_get(const photo_lib_t *lib, int i);

/* "<dir>/<name>" of photo i; false out of range or too long. */
bool photo_lib_path(const photo_lib_t *lib, int i, char *out, size_t n);

/* Index of the photo called `name`, or -1. */
int photo_lib_find(const photo_lib_t *lib, const char *name);

/* Rescan now. Returns true when the list changed. */
bool photo_lib_rescan(photo_lib_t *lib);

/* The 2 s poll (see the header). Returns true when the list changed. */
bool photo_lib_poll(photo_lib_t *lib);

/* Bumped on every change; the UI compares it to what it drew. */
unsigned photo_lib_generation(const photo_lib_t *lib);

/* Delete photo i from disk (unlink) and rescan. Returns 0 on success, -1
 * when i is out of range or the unlink failed (errno set; the list is
 * rescanned either way, so a file someone else removed just disappears). */
int photo_lib_delete(photo_lib_t *lib, int i);

/* True when `name` is a name the library lists: not hidden, a .jpg /
 * .jpeg / .png extension (any case), shorter than PHOTO_NAME_MAX. */
bool photo_lib_name_ok(const char *name);

#ifdef CHATBOX_TESTING
/* Full rescans (folder listed and every file stat'ed) since open. */
unsigned photo_lib_test_scans(const photo_lib_t *lib);
#endif

#endif /* CHATBOX_PHOTO_LIB_H */
