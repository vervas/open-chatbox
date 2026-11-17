# Configuration: `chatbox.conf`

`chatbox` reads one INI-style file at startup: the path given as its first argument, else `/etc/chatbox.conf`. `chatbox-indexer --config FILE` reads the music keys from the same file. A commented template is [deploy/chatbox.conf.example](../deploy/chatbox.conf.example); `setup-pi-system.sh` copies it to `/etc/chatbox.conf` if that doesn't exist. After editing, `sudo systemctl restart chatbox` (and `chatbox-indexer` for music keys).

## Example

```ini
# Matrix (required)
homeserver_url = https://matrix.example.org
access_token = <the kiosk account's access token>

# Screen sleep (optional)
sleep_lock_secs = 60
sleep_off_secs = 300

# Music (optional)
music_dir = /srv/music
music_db = /var/lib/chatbox/library.db
music_art_dir = /var/lib/chatbox/art
music_volume_max = 70
music_volume_default = 40
default_app = chat

# Photos (optional)
photos_dir = /srv/photos
sleep_photo_viewer_mins = 20

[contacts]
contact.1.name = Mum
contact.1.room_id = !AbCdEfGhIjKlMnOp:matrix.example.org
contact.1.avatar = mum
contact.1.lang = fr

contact.2.name = Dad
contact.2.room_id = !QrStUvWxYzAbCdEf:matrix.example.org
contact.2.avatar = dad

contact.3.name = Grandma
contact.3.room_id = !GhIjKlMnOpQrStUv:matrix.example.org
contact.3.lang = gr
```

## Global keys

All global keys must come **before** `[contacts]` (see [Parser rules](#parser-rules)).

| Key | Default | Meaning |
|---|---|---|
| `homeserver_url` | (required) | Base URL of the Matrix homeserver, e.g. `https://matrix.example.org`. Use HTTPS; libcurl verifies the certificate. |
| `access_token` | (required) | The kiosk account's access token ([homeserver.md](homeserver.md#getting-an-access-token)). Every request sends it as a Bearer token. |
| `sleep_lock_secs` | `60` | Idle seconds before the dark lock overlay covers the screen. `0` or missing = default. |
| `sleep_off_secs` | `300` | Seconds on the lock overlay before the display powers off. `0` or missing = default. |
| `music_dir` | `/srv/music` | The music folder the indexer scans ([music.md](music.md#adding-music)). Keep it outside `/home`: the indexer unit runs with `ProtectHome`. |
| `music_db` | `/var/lib/chatbox/library.db` | The SQLite library the indexer writes and the kiosk reads. |
| `music_art_dir` | `/var/lib/chatbox/art` | Album art thumbnails written by the indexer. |
| `music_volume_max` | `70` | The volume cap, in percent (0–100). The child can't go above it. |
| `music_volume_default` | `40` | Volume at boot, in percent; clamped to `music_volume_max`. |
| `default_app` | `chat` | What's on screen at boot: `chat`, `music`, or `home` (the app switcher, open over chat). Anything else means `chat`. |
| `photos_dir` | `/srv/photos` | Photos sent in the chats are saved here; the Photos app shows (and can delete) what's in it. Must be writable by the kiosk user ([photos.md](photos.md)). |
| `sleep_photo_viewer_mins` | `20` | While a photo is open full screen, the lock overlay waits this many idle minutes instead of `sleep_lock_secs`. `0` = no difference; clamped to 0–1440. |

Unknown global keys are ignored. Sections other than `[contacts]` are ignored too (their keys are read as global keys).

## Contacts

Up to 10 contacts, one Matrix room each, in the order they appear on the kiosk.

| Key | Meaning |
|---|---|
| `contact.N.name` | The name shown on the kiosk. `N` is 1..10. |
| `contact.N.room_id` | The Matrix room ID (`!…`, with `:server` on older room versions) shared by the kiosk account and that person. [homeserver/setup-matrix-rooms.sh](../homeserver/setup-matrix-rooms.sh) creates the rooms and prints these lines. |
| `contact.N.avatar` | Optional. The avatar image name: the kiosk shows `/usr/share/chatbox/avatars/<name>.png` (desktop build: `avatars/<name>.png` in the working directory), or `default.png` from the same folder if it doesn't exist. |
| `contact.N.lang` | Optional typing language for that chat: `gr` or `fr` (see below). Unknown values warn and mean none. |

The short form `Name = !room:server` also works, but it can't carry an avatar or a language.

### Avatars

Avatars are PNGs in `/usr/share/chatbox/avatars/`, named after `contact.N.avatar`. Put them in an `avatars/` folder at the repo root (it is git-ignored) and `make deploy` copies them over. To make one from any photo (needs ImageMagick):

```bash
make avatar IN=photo.heic OUT=avatars/mum.png     # 84×84 picture centred on a 128×128 transparent canvas
```

### Typing language

`contact.N.lang` sets the keyboard for that contact's chat:

| `lang` | Keyboard while that chat is open | `` ` `` then a vowel | ` `` `` ` then a vowel |
|---|---|---|---|
| `gr` | Greek XKB layout (α on A, ε on E, …) | ά έ ή ί ό ύ ώ (and capitals) | same as one `` ` `` (the first `` ` `` stays) |
| `fr` | stays US QWERTY | á é í ó ú (and capitals) | à è ì ò ù (and capitals) |
| none / unset | default | nothing | nothing |

One `` ` `` gives the acute (é, and the Greek tonos); two give the grave (è). The `` ` `` shows as soon as it is typed and becomes the accented letter when the vowel follows; any other key leaves a plain `` ` ``. Backspace removes the whole accented letter. The layout switch only happens on the framebuffer build (libinput + xkbcommon); the SDL build types through the OS layout, but the accent combo works on both. Code: `pi/src/lang.{c,h}`.

## Parser rules

The parser's quirks are deliberate ([player-spec.md](player-spec.md) §10):

- `homeserver_url` and `access_token` are required; anything else missing takes its default.
- **Every line after `[contacts]` is read as a contact**, so global keys must come before it.
- Comments are whole lines starting with `#`. There are no inline comments: `x = 1 # note` has the value `1 # note`.
- Bad numbers or unknown values warn on stderr and fall back to the default; a missing file or a syntax error (a line without `=`, an unclosed `[`) exits with code 1.

## Fixed paths and environment

These aren't in the config file:

| Path / variable | What |
|---|---|
| `/usr/share/chatbox/emoji/<codepoint>.png` | Twemoji PNGs for the emoji picker and emoji in messages. `make deploy` copies `pi/src/assets/emoji/` there; without them emoji draw as boxes. The desktop (SDL2) build reads `pi/src/assets/emoji/` relative to the working directory instead, so run it from the repo root. |
| `/usr/share/chatbox/avatars/` | Contact avatars (above); `avatars/` in the working directory on the desktop build. |
| `/run/chatbox/mpv.sock` | The mpv IPC socket (the kiosk unit's `RuntimeDirectory`). `CHATBOX_MPV_SOCKET` overrides it (dev and tests). |
| `/var/log/chatbox.err` | Where `chatbox.service` sends the kiosk's stdout and stderr. |
