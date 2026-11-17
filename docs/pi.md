# The Pi: setup, config and operations

The kiosk is a Raspberry Pi running Raspberry Pi OS Lite (no desktop), with a 1280×800 HDMI display, a USB keyboard ([keyboard.md](keyboard.md)) and a Bluetooth speaker. It reaches the homeserver over HTTPS ([homeserver.md](homeserver.md)).

Everything runs as one system user, **`chatbox`**. The kiosk is a systemd unit, `chatbox.service`, which takes over tty1 and draws straight to the framebuffer. Music adds `chatbox-indexer.service`, an mpv child process and PipeWire user services ([music.md](music.md)).

## First-time setup

### 1. System setup (once, safe to re-run)

```bash
ssh pi@chatbox.local 'sudo bash -s' < deploy/setup-pi-system.sh
# or, once a deploy has synced deploy/ (needed for bt-speaker/ and the units next to it):
ssh pi@chatbox.local 'sudo bash ~/chatbox-deploy/setup-pi-system.sh'
```

It:
- installs the build dependencies (including `libjpeg-dev` and `libpng-dev` for photos), `linux-perf` and `sysstat` (for profiling), and the music stack (`mpv ffmpeg sqlite3 pipewire pipewire-pulse wireplumber libspa-0.2-bluetooth bluez`);
- creates the `chatbox` system user (groups `input video render bluetooth`);
- writes a tty1 auto-login override for `chatbox` (legacy; the next deploy disables `getty@tty1` again, see below);
- disables console blanking;
- creates `/etc/chatbox.conf` from `deploy/chatbox.conf.example` if it doesn't exist;
- creates the log file `/var/log/chatbox.err`;
- sets up music: `/srv/music`, the admin user into group `chatbox`, linger for `chatbox`, the kiosk's drop-in, the BT speaker unit, the indexer unit ([music.md](music.md#users-and-units));
- creates the photo folder `/srv/photos` (`chatbox:chatbox 2775`, [photos.md](photos.md)).

### 2. Config

```bash
ssh pi@chatbox.local "sudo nano /etc/chatbox.conf"
```

Fill in `homeserver_url`, `access_token` and the contacts ([homeserver.md](homeserver.md#rooms-between-the-family-and-the-kiosk) prints them). To add a family member later, see [A new family member](homeserver.md#a-new-family-member). Every key: [configuration.md](configuration.md).

### 3. Deploy

From your dev machine, with `.env` holding `PI_HOST` and `PI_USER`:

```bash
make deploy                     # = PI_HOST=… PI_USER=… ./deploy/deploy-to-pi.sh
```

It ships the **local working tree**, not a branch or commit. It:
1. rsyncs `pi/` to `~/chatbox/` and `deploy/` to `~/chatbox-deploy/` on the Pi; copies emoji PNGs to `/usr/share/chatbox/emoji/` and `avatars/*.png` (if present) to `/usr/share/chatbox/avatars/`; makes sure the HDMI config is set for the 1280×800 display;
2. builds on the Pi from a clean `build/` (`cmake -DCHATBOX_BACKEND=FB -DCMAKE_BUILD_TYPE=Release`, `make -j4`);
3. installs `/usr/local/bin/chatbox` and, when SQLite was found, `/usr/local/bin/chatbox-indexer`; creates `/srv/photos` if it is missing and warns when libjpeg/libpng were not found;
4. disables `getty@tty1`, removes any old `.bash_profile` launcher, installs/enables/restarts `chatbox.service` and `chatbox-indexer.service`, and (re)installs the BT speaker user unit.

`CHATBOX_PROFILE=ON make deploy` ships a profiling build ([development.md](development.md#profiling-on-the-pi)).

## `/etc/chatbox.conf`

INI-style, read at startup (path = `argv[1]`, default `/etc/chatbox.conf`). Template: [deploy/chatbox.conf.example](../deploy/chatbox.conf.example). The full reference (every key, contacts, avatars, typing languages, the parser's rules) is [configuration.md](configuration.md). The two rules that bite most: every global key must come **before** `[contacts]`, and there are no inline comments.

Avatars are PNGs named after `contact.N.avatar`. Convert a photo with `make avatar IN=photo.heic OUT=avatars/mum.png`; the next deploy copies `avatars/` over.

## Operations

| Task | Command (on the Pi) |
|---|---|
| Status | `systemctl status chatbox chatbox-indexer` |
| Kiosk log | `sudo tail -f /var/log/chatbox.err` (not the journal; music lines start with `player:` / `library:`, photo lines with `photos:`) |
| Indexer log | `sudo journalctl -u chatbox-indexer` |
| Restart the kiosk | `sudo systemctl restart chatbox` |
| Stop it for maintenance | `sudo systemctl stop chatbox` (it restarts on its own only after a crash, `Restart=on-failure`) |
| Screenshot of the display | `make screenshot` from your dev machine (saves `screenshots/<timestamp>.png`; `make diff` compares the last two) |

**Display sleep.** After `sleep_lock_secs` idle a dark overlay covers the screen; after `sleep_off_secs` more the display powers off. While the Photos viewer is on screen the overlay waits `sleep_photo_viewer_mins` minutes instead (20 by default; the grid and every other screen use `sleep_lock_secs`). A new message resets the countdown and shows "New message!". The first key or tap only wakes the screen; it is swallowed, so a wake never sends, plays or switches anything.

## Troubleshooting

- **Kiosk not running.** `systemctl status chatbox`. The unit only starts when `/usr/local/bin/chatbox` and `/etc/chatbox.conf` exist (`ConditionPathExists`). Then read `/var/log/chatbox.err`: a config error exits with code 1; `/dev/fb0` errors mean `chatbox` lost the `video` group; "keyboard" lines show which `/dev/input/eventN` was attached (it is rescanned every 2 s, so hot-plugging works).
- **Getty fights the kiosk on tty1** after re-running `setup-pi-system.sh` (it still enables auto-login). Re-run `make deploy`, or `sudo systemctl disable --now getty@tty1`.
- **Messages not sending or arriving.** Grep `/var/log/chatbox.err`:
  - `SSL peer certificate or SSH remote key was not OK` → a cert problem on the homeserver, not the Pi ([homeserver.md](homeserver.md#troubleshooting));
  - `matrix_async: send attempt N failed, retrying in …` → transient; the outbox retries forever and delivers once the server is back;
  - `send rejected by server (HTTP …), dropping` → the server refused it permanently (check the room / token);
  - `send outbox full` or `pending tracker full` → more than 32 queued sends.
- **HTTP 401.** The `access_token` is wrong or was revoked ([homeserver.md](homeserver.md#troubleshooting)).
- **Keys from the LVGL patch do nothing** (Win, PgUp/PgDn, Esc) while the rest works: the binary has a stale LVGL patch. Deploys build from a clean `build/` on the Pi, so this mostly bites local builds ([development.md](development.md#the-lvgl-patch)).
