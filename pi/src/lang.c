/**
 * lang.c — per-contact typing language. See lang.h.
 */

#include "lang.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

typedef struct {
    uint32_t    vowel;
    const char *accented;
} accent_t;

/* Greek tonos (the font_chat_* ranges cover U+0370-03FF) */
static const accent_t g_greek[] = {
    { 0x03B1, "ά" }, { 0x03B5, "έ" }, { 0x03B7, "ή" }, { 0x03B9, "ί" },
    { 0x03BF, "ό" }, { 0x03C5, "ύ" }, { 0x03C9, "ώ" },
    { 0x0391, "Ά" }, { 0x0395, "Έ" }, { 0x0397, "Ή" }, { 0x0399, "Ί" },
    { 0x039F, "Ό" }, { 0x03A5, "Ύ" }, { 0x03A9, "Ώ" },
    { 0, NULL }
};

/* French acute, on every vowel so ` behaves the same whatever follows */
static const accent_t g_french_acute[] = {
    { 'a', "á" }, { 'e', "é" }, { 'i', "í" }, { 'o', "ó" }, { 'u', "ú" },
    { 'A', "Á" }, { 'E', "É" }, { 'I', "Í" }, { 'O', "Ó" }, { 'U', "Ú" },
    { 0, NULL }
};

/* French grave, on every vowel too */
static const accent_t g_french_grave[] = {
    { 'a', "à" }, { 'e', "è" }, { 'i', "ì" }, { 'o', "ò" }, { 'u', "ù" },
    { 'A', "À" }, { 'E', "È" }, { 'I', "Ì" }, { 'O', "Ò" }, { 'U', "Ù" },
    { 0, NULL }
};

typedef struct {
    const char     *name;
    const char     *xkb_layout;
    const accent_t *acute;      /* ` + vowel */
    const accent_t *grave;      /* `` + vowel */
} lang_info_t;

static const lang_info_t g_langs[LANG_COUNT] = {
    [LANG_NONE]   = { "none", NULL, NULL,           NULL           },
    [LANG_GREEK]  = { "gr",   "gr", g_greek,        NULL           },
    [LANG_FRENCH] = { "fr",   NULL, g_french_acute, g_french_grave },
};

static const lang_info_t *info(chat_lang_t lang)
{
    return ((int)lang >= 0 && lang < LANG_COUNT) ? &g_langs[lang] : &g_langs[LANG_NONE];
}

int lang_from_name(const char *name)
{
    if (!name || !*name || strcasecmp(name, "none") == 0) return LANG_NONE;
    if (strcasecmp(name, "gr") == 0 || strcasecmp(name, "el") == 0 ||
        strcasecmp(name, "greek") == 0)
        return LANG_GREEK;
    if (strcasecmp(name, "fr") == 0 || strcasecmp(name, "french") == 0)
        return LANG_FRENCH;
    return -1;
}

const char *lang_name(chat_lang_t lang)       { return info(lang)->name; }
const char *lang_xkb_layout(chat_lang_t lang) { return info(lang)->xkb_layout; }

static const char *lookup(const accent_t *a, uint32_t vowel)
{
    for (; a && a->accented; a++)
        if (a->vowel == vowel) return a->accented;
    return NULL;
}

/* Decode one UTF-8 character; *len gets its byte length (0 on bad input). */
static uint32_t utf8_decode(const char *s, size_t *len)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80)                                  { *len = 1; return u[0]; }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *len = 2;
        return ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *len = 3;
        return ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    }
    *len = 0;
    return 0;
}

const char *lang_accent_combo(chat_lang_t lang, const char *text,
                              uint32_t cursor, const char *inserted,
                              uint32_t *ticks)
{
    if (!text || !inserted || cursor == 0 || !ticks) return NULL;
    const lang_info_t *li = info(lang);
    if (!li->acute && !li->grave) return NULL;

    /* Exactly one character inserted */
    size_t len;
    uint32_t vowel = utf8_decode(inserted, &len);
    if (len == 0 || inserted[len] != '\0') return NULL;

    /* The two characters before the cursor */
    const char *p = text, *prev = NULL, *prev2 = NULL;
    for (uint32_t i = 0; i < cursor && *p; i++) {
        prev2 = prev;
        prev = p;
        p++;
        while ((*p & 0xC0) == 0x80) p++;   /* skip continuation bytes */
    }
    if (!prev || *prev != LANG_ACCENT_KEY || p != prev + 1) return NULL;

    /* `` -> grave. Otherwise the last ` gives the acute; so does `` in a
     * language without a grave (Greek), leaving the first ` as typed. */
    const char *accented;
    if (prev2 && *prev2 == LANG_ACCENT_KEY &&
        (accented = lookup(li->grave, vowel)) != NULL) {
        *ticks = 2;
        return accented;
    }
    if ((accented = lookup(li->acute, vowel)) != NULL) {
        *ticks = 1;
        return accented;
    }
    return NULL;
}
