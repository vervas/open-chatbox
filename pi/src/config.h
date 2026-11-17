/**
 * config.h — Chatbox configuration file parser
 *
 * Config file format (INI-like):
 *
 *   # Comment
 *   homeserver_url = https://matrix.example.org
 *   access_token = <access token>
 *   music_volume_max = 70
 *   default_app = chat
 *   photos_dir = /srv/photos
 *   sleep_photo_viewer_mins = 20
 *
 *   [contacts]
 *   contact.1.name = Mom
 *   contact.1.room_id = !roomid:localhost
 *   contact.1.avatar = mom
 *
 *   contact.2.name = Dad
 *   contact.2.room_id = !roomid:localhost
 *   contact.2.avatar = dad
 *   contact.2.lang = gr
 *
 * contact.N.lang (optional): gr | fr | none — the typing language for that
 * contact's chat (keyboard layout + the ` accent combo, see lang.h). An
 * unknown value warns and means none. The legacy "Name = room_id" form
 * can't carry one.
 *
 * Only lines that START with '#' are comments (no inline comments: the
 * value of "x = 1 ; note" is "1 ; note"). Section headers other than
 * [contacts] are ignored, so their keys are global keys. Every line after
 * [contacts] is read as a contact, so global keys must come before it.
 * See docs/player-spec.md §10 (D9).
 */

#ifndef CHATBOX_CONFIG_H
#define CHATBOX_CONFIG_H

#include <stddef.h>

#include "lang.h"

#define CONFIG_MAX_CONTACTS     10
#define CONFIG_MAX_STR_LEN      256
#define CONFIG_MAX_NAME_LEN     64

/* Music / shell defaults (docs/player-spec.md §10) */
#define CONFIG_DEFAULT_MUSIC_DIR        "/srv/music"
#define CONFIG_DEFAULT_MUSIC_DB         "/var/lib/chatbox/library.db"
#define CONFIG_DEFAULT_MUSIC_ART_DIR    "/var/lib/chatbox/art"
#define CONFIG_DEFAULT_VOLUME_MAX       70
#define CONFIG_DEFAULT_VOLUME_DEFAULT   40

/* Photos (docs/photos.md) */
#define CONFIG_DEFAULT_PHOTOS_DIR       "/srv/photos"
/* While the photo viewer is open the screen locks after this many idle
 * minutes instead of sleep_lock_secs. */
#define CONFIG_DEFAULT_PHOTO_VIEWER_MINS 20
#define CONFIG_MAX_PHOTO_VIEWER_MINS     1440

/* What's shown at boot (default_app = chat | music | home). home is the
 * app switcher, open over chat. */
typedef enum {
    CONFIG_APP_CHAT = 0,
    CONFIG_APP_MUSIC,
    CONFIG_APP_HOME,
} config_app_t;

typedef struct {
    char name[CONFIG_MAX_NAME_LEN];
    char room_id[CONFIG_MAX_STR_LEN];
    char avatar[CONFIG_MAX_NAME_LEN];
    chat_lang_t lang;       /* contact.N.lang; LANG_NONE when absent */
} contact_t;

typedef struct {
    char homeserver_url[CONFIG_MAX_STR_LEN];
    char access_token[CONFIG_MAX_STR_LEN];
    contact_t contacts[CONFIG_MAX_CONTACTS];
    int contact_count;
    int sleep_lock_secs;    /* idle seconds before lock screen (0 = use default 60)  */
    int sleep_off_secs;     /* seconds in lock before display off (0 = use default 300) */

    /* Music / shell. Always populated: missing keys take the defaults above. */
    char music_dir[CONFIG_MAX_STR_LEN];
    char music_db[CONFIG_MAX_STR_LEN];
    char music_art_dir[CONFIG_MAX_STR_LEN];
    int music_volume_max;       /* parent-set cap, 0..100                    */
    int music_volume_default;   /* volume at boot, 0..music_volume_max       */
    config_app_t default_app;

    /* Photos. Always populated, like the music fields. */
    char photos_dir[CONFIG_MAX_STR_LEN];
    int sleep_photo_viewer_mins; /* idle minutes before the lock while the
                                  * photo viewer is open; 0 = no override */
} config_t;

/**
 * Parse a chatbox config file.
 *
 * @param path  Path to the config file (e.g., "/etc/chatbox.conf")
 * @param cfg   Output config struct to populate
 * @return 0 on success, -1 on error (error logged to stderr)
 *
 * The music/shell fields are set to their defaults before the file is read,
 * so they are valid even when -1 is returned. Invalid values never fail the
 * parse: they fall back or clamp with a stderr warning:
 *   - music_volume_max / music_volume_default must be a whole decimal
 *     integer (optional sign, nothing else). Anything else ("loud", "70%",
 *     "70 ; note", empty) keeps the default. Numbers clamp to 0..100.
 *   - music_volume_default is clamped to <= music_volume_max after the whole
 *     file is read (key order doesn't matter).
 *   - default_app other than "chat" / "music" -> chat.
 *   - An empty music_dir / music_db / music_art_dir / photos_dir keeps the
 *     default.
 *   - sleep_photo_viewer_mins must be a whole number like the volumes;
 *     garbage keeps the default (20), negatives clamp to 0 (no override),
 *     anything above CONFIG_MAX_PHOTO_VIEWER_MINS clamps to it.
 */
int config_parse(const char *path, config_t *cfg);

/**
 * Print the parsed config to stdout (for debugging / test_config).
 */
void config_print(const config_t *cfg);

/** "chat" / "music" for a config_app_t. */
const char *config_app_name(config_app_t app);

#endif /* CHATBOX_CONFIG_H */
