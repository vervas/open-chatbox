# Architecture

Two binaries, both C11, built from `pi/`:

- **`chatbox`**, the kiosk: LVGL 9.2 on the Linux framebuffer (SDL2 on a dev machine), libcurl + cJSON for Matrix, mpv for audio, a read-only SQLite music library, libjpeg + libpng for photos.
- **`chatbox-indexer`**, a separate daemon with no LVGL. It scans `/srv/music` into `library.db` + art thumbnails ([music.md](music.md)).

The headers are the API reference; this page is the map. Non-obvious rules and gotchas live in [CLAUDE.md](../CLAUDE.md).

## Threading

All app code runs on LVGL's main thread: UI, Matrix I/O, the player. There are no worker threads of our own and no blocking calls in the loop. Work is driven by `lv_timer`s:

| Timer | Period | Does |
|---|---|---|
| `async_tick_cb` | 16 ms | drives `curl_multi` for the Matrix dispatcher; callbacks fire here |
| `player_tick_cb` | 16 ms | mpv JSON IPC, the play queue, respawning mpv |
| `poll_timer_cb` | 3 s | Matrix `/sync` (initial, then incremental via `next_batch`) once `whoami` has answered; retries `whoami` until then |
| `library_poll_cb` | 2 s | checks the library DB's generation; music reloads on a change |
| `photo_poll_cb` | 2 s | checks the photo folder (its mtime; a full rescan every 20 s); Photos redraws on a change |
| `photo_inbox_tick_cb` | 250 ms | starts the next chat photo download, retries failed ones |
| photo loaders | 10–16 ms | decode one grid card / one chat bubble picture per tick, only while some wait |
| `kb_hotplug_cb` (Pi) | 2 s | rescans `/dev/input/event*` if the keyboard went away |

The one exception is LVGL's libinput driver on the Pi: it reads input on its own thread, under its own `event_lock`, and the shell reads the queued events from the main thread.

## The shell and the apps

`main.c` builds a **shell** ([shell.c](../pi/src/shell.c)) with three apps behind the `app_t` contract ([app.h](../pi/src/app.h)): **music** (`app_music.c`), **chat** (`app_chat.c`) and **photos** (`app_photos.c`). Each app has its own root object and LVGL focus group; only the visible app's group receives keys. **Esc** opens a full-screen switcher (`switcher.c`) with a live tile per app.

The shell hooks every keyboard's read callback, so it sees keys before LVGL does. It swallows keys while the display is locked, routes Esc (app first, then the switcher), and runs one-shot keys through a 450 ms filter (`key_filter.c`) so a held or auto-repeated key acts once. The design notes are at the top of `shell.c`; the key contract is [player-spec.md](player-spec.md) §5 and §8.

`display_sleep.c` dims to a lock overlay after `sleep_lock_secs` and powers the display off after `sleep_off_secs`; the waking key is swallowed. A lock override (`display_sleep_set_lock_override`) stretches the first step to `sleep_photo_viewer_mins` while `app_photos_viewer_showing()` says the photo viewer is on screen.

## Chat

```
sidebar (ui_sidebar.c)          chat panel (ui_chat.c)
 avatars, unread dots    ──►     header · bubbles · emoji picker · input bar
```

- **Panels.** Each contact's chat is its own panel. Up to 3 are kept in an LRU cache in `main.c`, so switching back doesn't refetch; the history comes from `/sync`: the initial sync seeds each contact's room in `msg_cache.c` (`sync_merge.c`, dedup by `event_id`) and a panel renders from there, showing "Loading…" until the initial sync has succeeded.
- **Sync.** `/sync` every 3 s updates the open chat and sets unread dots on the others.
- **Sending is optimistic.** The bubble appears at once; it turns red if not confirmed within 60 s and back to normal if it is delivered later.
- **Text.** Bubbles and the input use Didact Gothic bitmaps (`font_chat_*`: Latin-1, Latin Extended-A, Greek, punctuation, €; DejaVu Sans fills the few glyphs it lacks) behind an LVGL imgfont of Twemoji PNGs (`emoji_font.c`). Titles use M PLUS Rounded 1c (`font_title_*`). Decomposed (NFD) text is composed just before display (`text_nfc.c`).

### The Matrix client

