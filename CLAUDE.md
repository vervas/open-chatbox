# CLAUDE.md — chatbox

Matrix messaging, music and photos kiosk for kids. C / LVGL on a Raspberry Pi (framebuffer), talks to a Matrix homeserver (Synapse behind a TLS reverse proxy). SDL2 build for desktop dev.

The README is the front page; the details live in `docs/` (building.md · configuration.md · pi.md: setup, deploy, ops · homeserver.md · music.md · photos.md · keyboard.md · development.md · architecture.md · player-spec.md · design.md). This file documents what's not obvious from reading code or those docs.

## Layout cheat sheet

- `pi/` — the firmware (C, LVGL, libcurl, cJSON). All real logic lives here.
- `pi/src/main.c` — entry point, LVGL loop, chat logic (show_chat, history, sync polling, panel cache); builds the shell
- `pi/src/shell.{c,h}` — app shell: app registry, per-app focus groups, keyboard read hook (Esc routing, key filter), app switching. Design notes at the top of `shell.c`
- `pi/src/app.h` — app contract (`app_t`); `app_chat.c` (chat's sidebar + content layout behind the contract), `app_music.c` (music's key sink + filter context, `app_music_configure`/`app_music_poll_library`), `app_photos.c` (Photos' key sink, `app_photos_configure`/`app_photos_poll_library`, `app_photos_viewer_showing` for the sleep override)
- `pi/src/ui_photos.{c,h}` — Photos UI (docs/photos.md, docs/design.md): the grid (4 across, 4:3 cards, newest first, virtualised card pool + a 32-entry decoded-thumbnail cache, one decoded per tick), the viewer (whole photo fitted to 1168×592, double-buffered), the delete confirm sticker (Enter deletes the file only, Esc keeps), "No photos yet", the switcher tile
- `pi/src/photo_lib.{c,h}` — the photo folder (`photos_dir`, `/srv/photos`): scan, newest first by mtime, poll (folder mtime + full rescan every 10 polls), delete. No database
- `pi/src/photo_decode.{c,h}` — JPEG (libjpeg, DCT scaling, EXIF orientation) / PNG (libpng) straight to the drawn size, BGR in malloc'd memory; `ui_photo_img.{c,h}` wraps that as an `lv_image_dsc_t` (RGB888). Photos never go through LVGL's decoders, heap or image cache. libjpeg/libpng are optional at build time (placeholders without)
- `pi/src/photo_inbox.{c,h}` — chat → photo folder: `m.image` events are downloaded (`matrix_async_get_media`, one at a time, retry with backoff), checked to be JPEG/PNG ≤ 25 MB, written to a hidden temp file then renamed to `YYYYMMDD-HHMMSS-<hash>.jpg|png`, deduped by event_id via `photos_dir/.chatbox-seen` (so a deleted photo never comes back). Photo bubbles are in `ui_chat.c`
- `pi/src/ui_music.{c,h}` — music UI (spec D13): the album grid (every album, `library_load_all_albums`, virtualised card pool) / an album's page (its songs, virtualised rows); Enter on a card opens the page, Enter on a song plays and opens Now Playing; mini strip, Now Playing, volume overlay, "No music yet", fallback covers, music's peek pill; drives the player through a `music_player_ops_t` table (`music_player_real_ops` = `player.h`; tests pass a fake)
- `pi/src/ui_theme.{c,h}` — the shared look ("sticker kit", `docs/design.md`): colour tokens, the three item states (`ui_item_style`, `ui_item_focus_states`), stickers, the peek pill. Apps don't define colours of their own. `ui_hints.{c,h}` — the optional key-hint bar (`-DCHATBOX_KEY_HINTS=ON`, off by default; the shell owns it, apps give `app_t.hints`)
- `pi/src/player.{c,h}` — mpv child + JSON IPC + the play queue (`player_tick` from a 16 ms `lv_timer` in `main.c`)
- `pi/src/library.{c,h}` — read-only SQLite library reader (`library_poll` every 2 s from `main.c`); `library_stub.c` replaces it when CMake finds no sqlite3 (music then always shows "No music yet")
- `pi/src/indexer_*.{c,h}`, `library_schema.h` — the `chatbox-indexer` daemon (separate binary, no LVGL)
- `pi/src/switcher.{c,h}` — full-screen app switcher overlay (Music, Chat, Photos tiles: square for two apps, upright 360×520 for three; fly-in/zoom/shake animations)
- `pi/src/key_filter.{c,h}` — one-shot key filter (450 ms window, player spec D2)
- `pi/src/matrix_api.{c,h}` — sync HTTP + async dispatcher (curl_multi)
- `pi/src/ui_chat.{c,h}` — chat panel, bubbles (text, and photo bubbles for `m.image`, decoded one per tick within an 8 MB budget: on-screen and newest first, the rest give their picture back), emoji picker, input bar
- `pi/src/ui_sidebar.{c,h}` — the contacts' shelf beside the open chat (`ui_sidebar_open` is the one way to open a contact's chat: the row, booting into the first)
- `pi/src/text_nfc.{c,h}` — composes decomposed (NFD) text just before display (chat bubbles, every name `library.c` returns); LVGL can't stack combining marks. Data stays raw (DB, paths, Matrix)
- `pi/src/font_chat_{28,36,48}.c` (reading text: Didact Gothic, DejaVu Sans for the few glyphs it lacks) and `font_title_{28,36,48}.c` (titles: M PLUS Rounded 1c ExtraBold, subset to Latin + Greek) — bitmaps from `pi/fonts/`; a character outside their ranges draws as a box. Regenerate all six with `make fonts` (`lv_font_conv@1.5.3`; 1.5.2 emits LVGL 8 code). Andika has no Greek: don't swap it in
- `pi/src/msg_cache.{c,h}` — per-room message cache (the rooms' timelines; dedup by `event_id`, 200 events per room)
- `pi/src/sync_merge.{c,h}` — folds a `/sync` response into msg_cache: the initial sync seeds each contact's timeline, incremental syncs append only new events
- `pi/src/lang.{c,h}` — per-contact typing language (`contact.N.lang = gr | fr`): the `` ` `` + vowel accent combo (applied in `ui_chat.c`'s `LV_EVENT_INSERT` hook) and the XKB layout `main.c` swaps into the libinput keyboard on `show_chat` (under the driver's `event_lock`: LVGL's libinput worker thread runs xkb)
- `pi/src/config.{c,h}` — INI parser for `/etc/chatbox.conf`
- `pi/src/display_sleep.{c,h}` — lock overlay after `sleep_lock_secs`, display off after `sleep_off_secs`; `display_sleep_set_lock_override` gives the Photos viewer `sleep_photo_viewer_mins` (20) instead, polled every check so leaving the viewer restores the normal timeout
- `pi/src/test_*.c` — unit tests (built as separate binaries)
- `pi/lib/{lvgl,cJSON}` — git submodules
- `pi/patches/` — patches applied to submodules at configure time
- `pi/conf/lv_conf.h` — LVGL config
- `homeserver/` — Synapse docker-compose, bootstrap and room setup scripts, example Caddy config
- `docker/` — build container + mock Matrix server (Flask)
- `deploy/` — Pi system setup, rsync deploy, systemd units (`chatbox.service`, `chatbox-indexer.service`), config template, `bt-speaker/` (BT reconnect user unit for `chatbox`)
- `test/` — e2e scripts (Docker)

## Build / test quick reference

```
make build            # native build (SDL2 on Mac/Linux, FB on Pi aarch64)
make run              # build + run
make deploy           # rsync + remote build + install on Pi (needs PI_HOST/PI_USER in .env)
make test             # keyboard navigation tests
docker-compose run build           # containerized build into pi/build-docker
docker-compose run test            # build + every unit test + e2e against the mock server
pi/build-native/test_keyboard test/fixtures/chatbox.conf
pi/build-native/test_async   # async dispatcher unit tests
pi/build-native/test_msg_cache
pi/build-native/test_sync_merge
pi/build-native/test_config test/fixtures/chatbox.conf
pi/build-native/test_key_filter  pi/build-native/test_lang  pi/build-native/test_player  pi/build-native/test_indexer  pi/build-native/test_library  pi/build-native/test_photos
```

`make build` uses `pi/build-native`; containers use `pi/build-docker`. Never point a container at `pi/build` or `pi/build-native`: they hold macOS CMake caches that Linux cmake rejects. The commands here use the standalone `docker-compose`; `docker compose` works the same. The Docker image has real `mpv`/`ffmpeg`, so `test_player`'s real-mpv suite and `test_indexer`'s real-ffprobe test run there (they skip where the tools are missing).

`test_keyboard`'s SQLite-only groups (music 21-30, chat 32-33's `chat_lang_*`, `chat_screen`, `chat_emoji_panel`, and Photos 34's `photos-*`, `switcher-three-apps`, `chat_photo_bubbles*`) also write PNG snapshots of the screens to `$TMPDIR/chatbox-snapshots/` (captured from the test display's flush callback) — look at them after UI changes.

A clean reconfigure is `rm -rf pi/build && cmake -S pi -B pi/build && cmake --build pi/build`. **Do this whenever you've pulled changes that touch `pi/patches/` or the lvgl submodule** — see the gotcha below.

## Conventions

- **Commits use gitmoji.** Prefix the subject with the right emoji (✨ feature, 🐛 fix, ♻️ refactor, 📝 docs, 🔧 config, 🎨 style/UI, ✅ tests, 🔥 removal, 🏗️ architecture, 🐎 perf).
- **Never run git commands in parallel.** Always chain sequentially with `&&`. The submodules trip `.git/index.lock` collisions otherwise. Prepend `sleep 3 &&` to git commands to dodge lingering locks from prior operations.
- **Single-threaded.** All our code runs on the LVGL main thread. No mutexes, no worker threads of our own (LVGL's libinput driver reads input on its own thread on the Pi). Async I/O comes from `curl_multi` driven by an `lv_timer` (`async_tick_cb`, 16ms cadence). Callbacks fire on the main thread.
- **No dynamic allocation in hot paths.** Static ring buffers (the queue, the outbox, msg_cache slots). `cJSON_Print*` and `strdup` of bodies are the exceptions.

## Async dispatcher architecture

The synchronous `matrix_*` calls are only used by `test_api`. The app routes everything through an async dispatcher in `pi/src/matrix_api.c` (overview in `docs/architecture.md`):

- **Main FIFO queue** — `MATRIX_QUEUE_DEPTH = 8`. Single in-flight request at a time. Holds whoami (queued first at boot) and sync polls.
- **Send outbox** — `SEND_OUTBOX_DEPTH = 32`, dedicated to outgoing messages. Drains into the main queue from `matrix_async_tick`, one in-flight at a time. **Transient failures (transport / HTTP error, incl. 408/429) retry FOREVER** at capped backoff (2/8/20/40/60s, then every 60s) — a send is never abandoned for a transient failure. **Permanent server rejection (4xx other than 408/429)** drops the entry with `MATRIX_ERR_REJECTED` so it can't block later sends behind it. `matrix_async_outbox_resume()` fast-tracks a retry (resets backoff to now) and runs even when the outbox is full. Treats a full main queue as backpressure (retry next tick), not a send failure. There is deliberately no `SEND_MAX_ATTEMPTS` cap and no stale-on-inbound drop — see the ordering-tradeoff comment above `SEND_OUTBOX_DEPTH` in `matrix_api.c`. **Reason this exists:** sends previously shared the 8-slot queue and could be silently dropped under contention with the optimistic bubble already on screen.
- **Media slot** — photo downloads (`matrix_async_get_media`) don't use the main queue: one slot with its own easy handle (`g_media_curl`) on the same `curl_multi`, so a photo that takes minutes never blocks `/sync` or sends. One download at a time; a second gets `MATRIX_ERR_QUEUE` (photo_inbox asks again 500 ms later).
- **Cancellation** — `matrix_async_cancel(type)` fires `MATRIX_ERR_CANCELLED` callbacks and frees per-slot resources. `matrix_cleanup` finalises the in-flight slot synchronously since no further ticks will run.

## LVGL patch hook gotcha

`pi/CMakeLists.txt` (around line 21) applies `pi/patches/lvgl-keyboard-drivers.patch` to the lvgl submodule via `git apply --check` then `git apply` at configure time. The patch remaps Win/Menu/Super → `0x04` (emoji toggle), PageUp/Down → `0x05`/`0x06`, and adds the Esc handler. Both the libinput (Pi) and SDL (desktop) keyboard drivers are touched.

Two failure modes you will hit:

1. **`git apply --check` fails silently.** When the submodule already has a partial / stale earlier version of the patch applied, the check returns nonzero, the else-branch logs `"LVGL patches already applied or not needed"`, and the build proceeds with the stale patch. The binary lacks recent additions (e.g. the Win-key mapping) — symptom: keys mapped in the patch silently do nothing, app otherwise normal.
2. **CMake doesn't reconfigure on patch change.** The patch isn't in `CMAKE_CONFIGURE_DEPENDS`, so `cmake --build` after a `git pull` won't re-run the apply.

**Workflow:** clean rebuild (`rm -rf pi/build && cmake -S pi -B pi/build && cmake --build pi/build`) after any pull that touches `pi/patches/` or after deploying from a different machine. If the firmware behaves like a stale lvgl binary, suggest the clean rebuild before chasing anything else.

The hook is intentionally simple — do not "harden" it without explicit go-ahead from the repo owner. The accepted workflow is the clean rebuild.

## Keyboard remapping (custom keycodes)

Defined in `pi/src/ui_chat.c` lines 37-39:

| Code | Source key | Action |
|---|---|---|
| `0x04` | Menu / Win / Super_L / Super_R | chat: toggle emoji panel. Music: toggle Now Playing ⇄ the album grid (one-shot; nothing loaded → bounce). Switcher, "No music yet": ignored |
| `0x05` | PageUp | chat: previous emoji page (only when panel is open). Music: volume +5 % (overlay) |
| `0x06` | PageDown | chat: next emoji page (only when panel is open). Music: volume −5 % (overlay) |
| `LV_KEY_ESC` | Esc | chat: close the emoji panel if open; music: Now Playing → the album page → the grid; photos: close the delete confirm, else the viewer → the grid; otherwise the shell opens the app switcher (Esc there closes it) |
| `LV_KEY_DEL` (127) | Delete | photos: delete the selected / open photo, after the confirm sticker (Enter = yes, Esc = no; one-shot). Chat: the textarea's forward delete. Not in the LVGL patch: SDL maps `SDLK_DELETE`, libinput's xkb fallback yields 0x7F |

Space, PgUp and PgDn are music keys only while music is visible (player spec D5); the full per-context key contract is `docs/player-spec.md` §8, and music's one-shot contexts come from `ui_music_key_ctx()`.

Handler is wired to the textarea, send button, and emoji panel ([ui_chat.c:389-390, 735](pi/src/ui_chat.c#L389)). The Win key only fires the toggle when one of those widgets has focus. Sidebar focus → no effect.

Esc never reaches LVGL on a real keyboard: the shell's read hook (`shell.c`) intercepts it, asks the visible app's `on_key` (chat → `ui_chat_close_emoji_panel()`, music → `ui_music_handle_esc()`, photos → `ui_photos_handle_esc()`), else toggles the switcher.

**Tab in chat = the next contact.** Tab from the text box moves the group's focus onto a sidebar row; `row_focus_cb` (`ui_sidebar.c`) then opens the contact after the one on screen, on the next tick (`lv_async_call`) and only if a row still has focus: deleting a focused text box also refocuses a row mid-delete, and switching there re-entered `show_chat` and crashed. `chat_key_cb`'s Esc branch only runs for keys sent straight to the group (the older `test_keyboard` groups). Every other key flows through LVGL into the *visible* app's group; chat's group stays LVGL's default group.

## Music on the Pi (ops)

Full picture: `docs/music.md`, and `docs/player-spec.md` §4 (users/units) and §9 (mpv).

- **One user, `chatbox`,** runs everything: the kiosk, its mpv child, `chatbox-indexer.service`, PipeWire/WirePlumber and `bt-speaker-reconnect.service` (user units, alive via `loginctl enable-linger chatbox`). Never run the indexer or open `library.db` as another user: SQLite WAL readers need write access to `-shm`.
- **Paths:** music `/srv/music` (`chatbox:chatbox 2775`, the admin user is in group `chatbox`); library `/var/lib/chatbox/library.db` + `art/` (`StateDirectory=chatbox`, shared by both units); mpv socket `/run/chatbox/mpv.sock` (the kiosk's `RuntimeDirectory`, 0700, wiped on every kiosk restart). `$CHATBOX_MPV_SOCKET` overrides the socket for dev/tests.
- **Logs:** kiosk and player → `/var/log/chatbox.err` (grep `player:`, `library:`); indexer → the journal (`journalctl -u chatbox-indexer`); BT loop → `journalctl _SYSTEMD_USER_UNIT=bt-speaker-reconnect.service`.
- **Audio reachability:** a system unit has no `XDG_RUNTIME_DIR`, so the player gives mpv `/run/user/<uid>`. The kiosk deliberately does **not** depend on `user@<uid>.service`/PipeWire (chat must never wait for audio); setup writes a drop-in `chatbox.service.d/10-user-runtime-dir.conf` ordering it after `user-runtime-dir@<uid>.service` only. Late PipeWire is covered by the player (mpv respawn backoff; an audio-output error pauses and retries on the next play).
- **Indexer unit** is sandboxed (`ProtectSystem=strict`, `ProtectHome`, `PrivateNetwork`, …) with low priority (`Nice=15`, batch CPU, idle IO). `ProtectHome` means `music_dir` must stay outside `/home`.
- **BT reconnect** runs as `chatbox` (an earlier version ran it as the admin user). `deploy/bt-speaker/install.sh` (called by setup and deploy) moves it to `chatbox` and renames any other user's unit + WirePlumber override to `*.disabled-by-chatbox`: two WirePlumber sessions monitoring bluez fight over the speaker.
- **Key repeat facts** (they drive every held-key rule): on the Pi only ↑ ↓ ← → Backspace can be held at all (libinput driver releases everything else at once); LVGL repeats after 400 ms then every 100 ms. SDL delivers OS repeats as fresh press + fake-release pairs and hides the real key-up. One-shot keys go through the 450 ms `key_filter` window, and a **real release resets it** so fast separate presses all act: on the Pi the shell's read hook sees real releases; on SDL `main.c`'s `SDL_AddEventWatch` reports key-downs/key-ups to the shell (`shell_key_pushed` / `shell_key_released`), which pairs them with the presses LVGL delivers. Details: `docs/player-spec.md` §8.1.

## Homeserver

Synapse does **not** terminate TLS: it runs plain HTTP on `127.0.0.1:8008` (`homeserver/docker-compose.yml`) behind a reverse proxy that does (`homeserver/Caddyfile.example`, docs/homeserver.md).

- Never re-run `homeserver/setup-synapse.sh` against a server in use: it wipes `files/` (DB + signing key). It refuses unless `FORCE_WIPE=1`.
- Kiosk logs go to `/var/log/chatbox.err` on the Pi (needs `sudo`), not the journal. `SSL peer certificate or SSH remote key was not OK` there = a certificate problem on the server side, not a firmware bug.

## Things to remember

- **Optimistic UI**: outgoing bubbles render immediately in `do_send` ([ui_chat.c:648](pi/src/ui_chat.c#L648)) before the network round-trip. The send outbox + retry exists so this optimism is honest.
- **Timelines come from `/sync` only** (as in matrix-js-sdk / matrix-rust-sdk). Boot order: whoami (queued in `main()` before anything else) → initial sync (timeline limit 50, the filter in `matrix_api.c`), which seeds every contact's room in `msg_cache` → a chat renders from `msg_cache` (`load_history`). There is no `/messages` fetch for the visible window: the kiosk never pages back, and fetching the same 50 events beside the initial sync is what produced duplicate bubbles and the old history race. A chat opened before the initial sync shows "Loading…" until a sync succeeds (the poll timer retries forever, so offline at boot is fine). No sync is sent before whoami answers (it tells our events from incoming ones). Our own device's echo is not drawn again (the optimistic bubble is it): it carries `unsigned.transaction_id`, or has the `event_id` our send returned (`sync_merge_note_sent`; a token made with Synapse's admin login API has no device, so the server sends no `transaction_id`). Other own events come from another device and draw as ours. A `limited` sync is appended like any other (the gap before it isn't filled). `matrix_async_get_messages` is left for `test_api`.
- **LVGL heap and image cache** (`lv_conf.h`): 16 MB builtin heap and a 5 MB decoded-image cache (`LV_CACHE_DEF_SIZE`). Decoded PNGs (album art, emoji) live in that heap; without the cache every redraw re-decodes a PNG. The music UI drops the cache on a library generation change.
- **Panel cache**: up to 3 chat panels (`PANEL_CACHE_SIZE` in `main.c`) are kept in an LRU cache so switching contacts doesn't rebuild their bubbles; an evicted panel is rebuilt from `msg_cache`. A chat switch cancels the history drain, but pending sends are NOT cancelled — they complete against `g_current_room` which may have changed.
- **Texts that look right but are dropped**: usually a queue / outbox issue, not a UI issue — or the server is unreachable (check for the SSL error above first). Grep stderr for: `matrix_async: send rejected by server (HTTP …), dropping` (permanent 4xx), `matrix_async: send attempt N failed, retrying in …ms` (transient, will retry), `matrix_async: send outbox full, dropping message` (outbox at 32), or `ui_chat: pending tracker full, marking send failed immediately`. A transient "retrying" line is expected during an outage and is not a drop.
