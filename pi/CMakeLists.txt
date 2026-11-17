cmake_minimum_required(VERSION 3.16)
project(chatbox C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build backend option: SDL2 (default, for Docker/dev) or FB (for Raspberry Pi)
set(CHATBOX_BACKEND "SDL2" CACHE STRING "Display backend: SDL2 or FB")

# Profiling: adds main-loop watchdog, section timers, and enables LVGL
# LV_USE_PERF_MONITOR / LV_USE_MEM_MONITOR overlays via lv_conf.h.
option(CHATBOX_PROFILE "Enable on-device profiling instrumentation" OFF)
# A bar along the bottom of every screen showing which keys do what
# (docs/design.md). Off: apps get the full 800 px.
option(CHATBOX_KEY_HINTS "Show the key-hint bar" OFF)
if(CHATBOX_KEY_HINTS)
    add_compile_definitions(CHATBOX_KEY_HINTS)
endif()

message(STATUS "Chatbox backend: ${CHATBOX_BACKEND}")
message(STATUS "Chatbox profiling: ${CHATBOX_PROFILE}")

# -------------------------------------------------------------------------
# LVGL configuration
# -------------------------------------------------------------------------
# Apply local patches to LVGL (Menu key + Esc fix for keyboard drivers)
set(LVGL_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/patches/lvgl-keyboard-drivers.patch")
if(EXISTS "${LVGL_PATCH}")
    execute_process(
        COMMAND git apply --check "${LVGL_PATCH}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib/lvgl"
        RESULT_VARIABLE PATCH_CHECK_RESULT
        OUTPUT_QUIET ERROR_QUIET
    )
    if(PATCH_CHECK_RESULT EQUAL 0)
        execute_process(
            COMMAND git apply "${LVGL_PATCH}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib/lvgl"
        )
        message(STATUS "Applied LVGL keyboard driver patches")
    else()
        message(STATUS "LVGL patches already applied or not needed")
    endif()
endif()

# Place lv_conf.h in pi/conf/ — tell LVGL via LV_CONF_PATH
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/conf/lv_conf.h" CACHE STRING "")
set(LV_CONF_INCLUDE_SIMPLE OFF CACHE BOOL "")
set(LV_LVGL_H_INCLUDE_SIMPLE ON CACHE BOOL "")
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "")
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "")

add_subdirectory(lib/lvgl)
add_subdirectory(lib/cJSON)

# -------------------------------------------------------------------------
# Backend-specific definitions and libraries
# -------------------------------------------------------------------------
set(CHATBOX_BACKEND_DEFS "")
set(CHATBOX_BACKEND_LIBS "")

if(CHATBOX_BACKEND STREQUAL "SDL2")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    list(APPEND CHATBOX_BACKEND_DEFS CHATBOX_USE_SDL)
    list(APPEND CHATBOX_BACKEND_LIBS ${SDL2_LIBRARIES})
    include_directories(${SDL2_INCLUDE_DIRS})
    link_directories(${SDL2_LIBRARY_DIRS})
    # Pass the backend define to LVGL so lv_conf.h enables LV_USE_SDL
    target_compile_definitions(lvgl PUBLIC CHATBOX_USE_SDL)
    target_include_directories(lvgl PUBLIC ${SDL2_INCLUDE_DIRS})
elseif(CHATBOX_BACKEND STREQUAL "FB")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBINPUT REQUIRED libinput)
    pkg_check_modules(LIBEVDEV REQUIRED libevdev)
    pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
    list(APPEND CHATBOX_BACKEND_DEFS CHATBOX_USE_FB)
    list(APPEND CHATBOX_BACKEND_LIBS
        ${LIBINPUT_LIBRARIES}
        ${LIBEVDEV_LIBRARIES}
        ${XKBCOMMON_LIBRARIES}
    )
    include_directories(
        ${LIBINPUT_INCLUDE_DIRS}
        ${LIBEVDEV_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS}
    )
    # Pass the backend define to LVGL so lv_conf.h enables LV_USE_LINUX_FBDEV
    target_compile_definitions(lvgl PUBLIC CHATBOX_USE_FB)
else()
    message(FATAL_ERROR "Unknown CHATBOX_BACKEND: ${CHATBOX_BACKEND}. Use SDL2 or FB.")
endif()

# -------------------------------------------------------------------------
# Find libcurl (used by matrix_api)
# -------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED libcurl)
include_directories(${CURL_INCLUDE_DIRS})

