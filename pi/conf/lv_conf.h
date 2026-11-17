/**
 * @file lv_conf.h
 * Chatbox LVGL configuration for v9.2.x
 * Supports two backends:
 *   SDL2  (Docker/dev, default) — enabled by CHATBOX_USE_SDL
 *   FB    (Raspberry Pi)        — enabled by CHATBOX_USE_FB
 */

/* clang-format off */
#if 1 /*Set it to "1" to enable content*/

#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COLOR SETTINGS
 *====================*/

/* 32-bit color (XRGB8888) for best fidelity on SDL2 and fbdev */
#define LV_COLOR_DEPTH 32

/*=========================
   STDLIB WRAPPER SETTINGS
 *=========================*/
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

#define LV_STDINT_INCLUDE       <stdint.h>
#define LV_STDDEF_INCLUDE       <stddef.h>
#define LV_STDBOOL_INCLUDE      <stdbool.h>
#define LV_INTTYPES_INCLUDE     <inttypes.h>
#define LV_FLOAT_INCLUDE        <float.h>
#define LV_MATH_INCLUDE         <math.h>

/*====================
   MEMORY SETTINGS
 *====================*/
/* 16 MB heap for LVGL. Chat alone fits in 1 MB (LRU chat panels); the
 * music app's artwork needs more: decoded images live in this heap, and
 * decoding one 512 px Now Playing PNG peaks at ~3 MB (file bytes + the
 * inflated scanlines + the 1 MB ARGB8888 result), on top of the 3 MB image
 * cache below. The rest is headroom against fragmentation (TLSF, no
 * compaction). A static BSS pool: pages never touched cost no RAM. */
#define LV_MEM_SIZE            (16U * 1024U * 1024U)
#define LV_MEM_POOL_INCLUDE    <stdlib.h>
#define LV_MEM_POOL_ALLOC      malloc
#define LV_MEM_POOL_FREE       free

/*====================
   HAL SETTINGS
 *====================*/
#define LV_TICK_CUSTOM          1
#if LV_TICK_CUSTOM
    #define LV_TICK_CUSTOM_INCLUDE  <stdint.h>
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR ((uint32_t)(lv_tick_get()))
#endif

#define LV_DPI_DEF              150

/*====================
   DRAW SETTINGS
 *====================*/
#define LV_DRAW_BUF_STRIDE_ALIGN    1
#define LV_DRAW_BUF_ALIGN           4

#define LV_USE_DRAW_SW          1
#if LV_USE_DRAW_SW
    #define LV_DRAW_SW_DRAW_UNIT_CNT    1
    #define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
    #define LV_DRAW_SW_CIRCLE_CACHE_CNT 4
    #define LV_DRAW_SW_COMPLEX          1
    #define LV_USE_DRAW_SW_ASM          LV_DRAW_SW_ASM_NONE
    /* Linear gradients at any angle: the music app's fallback covers are a
     * diagonal two-hue gradient (player spec §7.6). Only objects that set a
     * LINEAR gradient pay for it (per-pixel, 96x96 in lists). */
    #define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 1
#endif

/*====================
   DISPLAY SETTINGS
 *====================*/
#define LV_DISPLAY_DEF_REFR_PERIOD  16      /* 16ms = ~60 FPS, matches panel */
#define LV_DISPLAY_DEF_WIDTH        1280
#define LV_DISPLAY_DEF_HEIGHT       800
#define LV_USE_DIRECT_COLORS        0

/*====================
   INPUT DEVICE SETTINGS
 *====================*/
#define LV_INDEV_DEF_READ_PERIOD    16
#define LV_INDEV_DEF_SCROLL_LIMIT   10
#define LV_INDEV_DEF_SCROLL_THROW   10
#define LV_INDEV_DEF_LONG_PRESS_TIME 400
#define LV_INDEV_DEF_LONG_PRESS_REP_TIME 200
#define LV_INDEV_DEF_GESTURE_LIMIT  50
#define LV_INDEV_DEF_GESTURE_MIN_VELOCITY 3

/*====================
   RENDERING SETTINGS
 *====================*/
#define LV_LAYER_MAX_MEMORY_USAGE   150

/* Image cache (bytes of decoded image data, in the LVGL heap). The default
 * 0 means a PNG file image is decoded again on EVERY redraw of its area: a
 * list scroll or a selection move would re-inflate every visible cover.
 * 5 MB holds a screen of album cards (12 × 256x256 ARGB8888 = 3 MB), the
 * Now Playing cover (512x512 = 1 MB), the strip's and songs' 96 px
 * thumbnails (36 KB each) and the chat's emoji PNGs; least recently used
 * entries are evicted. The music app drops the cache when
 * the library generation changes (an art id can be reused for new bytes). */
