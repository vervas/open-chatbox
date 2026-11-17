/**
 * test_lang.c — per-contact language + ` accent combo (lang.c)
 *
 * Usage: test_lang   (exit 1 on failure)
 */

#include <stdio.h>
#include <string.h>

#include "lang.h"

static int g_tests = 0;
static int g_failures = 0;

#define EXPECT(cond, msg) do { \
    g_tests++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

/* Combo result as a string ("" for NULL) so failures print readably */
static const char *combo(chat_lang_t lang, const char *text, unsigned cursor,
                         const char *ins)
{
    uint32_t ticks = 0;
    const char *r = lang_accent_combo(lang, text, cursor, ins, &ticks);
    return r ? r : "";
}

#define EXPECT_COMBO(lang, text, cursor, ins, want) do { \
    const char *_g = combo(lang, text, cursor, ins); \
    g_tests++; \
    if (strcmp(_g, want) != 0) { \
        fprintf(stderr, "FAIL [%s:%d] combo('%s', cursor %u, '%s'): got '%s', want '%s'\n", \
                __FILE__, __LINE__, text, (unsigned)(cursor), ins, _g, want); \
        g_failures++; \
    } \
} while (0)

static void test_names(void)
{
    EXPECT(lang_from_name("gr") == LANG_GREEK,     "gr");
    EXPECT(lang_from_name("el") == LANG_GREEK,     "el");
    EXPECT(lang_from_name("Greek") == LANG_GREEK,  "Greek (case-insensitive)");
    EXPECT(lang_from_name("fr") == LANG_FRENCH,    "fr");
    EXPECT(lang_from_name("FR") == LANG_FRENCH,    "FR");
    EXPECT(lang_from_name("") == LANG_NONE,        "empty -> none");
    EXPECT(lang_from_name("none") == LANG_NONE,    "none");
    EXPECT(lang_from_name("klingon") == -1,        "unknown -> -1");

    EXPECT(strcmp(lang_name(LANG_GREEK), "gr") == 0,  "name gr");

    EXPECT(strcmp(lang_xkb_layout(LANG_GREEK), "gr") == 0, "greek loads gr");
    EXPECT(lang_xkb_layout(LANG_FRENCH) == NULL,  "french keeps default QWERTY");
    EXPECT(lang_xkb_layout(LANG_NONE) == NULL,    "none keeps default");
}

static void test_greek(void)
{
    EXPECT_COMBO(LANG_GREEK, "`", 1, "α", "ά");
    EXPECT_COMBO(LANG_GREEK, "`", 1, "ε", "έ");
    EXPECT_COMBO(LANG_GREEK, "`", 1, "η", "ή");
    EXPECT_COMBO(LANG_GREEK, "`", 1, "ι", "ί");
    EXPECT_COMBO(LANG_GREEK, "`", 1, "ο", "ό");
    EXPECT_COMBO(LANG_GREEK, "`", 1, "υ", "ύ");
    EXPECT_COMBO(LANG_GREEK, "`", 1, "ω", "ώ");
    EXPECT_COMBO(LANG_GREEK, "`", 1, "Α", "Ά");
    EXPECT_COMBO(LANG_GREEK, "`", 1, "Ω", "Ώ");
    /* Mid-word, after multi-byte Greek letters */
    EXPECT_COMBO(LANG_GREEK, "καλημερ`", 8, "α", "ά");
    /* Consonant / Latin vowel: no combo */
    EXPECT_COMBO(LANG_GREEK, "`", 1, "κ", "");
    EXPECT_COMBO(LANG_GREEK, "`", 1, "a", "");
}