[matrix_api.c](../pi/src/matrix_api.c) has two layers. The synchronous `matrix_*` calls are used only by `test_api`. The app uses the **async dispatcher** on top of `curl_multi`, with one main request in flight at a time (plus, at most, one photo download beside it):

- a **main FIFO queue** (8 slots) for whoami (first, at boot) and sync;
- a **media slot** for chat photo downloads, with its own curl handle on the same `curl_multi`, so a photo that takes minutes never holds up sync or sends (binary: `matrix_async_get_media` hands the callback bytes, not cJSON; one at a time, a second is told `MATRIX_ERR_QUEUE`; capped at 25 MB, a stalled transfer gives up after 20 s);
- a **send outbox** (32 slots) for outgoing messages. Transient failures (transport errors, HTTP 408/429/5xx) retry forever with capped backoff (2/8/20/40/60 s, then every 60 s); a permanent rejection (other 4xx) drops that message so it can't block the ones behind it;
- `matrix_async_cancel(type)` cancels pending requests (and the media slot's).

Requests carry a Bearer token. A send is `PUT /rooms/{id}/send/m.room.message/{txnId}`; the transaction ID (`chatbox_<counter>_<time>`) is fixed when the message is queued and reused by every retry, so a retry can't post it twice.

## Music

`ui_music.c` is the UI (the album grid, an album's page, Now Playing, volume overlay). It reads the library through `library.c` (read-only SQLite) and drives `player.c` through an ops table, so tests can swap in a fake player. `player.c` runs mpv as a child process and talks to it over a JSON IPC socket. The indexer is a separate process that writes the DB. Services, paths and users: [music.md](music.md). Design decisions: [player-spec.md](player-spec.md).

## Photos

Chat hands every `m.image` to `photo_inbox.c`, which downloads it through the dispatcher (`matrix_async_get_media`, binary, one at a time) and saves it atomically into `photos_dir`; `photo_lib.c` lists that folder newest first; `ui_photos.c` is the grid, viewer and delete confirm. Photos are decoded at the size they are drawn (`photo_decode.c`, libjpeg DCT scaling + EXIF orientation) into malloc'd memory and wrapped as LVGL images (`ui_photo_img.c`), so LVGL's heap and image cache never hold a full-size photo. Details: [photos.md](photos.md).

## Keyboard input

- The Pi uses libinput + xkbcommon. Our patch to LVGL ([development.md](development.md#the-lvgl-patch)) maps Win/Menu → `0x04` (emoji panel / Now Playing), PgUp/PgDn → `0x05`/`0x06` (emoji page / volume) and adds Esc.
- SDL builds get OS key repeats as fake press/release pairs; `main.c` watches real SDL key events so the one-shot filter still sees real releases.
- Only ↑ ↓ ← → and Backspace can be held on the Pi; everything else releases at once.
- Delete reaches LVGL as `LV_KEY_DEL` on both drivers without a patch (SDL maps it; libinput's xkb fallback gives the keysym's character, 0x7F). Photos uses it.

## Source map

| Area | Files |
|---|---|
| Entry, loop, chat logic | `main.c` |
| Shell, apps, switcher, key filter | `shell.{c,h}`, `app.h`, `app_chat.c`, `app_music.c`, `switcher.{c,h}`, `key_filter.{c,h}` |
| Chat UI | `ui_chat.{c,h}`, `ui_sidebar.{c,h}`, `emoji_font.{c,h}`, `emoji_pages.inc`, `avatars.{c,h}`, `font_chat_*.c`, `text_nfc.{c,h}` |
| Matrix | `matrix_api.{c,h}`, `msg_cache.{c,h}` |
| Music | `ui_music.{c,h}`, `player.{c,h}`, `library.{c,h}` (`library_stub.c` without SQLite), `library_schema.h` |
| Photos | `ui_photos.{c,h}`, `app_photos.c`, `ui_photo_img.{c,h}`, `photo_lib.{c,h}`, `photo_decode.{c,h}`, `photo_inbox.{c,h}` |
| Indexer | `indexer_main.c`, `indexer_scan.{c,h}`, `indexer_probe.{c,h}`, `indexer_watch.{c,h}` |
| Config, sleep, profiling | `config.{c,h}`, `display_sleep.{c,h}`, `prof.h` |
| Tests | `test_*.c` (one binary each) |

