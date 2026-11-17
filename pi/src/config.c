/**
 * config.c — Chatbox INI-like config file parser
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

/* Strip leading and trailing whitespace in-place. Returns pointer to first
 * non-space character (which may be inside the original buffer). */
static char *trim(char *s)
{
    /* Leading whitespace */
    while (*s && isspace((unsigned char)*s))
        s++;

    /* Trailing whitespace */
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1)))
        end--;
    *end = '\0';

    return s;
}

/* Copy at most dst_size-1 chars from src into dst, ensuring NUL termination */
static void safe_copy(char *dst, const char *src, size_t dst_size)
{
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/* Strict integer parse: the whole (already trimmed) value must be a decimal
 * integer with an optional sign. Unlike atoi(), garbage such as "loud" or
 * "70%" is rejected instead of silently becoming 0 or 70. Out-of-range
 * values saturate to INT_MIN/INT_MAX (the caller clamps anyway).
 * Returns 0 and sets *out on success, -1 on garbage. */
static int parse_int_strict(const char *value, int *out)
{
    if (*value == '\0')
        return -1;
    char *end = NULL;
    errno = 0;
    long v = strtol(value, &end, 10);
    if (end == value || *end != '\0')
        return -1;
    if (v > INT_MAX || (errno == ERANGE && v > 0)) v = INT_MAX;
    if (v < INT_MIN || (errno == ERANGE && v < 0)) v = INT_MIN;
    *out = (int)v;
    return 0;
}

/* Parse a 0..100 volume key. Garbage keeps the current value (the default);
 * out-of-range numbers clamp. Both warn on stderr. */
static void parse_volume(const char *key, const char *value, int line_num, int *dst)
{
    int v;
    if (parse_int_strict(value, &v) != 0) {
        fprintf(stderr, "config_parse: line %d: %s = '%s' is not a number, using %d\n",
                line_num, key, value, *dst);
        return;
    }
    if (v < 0 || v > 100) {
        int c = v < 0 ? 0 : 100;
        fprintf(stderr, "config_parse: line %d: %s = %d out of range 0..100, clamped to %d\n",
                line_num, key, v, c);
        v = c;
    }
    *dst = v;
}

/* Parse sleep_photo_viewer_mins: garbage keeps the current value (the
 * default), out-of-range numbers clamp to 0..CONFIG_MAX_PHOTO_VIEWER_MINS. */
static void parse_minutes(const char *key, const char *value, int line_num, int *dst)
{
    int v;
    if (parse_int_strict(value, &v) != 0) {
        fprintf(stderr, "config_parse: line %d: %s = '%s' is not a number, using %d\n",
                line_num, key, value, *dst);
        return;
    }
    if (v < 0 || v > CONFIG_MAX_PHOTO_VIEWER_MINS) {
        int c = v < 0 ? 0 : CONFIG_MAX_PHOTO_VIEWER_MINS;
        fprintf(stderr, "config_parse: line %d: %s = %d out of range 0..%d, clamped to %d\n",
                line_num, key, v, CONFIG_MAX_PHOTO_VIEWER_MINS, c);
        v = c;
    }
    *dst = v;
}

/* Copy a path key; an empty value keeps the default. */
static void parse_path(const char *key, const char *value, int line_num,
                       char *dst, size_t dst_size)
{
    if (*value == '\0') {
        fprintf(stderr, "config_parse: line %d: %s is empty, using %s\n",
                line_num, key, dst);
        return;
    }
    safe_copy(dst, value, dst_size);
}

static void set_defaults(config_t *cfg)
{
    safe_copy(cfg->music_dir,     CONFIG_DEFAULT_MUSIC_DIR,     sizeof(cfg->music_dir));
    safe_copy(cfg->music_db,      CONFIG_DEFAULT_MUSIC_DB,      sizeof(cfg->music_db));
    safe_copy(cfg->music_art_dir, CONFIG_DEFAULT_MUSIC_ART_DIR, sizeof(cfg->music_art_dir));
    cfg->music_volume_max     = CONFIG_DEFAULT_VOLUME_MAX;
    cfg->music_volume_default = CONFIG_DEFAULT_VOLUME_DEFAULT;
    cfg->default_app          = CONFIG_APP_CHAT;
    safe_copy(cfg->photos_dir,    CONFIG_DEFAULT_PHOTOS_DIR,    sizeof(cfg->photos_dir));
    cfg->sleep_photo_viewer_mins = CONFIG_DEFAULT_PHOTO_VIEWER_MINS;
}

const char *config_app_name(config_app_t app)
{
    return app == CONFIG_APP_MUSIC ? "music" : app == CONFIG_APP_HOME ? "home" : "chat";
}

int config_parse(const char *path, config_t *cfg)
{
    if (!path || !cfg) {
        fprintf(stderr, "config_parse: NULL argument\n");
        return -1;
    }

    memset(cfg, 0, sizeof(*cfg));
    set_defaults(cfg);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "config_parse: cannot open '%s': ", path);
        perror("");
        return -1;
    }

    char line[512];
    int in_contacts = 0;
    int line_num = 0;

    while (fgets(line, sizeof(line), f)) {
        line_num++;
        char *p = trim(line);

        /* Skip blank lines and comments */
        if (*p == '\0' || *p == '#')
            continue;

        /* Section header */
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (!end) {
                fprintf(stderr, "config_parse: malformed section header at line %d\n", line_num);
                fclose(f);
                return -1;
            }
            *end = '\0';
            char *section = trim(p + 1);
            if (strcmp(section, "contacts") == 0) {
                in_contacts = 1;
            } else {
                in_contacts = 0;
            }
            continue;
        }

        /* Key = Value */
        char *eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "config_parse: syntax error at line %d (no '='): %s\n", line_num, p);
            fclose(f);
            return -1;
        }

        *eq = '\0';
        char *key   = trim(p);
        char *value = trim(eq + 1);

        if (*key == '\0') {
            fprintf(stderr, "config_parse: empty key at line %d\n", line_num);
            fclose(f);
            return -1;
        }

        if (in_contacts) {
            /* New format: contact.N.field = value */
            int idx = -1;
            char field[64] = {0};
            if (sscanf(key, "contact.%d.%63s", &idx, field) == 2 && idx >= 1) {
                idx--;  /* 1-based in config, 0-based in array */
                if (idx >= CONFIG_MAX_CONTACTS) continue;
                contact_t *c = &cfg->contacts[idx];
                if (strcmp(field, "name") == 0) {
                    safe_copy(c->name, value, sizeof(c->name));
                } else if (strcmp(field, "room_id") == 0) {
                    safe_copy(c->room_id, value, sizeof(c->room_id));
                } else if (strcmp(field, "avatar") == 0) {
                    safe_copy(c->avatar, value, sizeof(c->avatar));
                } else if (strcmp(field, "lang") == 0) {
                    int lang = lang_from_name(value);
                    if (lang < 0) {
                        fprintf(stderr, "config_parse: line %d: unknown %s '%s', using none\n",
                                line_num, key, value);
                        lang = LANG_NONE;
                    }
                    c->lang = (chat_lang_t)lang;
                }
                if (idx >= cfg->contact_count)
                    cfg->contact_count = idx + 1;
            } else {
                /* Legacy format: Name = room_id */
                if (cfg->contact_count >= CONFIG_MAX_CONTACTS) {
                    fprintf(stderr, "config_parse: maximum contacts (%d) exceeded\n", CONFIG_MAX_CONTACTS);
                    continue;
                }
                contact_t *c = &cfg->contacts[cfg->contact_count];
                safe_copy(c->name,    key,   sizeof(c->name));
                safe_copy(c->room_id, value, sizeof(c->room_id));
                cfg->contact_count++;
            }
        } else {
            if (strcmp(key, "homeserver_url") == 0) {
                safe_copy(cfg->homeserver_url, value, sizeof(cfg->homeserver_url));
            } else if (strcmp(key, "access_token") == 0) {
                safe_copy(cfg->access_token, value, sizeof(cfg->access_token));
            } else if (strcmp(key, "sleep_lock_secs") == 0) {
                cfg->sleep_lock_secs = atoi(value);
            } else if (strcmp(key, "sleep_off_secs") == 0) {
                cfg->sleep_off_secs = atoi(value);
            } else if (strcmp(key, "music_dir") == 0) {
                parse_path(key, value, line_num, cfg->music_dir, sizeof(cfg->music_dir));
            } else if (strcmp(key, "music_db") == 0) {
                parse_path(key, value, line_num, cfg->music_db, sizeof(cfg->music_db));
            } else if (strcmp(key, "music_art_dir") == 0) {
                parse_path(key, value, line_num, cfg->music_art_dir, sizeof(cfg->music_art_dir));
            } else if (strcmp(key, "music_volume_max") == 0) {
                parse_volume(key, value, line_num, &cfg->music_volume_max);
            } else if (strcmp(key, "music_volume_default") == 0) {
                parse_volume(key, value, line_num, &cfg->music_volume_default);
            } else if (strcmp(key, "photos_dir") == 0) {
                parse_path(key, value, line_num, cfg->photos_dir, sizeof(cfg->photos_dir));
            } else if (strcmp(key, "sleep_photo_viewer_mins") == 0) {
                parse_minutes(key, value, line_num, &cfg->sleep_photo_viewer_mins);
            } else if (strcmp(key, "default_app") == 0) {
                if (strcmp(value, "chat") == 0) {
                    cfg->default_app = CONFIG_APP_CHAT;
                } else if (strcmp(value, "music") == 0) {
                    cfg->default_app = CONFIG_APP_MUSIC;
                } else if (strcmp(value, "home") == 0) {
                    cfg->default_app = CONFIG_APP_HOME;
                } else {
                    fprintf(stderr, "config_parse: line %d: unknown default_app '%s', using chat\n",
                            line_num, value);
                    cfg->default_app = CONFIG_APP_CHAT;
                }
            }
            /* Unknown keys in the global section are silently ignored */
        }
    }

    fclose(f);

    /* After the whole file, so the order of the two volume keys doesn't matter */
    if (cfg->music_volume_default > cfg->music_volume_max) {
        fprintf(stderr, "config_parse: music_volume_default %d > music_volume_max %d, clamped to %d\n",
                cfg->music_volume_default, cfg->music_volume_max, cfg->music_volume_max);
        cfg->music_volume_default = cfg->music_volume_max;
    }

    /* Validate required fields */
    if (cfg->homeserver_url[0] == '\0') {
        fprintf(stderr, "config_parse: 'homeserver_url' is missing or empty\n");
        return -1;
    }
    if (cfg->access_token[0] == '\0') {
        fprintf(stderr, "config_parse: 'access_token' is missing or empty\n");
        return -1;
    }

    return 0;
}