static void test_french(void)
{
    /* One `: acute on every vowel */
    EXPECT_COMBO(LANG_FRENCH, "`", 1, "a", "á");
    EXPECT_COMBO(LANG_FRENCH, "`", 1, "e", "é");
    EXPECT_COMBO(LANG_FRENCH, "`", 1, "i", "í");
    EXPECT_COMBO(LANG_FRENCH, "`", 1, "o", "ó");
    EXPECT_COMBO(LANG_FRENCH, "`", 1, "u", "ú");
    EXPECT_COMBO(LANG_FRENCH, "`", 1, "E", "É");
    EXPECT_COMBO(LANG_FRENCH, "caf`", 4, "e", "é");
    EXPECT_COMBO(LANG_FRENCH, "très `", 6, "a", "á");   /* after a 2-byte char */
    /* Two `: grave on every vowel */
    EXPECT_COMBO(LANG_FRENCH, "``", 2, "a", "à");
    EXPECT_COMBO(LANG_FRENCH, "``", 2, "e", "è");
    EXPECT_COMBO(LANG_FRENCH, "``", 2, "i", "ì");
    EXPECT_COMBO(LANG_FRENCH, "``", 2, "o", "ò");
    EXPECT_COMBO(LANG_FRENCH, "``", 2, "u", "ù");
    EXPECT_COMBO(LANG_FRENCH, "``", 2, "E", "È");
    EXPECT_COMBO(LANG_FRENCH, "tr``", 4, "e", "è");
    EXPECT_COMBO(LANG_FRENCH, "é``", 3, "e", "è");
    /* Three `: the first stays, the last two make the grave */
    EXPECT_COMBO(LANG_FRENCH, "```", 3, "e", "è");
    /* Not a vowel */
    EXPECT_COMBO(LANG_FRENCH, "`", 1, "c", "");
    EXPECT_COMBO(LANG_FRENCH, "`", 1, " ", "");
}

/* How many ` the combo consumes */
static void test_ticks(void)
{
    uint32_t t = 0;
    EXPECT(lang_accent_combo(LANG_FRENCH, "`", 1, "e", &t) && t == 1,  "` e eats 1");
    EXPECT(lang_accent_combo(LANG_FRENCH, "``", 2, "e", &t) && t == 2, "`` e eats 2");
    EXPECT(lang_accent_combo(LANG_FRENCH, "``", 2, "a", &t) && t == 2, "`` a eats 2");
    EXPECT(lang_accent_combo(LANG_FRENCH, "`", 1, "a", &t) && t == 1,  "` a eats 1");
    EXPECT(lang_accent_combo(LANG_FRENCH, "``", 2, "c", &t) == NULL,   "`` c: no combo");
    /* Greek has no grave: `` α accents with the last ` and leaves the first */
    EXPECT(lang_accent_combo(LANG_GREEK, "``", 2, "α", &t) && t == 1,  "`` α eats 1");
    EXPECT(lang_accent_combo(LANG_FRENCH, "`", 1, "e", NULL) == NULL,  "NULL ticks");
}

static void test_no_combo(void)
{
    /* No language: nothing */
    EXPECT_COMBO(LANG_NONE, "`", 1, "a", "");
    /* ` not right before the cursor */
    EXPECT_COMBO(LANG_FRENCH, "`x", 2, "e", "");
    EXPECT_COMBO(LANG_FRENCH, "x`", 1, "e", "");   /* cursor moved left of the ` */
    EXPECT_COMBO(LANG_FRENCH, "",   0, "e", "");
    EXPECT_COMBO(LANG_FRENCH, "`",  0, "e", "");
    /* More than one character inserted (paste / emoji) */
    EXPECT_COMBO(LANG_FRENCH, "`",  1, "ee", "");
    EXPECT_COMBO(LANG_FRENCH, "`",  1, "", "");
    /* An accented result is not itself a vowel to accent again */
    EXPECT_COMBO(LANG_FRENCH, "`",  1, "è", "");
    EXPECT_COMBO(LANG_FRENCH, "`",  1, "é", "");
    EXPECT_COMBO(LANG_GREEK,  "`",  1, "ά", "");
    /* NULLs */
    uint32_t t;
    EXPECT(lang_accent_combo(LANG_FRENCH, NULL, 1, "e", &t) == NULL, "NULL text");
    EXPECT(lang_accent_combo(LANG_FRENCH, "`", 1, NULL, &t) == NULL, "NULL inserted");
}

int main(void)
{
    printf("=== lang unit tests ===\n");
    test_names();
    test_greek();
    test_french();
    test_ticks();
    test_no_combo();
    printf("\n%d tests, %d failures\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