#define LV_CACHE_DEF_SIZE               (5U * 1024U * 1024U)
/* Image headers (size/format) of PNG files, so lv_image_set_src doesn't
 * reopen the file for every row it binds. */
#define LV_IMAGE_HEADER_CACHE_DEF_CNT   64

/*====================
   LAYOUT
 *====================*/
#define LV_USE_FLEX     1
#define LV_USE_GRID     1

/*====================
   BUILT-IN WIDGETS
 *====================*/
#define LV_USE_ANIMIMG      0
#define LV_USE_ARC          1
#define LV_USE_BAR          1
#define LV_USE_BUTTON       1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_CALENDAR     0
#define LV_USE_CANVAS       0
#define LV_USE_CHART        0
#define LV_USE_CHECKBOX     1
#define LV_USE_DROPDOWN     1
#define LV_USE_IMAGE        1
#define LV_USE_IMAGEBUTTON  0
#define LV_USE_KEYBOARD     1
#define LV_USE_LABEL        1
#define LV_USE_LED          0
#define LV_USE_LINE         1
#define LV_USE_LIST         1
#define LV_USE_MENU         0
#define LV_USE_MSGBOX       0
#define LV_USE_ROLLER       1
#define LV_USE_SCALE        0
#define LV_USE_SLIDER       1
#define LV_USE_SPAN         0
#define LV_USE_SPINBOX      0
#define LV_USE_SPINNER      1
#define LV_USE_SWITCH       1
#define LV_USE_TABLE        0
#define LV_USE_TABVIEW      0
#define LV_USE_TEXTAREA     1
#define LV_USE_TILEVIEW     0
#define LV_USE_WIN          0

/*====================
   THEMES
 *====================*/
#define LV_USE_THEME_DEFAULT    1
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK             0
    #define LV_THEME_DEFAULT_GROW             1
    #define LV_THEME_DEFAULT_TRANSITION_TIME  80
#endif
#define LV_USE_THEME_SIMPLE     1
#define LV_USE_THEME_MONO       0

/*====================
   FONTS
 *====================*/
/* Enable built-in Montserrat fonts */
#define LV_FONT_MONTSERRAT_8     0
#define LV_FONT_MONTSERRAT_10    0
#define LV_FONT_MONTSERRAT_12    0
#define LV_FONT_MONTSERRAT_14    1
#define LV_FONT_MONTSERRAT_16    0
#define LV_FONT_MONTSERRAT_18    0
#define LV_FONT_MONTSERRAT_20    0
#define LV_FONT_MONTSERRAT_22    0
#define LV_FONT_MONTSERRAT_24    1
#define LV_FONT_MONTSERRAT_26    0
#define LV_FONT_MONTSERRAT_28    0
#define LV_FONT_MONTSERRAT_30    0
#define LV_FONT_MONTSERRAT_32    1
#define LV_FONT_MONTSERRAT_34    0
#define LV_FONT_MONTSERRAT_36    1
#define LV_FONT_MONTSERRAT_38    0
#define LV_FONT_MONTSERRAT_40    0
#define LV_FONT_MONTSERRAT_42    0
#define LV_FONT_MONTSERRAT_44    0
#define LV_FONT_MONTSERRAT_46    0
#define LV_FONT_MONTSERRAT_48    1

#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_FONT_FMT_TXT_LARGE   0
#define LV_USE_FONT_COMPRESSED  0
#define LV_USE_FONT_PLACEHOLDER 1

/*====================
   ANIMATIONS
 *====================*/
#define LV_USE_ANIM     1

/*====================
   OTHERS
 *====================*/
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

#define LV_USE_LOG          1
#ifdef CHATBOX_PROFILE
#define LV_LOG_LEVEL        LV_LOG_LEVEL_INFO
#else
#define LV_LOG_LEVEL        LV_LOG_LEVEL_WARN
#endif
#define LV_LOG_PRINTF       1
#define LV_LOG_USE_TIMESTAMP 0
#define LV_LOG_USE_FILE_LINE 1

#define LV_USE_REFR_DEBUG           0
#ifdef CHATBOX_PROFILE
#define LV_USE_PERF_MONITOR         1
#define LV_USE_MEM_MONITOR          1
#else
#define LV_USE_PERF_MONITOR         0
#define LV_USE_MEM_MONITOR          0
#endif
#define LV_USE_STDLIB_MEM_USED      0

/*====================
   DEVICES
 *====================*/

