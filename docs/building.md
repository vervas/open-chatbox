# Building

Two binaries, both C11, built with CMake from `pi/`:

- **`chatbox`**, the kiosk app;
- **`chatbox-indexer`**, the music indexer daemon (only when SQLite is found).

Plus one test binary per `pi/src/test_*.c`.

## Get the source

LVGL and cJSON are git submodules (`pi/lib/lvgl` at LVGL v9.2.2, `pi/lib/cJSON`), so clone with them:

```bash
git clone --recursive <repo-url> chatbox
# or, in an existing clone:
git submodule update --init --recursive
```

At configure time CMake applies [pi/patches/lvgl-keyboard-drivers.patch](../pi/patches/lvgl-keyboard-drivers.patch) to the LVGL submodule (see [The LVGL patch](development.md#the-lvgl-patch)).

## Targets

| Target | Backend | Display / input | Where |
|---|---|---|---|
| Raspberry Pi (the kiosk) | `FB` | Linux framebuffer `/dev/fb0` + libinput/xkbcommon keyboard, no X11/Wayland | built on the Pi itself (`make build`, or `make deploy` from a dev machine) |
| Another Linux machine as the kiosk (e.g. an old laptop) | `FB` | same as the Pi | `cmake -DCHATBOX_BACKEND=FB` on that machine |
| Desktop, macOS or Linux | `SDL2` (default) | an SDL2 window, the OS keyboard layout | `make build` |
| Docker (Debian bookworm) | `SDL2` | headless (`SDL_VIDEODRIVER=dummy`) | `docker-compose run build` |

There is no cross-compile setup and no WebAssembly/Emscripten build: the Pi build runs on the Pi.

## Dependencies

| | Required | Optional (what you lose without it) |
|---|---|---|
| all | C compiler, CMake ≥ 3.16, pkg-config, libcurl, git | SQLite 3 (music: without it the app shows "No music yet" and the indexer isn't built); libjpeg + libpng (photos: without them photos show a placeholder, but chat still saves them) |
| `SDL2` backend | SDL2 | |
| `FB` backend | libinput, libevdev, xkbcommon (also libdrm, libgbm on the Pi) | |
| at runtime | | `mpv` (playback) and `ffmpeg`/`ffprobe` (the indexer's tags and art) |

Per platform:

```bash
# macOS (Homebrew); libcurl comes with the OS
brew install cmake pkgconf sdl2 sqlite jpeg-turbo libpng
brew install mpv ffmpeg                         # optional: music on the desktop

# Debian / Ubuntu desktop
sudo apt install build-essential cmake git pkg-config libsdl2-dev \
    libcurl4-openssl-dev libsqlite3-dev libjpeg-dev libpng-dev
sudo apt install mpv ffmpeg                     # optional

# Raspberry Pi OS Lite (bookworm) / Debian for the FB kiosk
sudo apt install build-essential cmake git pkg-config \
    libdrm-dev libgbm-dev libinput-dev libevdev-dev libxkbcommon-dev \
    libcurl4-openssl-dev libsqlite3-dev libjpeg-dev libpng-dev \
    mpv ffmpeg sqlite3
```

On the Pi, [deploy/setup-pi-system.sh](../deploy/setup-pi-system.sh) installs all of this plus the audio stack ([pi.md](pi.md)).

## Desktop (SDL2)

```bash
make build        # cmake + make in pi/build-native (SDL2 on macOS / x86_64 Linux)
make run          # chatbox with test/fixtures/chatbox.conf, in a 1280×800 window
make test         # test_keyboard, the headless UI/keyboard tests
make clean
```

`make run` points at `http://mock:8008` from the fixture config; for a working chat on the desktop, run the mock server (`docker-compose up -d mock` and a copy of the fixture with `homeserver_url = http://localhost:8008`) or point a copy of [deploy/chatbox.conf.example](../deploy/chatbox.conf.example) at your homeserver. The desktop build loads emoji from `pi/src/assets/emoji/` and avatars from `avatars/`, relative to the working directory, so run it from the repo root ([configuration.md](configuration.md#fixed-paths-and-environment)).

By hand, without the Makefile:

```bash
cmake -S pi -B pi/build-native -DCHATBOX_BACKEND=SDL2
cmake --build pi/build-native -j
pi/build-native/chatbox path/to/chatbox.conf
```

## Raspberry Pi (framebuffer)

The usual way is from a dev machine: `make deploy` rsyncs the tree to the Pi, builds there with `-DCHATBOX_BACKEND=FB -DCMAKE_BUILD_TYPE=Release`, and installs the binaries and services ([pi.md](pi.md#3-deploy)).

On the Pi itself, `make build` picks `FB` automatically on aarch64 Linux. By hand:

```bash
cmake -S pi -B pi/build -DCHATBOX_BACKEND=FB -DCMAKE_BUILD_TYPE=Release
cmake --build pi/build -j4
sudo install -m 755 pi/build/chatbox pi/build/chatbox-indexer /usr/local/bin/
```

The FB build draws straight to `/dev/fb0` at 1280×800 and reads every keyboard through libinput (hot-plug is rescanned every 2 s). Run it on a text console (tty1; [deploy/chatbox.service](../deploy/chatbox.service) does this), as a user in the `video` and `input` groups.

### Other Linux machines

Nothing in the FB build is Pi-specific: any Linux box with a framebuffer console and a keyboard can be the kiosk. The Makefile only picks `FB` on aarch64, so call CMake directly (`-DCHATBOX_BACKEND=FB`) on an x86_64 machine. The setup and deploy scripts in `deploy/` assume a Debian-based system (`apt`, systemd) and the Pi's boot config (`/boot/firmware/config.txt` for HDMI); on other hardware, skip or adapt those parts. The UI is laid out for 1280×800.

## Docker

The image ([docker/Dockerfile](../docker/Dockerfile), Debian bookworm) has every build and test dependency, including real `mpv` and `ffmpeg`:

```bash
docker-compose run build          # SDL2 build into pi/build-docker
docker-compose run test           # build + every unit test + the e2e script against the mock server
```

Containers always build into `pi/build-docker`. Don't point a container at `pi/build` or `pi/build-native`: those hold the host's CMake cache (e.g. macOS paths), which a Linux CMake refuses to reuse.

## CMake options

| Option | Default | |
|---|---|---|
| `CHATBOX_BACKEND` | `SDL2` | `SDL2` or `FB` |
| `CHATBOX_KEY_HINTS` | `OFF` | a key-hint bar along the bottom of the screen |
| `CHATBOX_PROFILE` | `OFF` | main-loop watchdog, section timers and LVGL's FPS/heap overlays ([development.md](development.md#profiling-on-the-pi)) |
| `CMAKE_BUILD_TYPE` | (none) | `Release` for the kiosk |

CMake caches options in the build directory: `make clean` (or a fresh build dir) before switching.

## Fonts

The bitmap fonts in `pi/src/font_*.c` are generated and committed; you only need to regenerate them after changing a range or a font: `make fonts` (needs `npx`, uses `lv_font_conv@1.5.3`). Sources and licences are in [pi/fonts/](../pi/fonts).