void config_print(const config_t *cfg)
{
    printf("homeserver_url = %s\n", cfg->homeserver_url);
    printf("access_token = %s\n",   cfg->access_token);
    printf("sleep_lock_secs = %d\n", cfg->sleep_lock_secs);
    printf("sleep_off_secs = %d\n",  cfg->sleep_off_secs);
    printf("music_dir = %s\n",            cfg->music_dir);
    printf("music_db = %s\n",             cfg->music_db);
    printf("music_art_dir = %s\n",        cfg->music_art_dir);
    printf("music_volume_max = %d\n",     cfg->music_volume_max);
    printf("music_volume_default = %d\n", cfg->music_volume_default);
    printf("default_app = %s\n",          config_app_name(cfg->default_app));
    printf("photos_dir = %s\n",           cfg->photos_dir);
    printf("sleep_photo_viewer_mins = %d\n", cfg->sleep_photo_viewer_mins);
    for (int i = 0; i < cfg->contact_count; i++) {
        printf("contact[%d]: %s = %s (avatar=%s, lang=%s)\n",
               i, cfg->contacts[i].name, cfg->contacts[i].room_id,
               cfg->contacts[i].avatar[0] ? cfg->contacts[i].avatar : "default",
               lang_name(cfg->contacts[i].lang));
    }
}
