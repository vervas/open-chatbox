/**
 * test_photo_fixture.h — Test photos written at test time (test_photos,
 * test_keyboard): a little painted landscape (sky, sun, hills) whose
 * colours come from `seed`, as a JPEG (optionally with an EXIF
 * orientation) or a PNG (optionally with transparency).
 *
 * The paint is RGB, top-left origin. Known pixels for the tests: the top
 * row is sky, the bottom row is hill green, and the left edge of the image
 * has a 1/16-wide red stripe, so a rotation or mirror is detectable.
 */

#ifndef CHATBOX_TEST_PHOTO_FIXTURE_H
#define CHATBOX_TEST_PHOTO_FIXTURE_H

#include <stdbool.h>
#include <stdint.h>

void photo_fixture_paint(uint8_t *rgb, int w, int h, int seed);

/* Return false when the library is missing from this build. */
bool photo_fixture_jpeg(const char *path, int w, int h, int seed, int exif_orient);
bool photo_fixture_png(const char *path, int w, int h, int seed, bool alpha);

#endif /* CHATBOX_TEST_PHOTO_FIXTURE_H */
