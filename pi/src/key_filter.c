/**
 * key_filter.c — One-shot key filter (player spec D2). See key_filter.h.
 */

#include "key_filter.h"

#include <string.h>

void key_filter_init(key_filter_t *kf)
{
    memset(kf, 0, sizeof(*kf));
}

/* Find the slot for `key`, or NULL if it has no record. */
static key_filter_slot_t *find_slot(key_filter_t *kf, uint32_t key)
{
    for (int i = 0; i < KEY_FILTER_SLOTS; i++) {
        if (kf->slots[i].used && kf->slots[i].key == key)
            return &kf->slots[i];
    }
    return NULL;
}

/* Pick a slot for a key with no record: a free one, else the one whose
 * last event is oldest relative to `now_ms` (unsigned age, wrap-safe). */
static key_filter_slot_t *claim_slot(key_filter_t *kf, uint32_t now_ms)
{
    key_filter_slot_t *oldest = &kf->slots[0];
    uint32_t oldest_age = 0;

    for (int i = 0; i < KEY_FILTER_SLOTS; i++) {
        key_filter_slot_t *s = &kf->slots[i];
        if (!s->used)
            return s;
        uint32_t age = now_ms - s->last_ms;
        if (age > oldest_age) {
            oldest_age = age;
            oldest = s;
        }
    }
    return oldest;
}

bool key_filter_accept(key_filter_t *kf, uint32_t key, uint32_t now_ms,
                       bool one_shot)
{
    bool accept = true;
    key_filter_slot_t *s = find_slot(kf, key);

    if (s) {
        /* Unsigned subtraction: correct across the 2^32 tick wrap. */
        if (one_shot && (uint32_t)(now_ms - s->last_ms) < KEY_FILTER_WINDOW_MS)
            accept = false;
    } else {
        /* No previous event on record: a fresh key is never dropped. */
        s = claim_slot(kf, now_ms);
        s->key = key;
        s->used = true;
    }

    /* Always record, accepted or dropped, so a held key's window slides. */
    s->last_ms = now_ms;
    return accept;
}

void key_filter_release(key_filter_t *kf, uint32_t key)
{
    key_filter_slot_t *s = find_slot(kf, key);
    /* A free slot is "no previous event": the next press is fresh. */
    if (s) memset(s, 0, sizeof(*s));
}

void key_filter_record(key_filter_t *kf, uint32_t key, uint32_t now_ms)
{
    (void)key_filter_accept(kf, key, now_ms, false);
}

bool key_filter_is_one_shot(uint32_t key, key_ctx_t ctx)
{
    switch (ctx) {
    case KEY_CTX_MUSIC_BROWSE:
    case KEY_CTX_NOW_PLAYING:
        /* Now Playing: ←/→ skip tracks (one-shot), ↑/↓ are volume and
         * repeat like PgUp/PgDn. Win toggles Now Playing ⇄ Albums, so a
         * held Win must not flicker between them. */
        return key == KF_KEY_SPACE || key == KF_KEY_ENTER ||
               key == KF_KEY_LEFT  || key == KF_KEY_RIGHT ||
               key == KF_KEY_ESC   || key == KF_KEY_MENU;
    case KEY_CTX_NO_MUSIC:
        return key == KF_KEY_SPACE || key == KF_KEY_ESC;
    case KEY_CTX_SWITCHER:
        return key == KF_KEY_ENTER || key == KF_KEY_LEFT ||
               key == KF_KEY_RIGHT || key == KF_KEY_ESC;
    case KEY_CTX_CHAT:
        return key == KF_KEY_ESC;
    case KEY_CTX_PHOTOS:
        /* ←/→ step one card or one photo (a held arrow would decode photo
         * after photo in the viewer); ↑/↓ repeat to scroll the grid like
         * music's. Delete and Enter must never act twice. */
        return key == KF_KEY_ENTER || key == KF_KEY_ESC || key == KF_KEY_LEFT ||
               key == KF_KEY_RIGHT || key == KF_KEY_DEL;
    default:
        return false;
    }
}
