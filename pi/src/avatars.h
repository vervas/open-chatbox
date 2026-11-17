/**
 * avatars.h — Contact avatar image lookup
 *
 * Avatars are PNG files loaded from disk at runtime.
 * Call avatar_init() with the directory path, then avatar_get()
 * to resolve a config avatar name to an LVGL image source.
 */

#ifndef CHATBOX_AVATARS_H
#define CHATBOX_AVATARS_H

#include "lvgl.h"

/**
 * Set the avatar directory path.
 * Must be called before avatar_get().
 *
 * @param dir  Directory containing {name}.png files
 *             (e.g., "/usr/share/chatbox/avatars")
 */
void avatar_init(const char *dir);

/**
 * Look up an avatar image by name.
 *
 * Checks for {dir}/{name}.png on disk.
 * Falls back to {dir}/default.png if not found.
 *
 * @param name  Avatar identifier from config (e.g., "mom", "dad")
 * @return Source suitable for lv_image_set_src() — an "A:path" string.
 *         Never returns NULL.
 */
const void *avatar_get(const char *name);

#endif /* CHATBOX_AVATARS_H */
