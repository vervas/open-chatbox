/**
 * avatars.c — Avatar image lookup from PNG files on disk
 */

#include "avatars.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define AVATAR_PATH_MAX 256

static char g_avatar_dir[AVATAR_PATH_MAX] = {0};
static char g_avatar_paths[CONFIG_MAX_CONTACTS][AVATAR_PATH_MAX];
static int  g_path_idx = 0;

/* Fallback path returned when no PNG exists */
static char g_default_path[AVATAR_PATH_MAX] = {0};

void avatar_init(const char *dir)
{
    if (!dir) return;
    strncpy(g_avatar_dir, dir, sizeof(g_avatar_dir) - 1);
    g_avatar_dir[sizeof(g_avatar_dir) - 1] = '\0';
    g_path_idx = 0;

    snprintf(g_default_path, sizeof(g_default_path), "A:%s/default.png", dir);
}

const void *avatar_get(const char *name)
{
    if (!g_avatar_dir[0]) return NULL;

    if (name && name[0]) {
        char raw_path[AVATAR_PATH_MAX];
        snprintf(raw_path, sizeof(raw_path), "%s/%s.png", g_avatar_dir, name);

        if (access(raw_path, F_OK) == 0 && g_path_idx < CONFIG_MAX_CONTACTS) {
            int idx = g_path_idx++;
            snprintf(g_avatar_paths[idx], AVATAR_PATH_MAX,
                     "A:%s/%s.png", g_avatar_dir, name);
            return g_avatar_paths[idx];
        }
    }

    return g_default_path;
}