# -------------------------------------------------------------------------
# Include paths
# -------------------------------------------------------------------------
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/conf
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/lvgl
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/cJSON
)

# -------------------------------------------------------------------------
# SQLite (music library reader + indexer). Optional: without it the kiosk
# still builds, with library_stub.c (music shows "No music yet"), and the
# indexer and its tests are skipped.
# -------------------------------------------------------------------------
pkg_check_modules(SQLITE3 sqlite3)
if(NOT SQLITE3_FOUND)
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        set(SQLITE3_FOUND TRUE)
        set(SQLITE3_INCLUDE_DIRS ${SQLite3_INCLUDE_DIRS})
        set(SQLITE3_LIBRARIES ${SQLite3_LIBRARIES})
        set(SQLITE3_LIBRARY_DIRS "")
    endif()
endif()

if(SQLITE3_FOUND)
    set(MUSIC_LIBRARY_SOURCES src/library.c)
else()
    set(MUSIC_LIBRARY_SOURCES src/library_stub.c)
    message(WARNING "sqlite3 not found: chatbox is built with a stub music library "
                    "(music always shows \"No music yet\"); chatbox-indexer, "
                    "test_indexer and test_library are not built (install libsqlite3-dev)")
endif()

# -------------------------------------------------------------------------
# libjpeg / libpng (photos, docs/photos.md). Optional: without one, that
# format never decodes (the UI shows its placeholder); photos are still
# saved from chat.
# -------------------------------------------------------------------------
pkg_check_modules(JPEG libjpeg)
pkg_check_modules(PNG libpng)
set(PHOTO_DECODE_DEFS "")
set(PHOTO_DECODE_INCS "")
set(PHOTO_DECODE_LIBDIRS "")
set(PHOTO_DECODE_LIBS "")
if(JPEG_FOUND)
    list(APPEND PHOTO_DECODE_DEFS CHATBOX_HAVE_LIBJPEG)
    list(APPEND PHOTO_DECODE_INCS ${JPEG_INCLUDE_DIRS})
    list(APPEND PHOTO_DECODE_LIBDIRS ${JPEG_LIBRARY_DIRS})
    list(APPEND PHOTO_DECODE_LIBS ${JPEG_LIBRARIES})
else()
    message(WARNING "libjpeg not found: JPEG photos will not show (install libjpeg-dev)")
endif()
if(PNG_FOUND)
    list(APPEND PHOTO_DECODE_DEFS CHATBOX_HAVE_LIBPNG)
    list(APPEND PHOTO_DECODE_INCS ${PNG_INCLUDE_DIRS})
    list(APPEND PHOTO_DECODE_LIBDIRS ${PNG_LIBRARY_DIRS})
    list(APPEND PHOTO_DECODE_LIBS ${PNG_LIBRARIES})
else()
    message(WARNING "libpng not found: PNG photos will not show (install libpng-dev)")
endif()

# A target that decodes photos (photo_decode.c).
function(chatbox_use_photo_decode tgt)
    target_compile_definitions(${tgt} PRIVATE ${PHOTO_DECODE_DEFS})
    target_include_directories(${tgt} PRIVATE ${PHOTO_DECODE_INCS})
    target_link_directories(${tgt} PRIVATE ${PHOTO_DECODE_LIBDIRS})
    target_link_libraries(${tgt} ${PHOTO_DECODE_LIBS})
endfunction()

# Photos: the library, the decoder, the chat inbox and the UI. Shared by
# chatbox and test_keyboard.
set(PHOTO_SOURCES
    src/photo_lib.c
    src/photo_decode.c
    src/photo_inbox.c
    src/ui_photo_img.c
    src/ui_photos.c
    src/app_photos.c
)

# Music UI + player, shared by chatbox and test_keyboard.
set(MUSIC_SOURCES
    src/ui_music.c
    src/player.c
    src/text_nfc.c
    ${MUSIC_LIBRARY_SOURCES}
)

# The shared look (ui_theme.h) and its fonts, for chatbox and test_keyboard.
set(THEME_SOURCES
    src/ui_theme.c
    src/ui_hints.c
    src/font_chat_28.c
    src/font_chat_36.c
    src/font_chat_48.c
    src/font_title_28.c
    src/font_title_36.c
    src/font_title_48.c
    src/font_icon_120.c
    src/font_count_96.c
)