/* SDL2 display driver — for Docker/dev builds */
#ifdef CHATBOX_USE_SDL
    #define LV_USE_SDL              1
    #if LV_USE_SDL
        #define LV_SDL_INCLUDE_PATH     <SDL2/SDL.h>
        #define LV_SDL_RENDER_MODE      LV_DISPLAY_RENDER_MODE_DIRECT
        #define LV_SDL_BUF_COUNT        1
        #define LV_SDL_ACCELERATED      1
        #define LV_SDL_FULLSCREEN       0
        #define LV_SDL_DIRECT_EXIT      1
        #define LV_SDL_MOUSEWHEEL_MODE  LV_SDL_MOUSEWHEEL_MODE_ENCODER
    #endif
#else
    #define LV_USE_SDL              0
#endif

/* Linux framebuffer driver — for Raspberry Pi builds */
#ifdef CHATBOX_USE_FB
    #define LV_USE_LINUX_FBDEV      1
    #if LV_USE_LINUX_FBDEV
        #define LV_LINUX_FBDEV_BSD           0
        #define LV_LINUX_FBDEV_RENDER_MODE   LV_DISPLAY_RENDER_MODE_PARTIAL
        #define LV_LINUX_FBDEV_BUFFER_COUNT  0
        #define LV_LINUX_FBDEV_BUFFER_SIZE   40
    #endif
#else
    #define LV_USE_LINUX_FBDEV      0
#endif

/* libinput driver — for Raspberry Pi keyboard/touch */
#ifdef CHATBOX_USE_FB
    #define LV_USE_LIBINPUT         1
    #if LV_USE_LIBINPUT
        #define LV_LIBINPUT_BSD             0
        #define LV_LIBINPUT_XKB             1
    #endif
#else
    #define LV_USE_LIBINPUT         0
#endif

/* evdev driver — disabled, using SDL or libinput */
#define LV_USE_EVDEV            0

/* DRM/KMS driver — disabled, using fbdev or SDL */
#define LV_USE_LINUX_DRM        0

/*====================
   FILE SYSTEM
 *====================*/
#define LV_USE_FS_POSIX  1
#if LV_USE_FS_POSIX
    #define LV_FS_POSIX_LETTER  'A'
    #define LV_FS_POSIX_PATH    ""
    #define LV_FS_POSIX_CACHE_SIZE 0
#endif

/*====================
   MISC
 *====================*/
#define LV_USE_OBSERVER         1
#define LV_USE_SNAPSHOT         0
#ifdef CHATBOX_PROFILE
/* SYSMON gates LV_USE_PERF_MONITOR / LV_USE_MEM_MONITOR — without it,
 * lv_conf_internal.h forces both to 0 regardless of what we set above. */
#define LV_USE_SYSMON           1
#else
#define LV_USE_SYSMON           0
#endif
#define LV_USE_PROFILER         0
#define LV_USE_FFMPEG           0
#define LV_USE_FREETYPE         0
#define LV_USE_LODEPNG          1
#define LV_USE_LIBPNG           0
#define LV_USE_TJPGD            0
#define LV_USE_LIBJPEG_TURBO    0
#define LV_USE_BMP              0
#define LV_USE_GIF              0
#define LV_USE_QRCODE           0
#define LV_USE_BARCODE          0
#define LV_USE_LOTTIE           0
#define LV_USE_RLOTTIE          0
#define LV_USE_VECTOR_ANIM      0
#define LV_USE_FRAGMENT         0
#define LV_USE_IMGFONT          1
#define LV_USE_MONKEY           0
#define LV_USE_GRIDNAV          1
#define LV_USE_BUTTON_BIND      0
#define LV_USE_IME_PINYIN       0
#define LV_USE_FILE_EXPLORER    0
#define LV_USE_FONT_MANAGER     0
#define LV_USE_SVG              0
#define LV_USE_OBJ_ID           0
#define LV_USE_OBJ_ID_BUILTIN   0
#define LV_USE_OBJ_PROPERTY     0
#define LV_USE_OBJ_PROPERTY_NAME 0
#define LV_USE_STYLE_HELPER     0
#define LV_USE_XML              0
#define LV_USE_DEMO_WIDGETS     0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER  0
#define LV_USE_DEMO_BENCHMARK   0
#define LV_USE_DEMO_RENDER      0
#define LV_USE_DEMO_SCROLL      0
#define LV_USE_DEMO_MUSIC       0
#define LV_USE_DEMO_FLEX_LAYOUT 0
#define LV_USE_DEMO_MULTILANG   0
#define LV_USE_DEMO_TRANSFORM   0
#define LV_USE_DEMO_SCROLL_TEXT 0

#endif /* LV_CONF_H */

/* clang-format on */
#endif /* End of "Content enable" */
