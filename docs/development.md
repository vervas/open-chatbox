# Development

## Requirements

| What | For |
|---|---|
| SDL2, CMake, pkg-config, libcurl (and SQLite, libjpeg, libpng) | native builds: `make build` ([building.md](building.md)) |
| Docker with `docker-compose` | the Linux test image and the mock Matrix server |
| `.env` with `PI_HOST`, `PI_USER` | `make deploy`, `make run-pi`, `make screenshot` |
| `npx` | regenerating fonts (`make fonts`) |
| ImageMagick (`magick`) | `make avatar` |

Submodules: `git submodule update --init --recursive` (`pi/lib/lvgl` at v9.2.2, `pi/lib/cJSON`). The commands here use the standalone `docker-compose`; `docker compose` works the same.

## Building and running

```bash
make build        # pi/build-native: SDL2 on Mac/Linux, FB on an aarch64 Pi
make run          # chatbox with test/fixtures/chatbox.conf (point it at the mock server)
make run-pi       # pulls the Pi's /etc/chatbox.conf + avatars into pi-local/, avatars/, and runs with them
make clean
```

CMake option `CHATBOX_BACKEND` is `SDL2` (default; `lv_sdl_window` + `lv_sdl_keyboard`) or `FB` (the Pi; `lv_linux_fbdev` on `/dev/fb0` + `lv_libinput` with xkbcommon). It becomes `CHATBOX_USE_SDL` / `CHATBOX_USE_FB` for both the app and LVGL. Without SQLite, `chatbox` builds with a stub library (music always shows "No music yet") and the indexer and its tests are skipped.

Build directories: `make build` uses `pi/build-native`, containers use `pi/build-docker`. Never point a container at `pi/build` or `pi/build-native`: they hold macOS CMake caches that Linux cmake rejects.

### The LVGL patch

`pi/CMakeLists.txt` applies [pi/patches/lvgl-keyboard-drivers.patch](../pi/patches/lvgl-keyboard-drivers.patch) to the LVGL submodule at configure time. It maps Win/Menu/Super → `0x04`, PageUp/PageDown → `0x05`/`0x06` and adds Esc, in both the libinput (Pi) and SDL keyboard drivers. The hook has two traps:

1. If the submodule already has a stale earlier version applied, `git apply --check` fails, CMake logs "LVGL patches already applied or not needed", and the build quietly keeps the stale patch. Symptom: keys from the patch do nothing, everything else works.
2. The patch isn't a configure dependency, so `cmake --build` after a pull doesn't re-apply it.

After any pull that touches `pi/patches/` or the LVGL submodule, rebuild clean:

```bash
rm -rf pi/build && cmake -S pi -B pi/build && cmake --build pi/build
```

### Fonts

`pi/src/font_chat_{28,36,48}.c` (reading text: Didact Gothic, with DejaVu Sans for the few glyphs it lacks) and `font_title_{28,36,48}.c` (titles: M PLUS Rounded 1c ExtraBold) are bitmaps (Latin-1, Latin Extended-A, Greek, common punctuation, €) generated from the TTFs in `pi/fonts/` (OFL; licences next to them). A character outside their ranges draws as a box. Regenerate all six with `make fonts`, or the `Opts:` line at the top of each file (`lv_font_conv@1.5.3`; 1.5.2 emits LVGL 8 code).

The look is in `pi/src/ui_theme.h` (the "sticker kit": colour tokens, item states, stickers); the why is in [design.md](design.md). A build with `-DCHATBOX_KEY_HINTS=ON` adds the key-hint bar along the bottom (off by default).

## Tests

| Command | What |
|---|---|
| `make test` | `test_keyboard`: headless UI/keyboard tests (navigation, emoji panel, shell and switcher, music screens, Photos). The music, chat and Photos groups also write PNG snapshots to `$TMPDIR/chatbox-snapshots/`; look at them after UI changes |
| `docker-compose run test` | builds in Docker, runs every unit test binary (with real `mpv`/`ffmpeg`, so the real-mpv and real-ffprobe tests run too), then the e2e script against the mock server |
| `docker-compose up e2e` | only the e2e script ([test/test_e2e_docker.sh](../test/test_e2e_docker.sh)) |
| `docker-compose run test-config` / `test-api` / `test-keyboard` | one binary each (`test-api` needs `docker-compose up -d mock`) |