# -------------------------------------------------------------------------
# chatbox main binary
# -------------------------------------------------------------------------
add_executable(chatbox
    src/main.c
    src/shell.c
    src/switcher.c
    src/app_chat.c
    src/app_music.c
    ${MUSIC_SOURCES}
    ${PHOTO_SOURCES}
    src/key_filter.c
    src/config.c
    src/lang.c
    src/matrix_api.c
    src/msg_cache.c
    src/sync_merge.c
    src/ui_sidebar.c
    src/ui_chat.c
    src/display_sleep.c
    src/avatars.c
    src/emoji_font.c
    ${THEME_SOURCES}
)

target_compile_definitions(chatbox PRIVATE
    ${CHATBOX_BACKEND_DEFS}
)

if(CHATBOX_PROFILE)
    target_compile_definitions(chatbox PRIVATE CHATBOX_PROFILE)
    # lv_conf.h gates LV_USE_PERF_MONITOR / LV_USE_MEM_MONITOR / LV_LOG_LEVEL
    # on CHATBOX_PROFILE, so LVGL itself must see the define.
    target_compile_definitions(lvgl PUBLIC CHATBOX_PROFILE)
endif()

target_compile_options(chatbox PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

target_link_libraries(chatbox
    lvgl
    cjson
    ${CURL_LIBRARIES}
    ${CHATBOX_BACKEND_LIBS}
    m
    pthread
)
chatbox_use_photo_decode(chatbox)
if(SQLITE3_FOUND)
    target_include_directories(chatbox PRIVATE ${SQLITE3_INCLUDE_DIRS})
    target_link_directories(chatbox PRIVATE ${SQLITE3_LIBRARY_DIRS})
    target_link_libraries(chatbox ${SQLITE3_LIBRARIES})
endif()

# -------------------------------------------------------------------------
# test_config binary
# -------------------------------------------------------------------------
add_executable(test_config
    src/test_config.c
    src/config.c
    src/lang.c
)

target_compile_options(test_config PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# -------------------------------------------------------------------------
# test_api binary
# -------------------------------------------------------------------------
add_executable(test_api
    src/test_api.c
    src/matrix_api.c
)

target_compile_options(test_api PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

target_link_libraries(test_api
    cjson
    ${CURL_LIBRARIES}
    m
)

# -------------------------------------------------------------------------
# test_async binary — exercises the curl_multi-driven dispatcher
# -------------------------------------------------------------------------
add_executable(test_async
    src/test_async.c
    src/matrix_api.c
)

target_compile_options(test_async PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

target_link_libraries(test_async
    cjson
    ${CURL_LIBRARIES}
    m
)

# -------------------------------------------------------------------------
# test_msg_cache binary
# -------------------------------------------------------------------------
add_executable(test_msg_cache
    src/test_msg_cache.c
    src/msg_cache.c
)

target_compile_definitions(test_msg_cache PRIVATE CHATBOX_TESTING)

target_compile_options(test_msg_cache PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

target_link_libraries(test_msg_cache
    cjson
    m
)

# -------------------------------------------------------------------------
# test_sync_merge binary — /sync into the rooms' timelines (pure C)
# -------------------------------------------------------------------------
add_executable(test_sync_merge
    src/test_sync_merge.c
    src/sync_merge.c
    src/msg_cache.c
)
target_compile_definitions(test_sync_merge PRIVATE CHATBOX_TESTING)
target_compile_options(test_sync_merge PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_sync_merge cjson m)

# -------------------------------------------------------------------------
# test_photos — photo library, decoder, chat inbox (pure C + cJSON + curl
# for matrix_mxc_parse; no LVGL). It writes its test JPEGs / PNGs with the
# same libraries.
# -------------------------------------------------------------------------
add_executable(test_photos
    src/test_photos.c
    src/test_photo_fixture.c
    src/photo_lib.c
    src/photo_decode.c
    src/photo_inbox.c
    src/matrix_api.c
)
target_compile_definitions(test_photos PRIVATE CHATBOX_TESTING)
target_compile_options(test_photos PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_photos cjson ${CURL_LIBRARIES} m)
chatbox_use_photo_decode(test_photos)

# -------------------------------------------------------------------------
# test_key_filter binary — one-shot key filter (pure C, no LVGL)
# -------------------------------------------------------------------------
add_executable(test_key_filter
    src/test_key_filter.c
    src/key_filter.c
)

target_compile_options(test_key_filter PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# -------------------------------------------------------------------------
# test_lang binary — per-contact language + accent combo (pure C, no LVGL)
# -------------------------------------------------------------------------
add_executable(test_lang
    src/test_lang.c
    src/lang.c
)

target_compile_options(test_lang PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# -------------------------------------------------------------------------
# test_player — the player core / mpv client (src/player.c, docs/player-spec.md
# §9) against an in-process fake mpv, plus test_fake_mpv, a stand-in mpv
# binary for the one test that spawns a real process. Pure C + cJSON: no
# LVGL, no SQLite (the queue takes ids and paths).
# -------------------------------------------------------------------------
add_executable(test_player
    src/test_player.c
    src/player.c
)
target_compile_definitions(test_player PRIVATE CHATBOX_TESTING)

add_executable(test_fake_mpv
    src/test_fake_mpv.c
)
add_dependencies(test_player test_fake_mpv)

foreach(tgt test_player test_fake_mpv)
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(${tgt} cjson)
endforeach()
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(test_player PRIVATE -Wno-format-truncation)
endif()

# -------------------------------------------------------------------------
# chatbox-indexer daemon + test_indexer (music library, docs/player-spec.md
# §2–§3), and test_library for the UI's read-only library reader
# (src/library.c, §7.7). C11, SQLite, cJSON; no LVGL. Without SQLite dev
# files these targets are skipped (see the warning above).
# -------------------------------------------------------------------------
if(SQLITE3_FOUND)
    set(INDEXER_SOURCES
        src/indexer_scan.c
        src/indexer_probe.c
        src/indexer_watch.c
    )

    add_executable(chatbox-indexer
        src/indexer_main.c
        src/config.c
        src/lang.c
        ${INDEXER_SOURCES}
    )

    add_executable(test_indexer
        src/test_indexer.c
        src/test_music_fixture.c
        ${INDEXER_SOURCES}
    )

    # The DB is built by the real indexer scan with test_indexer's stubs.
    add_executable(test_library
        src/test_library.c
        src/test_music_fixture.c
        src/library.c
        src/text_nfc.c
        ${INDEXER_SOURCES}
    )
    target_compile_definitions(test_library PRIVATE CHATBOX_TESTING)

    foreach(tgt chatbox-indexer test_indexer test_library)
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wno-unused-parameter)
        target_include_directories(${tgt} PRIVATE ${SQLITE3_INCLUDE_DIRS})
        target_link_directories(${tgt} PRIVATE ${SQLITE3_LIBRARY_DIRS})
        target_link_libraries(${tgt} cjson ${SQLITE3_LIBRARIES})
    endforeach()
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        # Fixture paths are snprintf'd into PATH_MAX buffers on purpose.
        target_compile_options(test_indexer PRIVATE -Wno-format-truncation)
        target_compile_options(test_library PRIVATE -Wno-format-truncation)
    endif()
endif()

# -------------------------------------------------------------------------
# test_keyboard binary (headless keyboard navigation tests)
# -------------------------------------------------------------------------
add_executable(test_keyboard
    src/test_keyboard.c
    src/shell.c
    src/switcher.c
    src/app_chat.c
    src/app_music.c
    ${MUSIC_SOURCES}
    ${PHOTO_SOURCES}
    src/key_filter.c
    src/config.c
    src/lang.c
    src/msg_cache.c
    src/matrix_api.c
    src/ui_sidebar.c
    src/ui_chat.c
    src/display_sleep.c
    src/avatars.c
    src/emoji_font.c
    ${THEME_SOURCES}
)

target_compile_definitions(test_keyboard PRIVATE CHATBOX_TESTING)
chatbox_use_photo_decode(test_keyboard)

target_compile_options(test_keyboard PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

target_link_libraries(test_keyboard
    lvgl
    cjson
    ${CURL_LIBRARIES}
    ${CHATBOX_BACKEND_LIBS}
    m
    pthread
)
# The music groups build a real library DB with the indexer's scan and the
# stub prober (test_music_fixture.c); without SQLite they are skipped.
if(SQLITE3_FOUND)
    target_sources(test_keyboard PRIVATE src/test_music_fixture.c src/test_photo_fixture.c ${INDEXER_SOURCES})
    target_compile_definitions(test_keyboard PRIVATE CHATBOX_HAVE_SQLITE)
    target_include_directories(test_keyboard PRIVATE ${SQLITE3_INCLUDE_DIRS})
    target_link_directories(test_keyboard PRIVATE ${SQLITE3_LIBRARY_DIRS})
    target_link_libraries(test_keyboard ${SQLITE3_LIBRARIES})
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_compile_options(test_keyboard PRIVATE -Wno-format-truncation)
    endif()
endif()