Unit test binaries (in the build dir): `test_config`, `test_api` (sync Matrix calls against a server), `test_async` (the async dispatcher and send outbox), `test_msg_cache`, `test_sync_merge` (`/sync` into the timelines), `test_key_filter`, `test_lang` (typing languages), `test_player` (against a fake mpv), `test_indexer`, `test_library`, `test_photos` (the photo folder, the decoder, chat's photo inbox; it paints its own test JPEGs/PNGs), `test_keyboard`.

The e2e script starts the mock server and a headless `chatbox` (`SDL_VIDEODRIVER=dummy`), runs `test_keyboard`, injects messages, sends one through `test_api`, checks the mock received it and that `chatbox` didn't crash. It also sends photos (an `m.image` with a JPEG made by `ffmpeg`, a fake one, and one on a server without authenticated media) and checks they land in `photos_dir` once, or are refused.

There is no automated test on real hardware. For that, use a strict `curl` from the Pi to the homeserver ([homeserver.md](homeserver.md#troubleshooting)) and the on-Pi checklist in [music.md](music.md#on-pi-checklist).

## The mock Matrix server

[docker/mock_server/server.py](../docker/mock_server/server.py) is a Flask stand-in for the Matrix client-server API on port 8008. Every endpoint wants `Authorization: Bearer test_token_123` (else 401).

| Endpoint | Method | |
|---|---|---|
| `/_matrix/client/v3/account/whoami` | GET | mock identity |
| `/_matrix/client/v3/sync` | GET | room events; honours `since` |
| `/_matrix/client/v3/rooms/<id>/messages` | GET | history; honours `limit`, `from` |
| `/_matrix/client/v3/rooms/<id>/send/<type>/<txnId>` | PUT | stores the message in memory |
| `/_matrix/client/v3/login` | POST | mock access token |
| `/_test/sent_messages` | GET | everything sent so far (test helper) |
| `/_test/fail_sync?count=N` | POST | the next N syncs answer 503 (offline at boot) |
| `/_test/replay_sync` | POST | the next incremental sync repeats every event, `limited: true` (dedup check) |
| `/_matrix/media/v3/upload` | POST | store the body as media, answer its `mxc://localhost/<id>` |
| `/_matrix/client/v1/media/download/localhost/<id>` | GET | the media (authenticated, Matrix 1.11); 404 while `/_test/media_v1?enabled=0` |
| `/_matrix/media/v3/download/localhost/<id>` | GET | the media, legacy endpoint (no token needed) |
| `/_test/inject?room=<id>` | POST | the JSON body becomes the content of an event from the room's other member (e.g. an `m.image`), in the next sync |
| `/_test/media_v1?enabled=0\|1` | POST | pretend to be a server without authenticated media |
| `/_test/media_downloads` | GET | which endpoint served each download |

Sent messages show up in later `/sync` responses for the same room, with `unsigned.transaction_id` (as Synapse returns it to the sending device).

```bash
docker-compose up -d mock
curl -s -H "Authorization: Bearer test_token_123" http://localhost:8008/_matrix/client/v3/account/whoami
```

## Screenshots of the real display

`make screenshot` grabs `/dev/fb0` from the Pi into `screenshots/<timestamp>.png`; `make diff` compares the two newest (or `BEFORE=… AFTER=…`).

## Profiling on the Pi

When the kiosk feels sluggish, deploy a profiling build:

```bash
CHATBOX_PROFILE=ON make deploy
```

That adds a main-loop watchdog (`[prof] main-loop tick took=N ms` whenever one `lv_timer_handler()` pass exceeds 100 ms), section timers around the usual suspects (`populate_emoji_panel`, `emoji_change_page`, `ui_chat_add_message`, `poll_timer_cb`, `kb_hotplug_cb`, `show_chat`), and LVGL's FPS/CPU and heap overlays. It is a build-time CMake option with no runtime toggle; off, it compiles out completely. Redeploy without it to go back to a release build (the deploy rebuilds from a clean `build/` every time).

On the Pi:

```bash
sudo tail -f /var/log/chatbox.err | grep -E 'prof|WARN'        # watchdog + timers (the kiosk logs here, not the journal)
top -H -p $(pgrep -x chatbox)                                   # main-thread CPU
iostat -x 1                                                     # filesystem stalls
sudo perf record -F 99 -p $(pgrep -x chatbox) -g -- sleep 30 && sudo perf report --stdio | head -60
```

Local profiling builds: `make build` doesn't pass `-DCHATBOX_PROFILE`, and CMake caches whatever was set last in `pi/build-native/CMakeCache.txt`. Use `make clean && make build` for a known-off build, or `make clean && cmake -DCHATBOX_PROFILE=ON -B pi/build-native pi` for on.
