# Music on the Pi

The kiosk also plays music: an iPod-style app (a grid of album covers; Enter opens an album's page, Enter on a song plays it and opens Now Playing, where ←/→ skip tracks, ↑/↓ change the volume, Enter restarts the song and Esc goes back to the album; Esc steps back one level at a time; Win toggles Now Playing ⇄ the grid; Space pauses/resumes everywhere in music) next to chat, switched with **Esc**. Keyboard only. The contract is [player-spec.md](player-spec.md) (the key map is its §8; the held-key/one-shot rules are §8.1). This page covers how it runs on the Pi.

## Architecture

```
            /srv/music  (chatbox:chatbox 2775, parents rsync / Samba in here)
               │  read                                 │ read (plays files)
               ▼                                       │
┌──────────────────────────────┐                       │
│ chatbox-indexer.service      │ inotify, 5 s debounce,│
│ (system unit, user chatbox,  │ full rescan / 30 min  │
│  nice 15, idle IO, journal)  │                       │
│  ffprobe (tags) ffmpeg (art) │                       │
└──────────────┬───────────────┘                       │
               │ write (one transaction per pass)       │
               ▼                                       │
   /var/lib/chatbox/  (StateDirectory, shared)         │
     library.db  (SQLite WAL, meta.generation)         │
     art/<id>_{96,256,512}.png                         │
               │ read-only, polls generation           │
               ▼                                       │
┌──────────────────────────────┐   spawns + JSON IPC   ┌───────────────────┐
│ chatbox.service (kiosk, tty1)│──────────────────────►│ mpv  (child,      │
│ LVGL shell: Chat │ Music     │ /run/chatbox/mpv.sock │  user chatbox)    │
│ log: /var/log/chatbox.err    │ (RuntimeDirectory)    └────────┬──────────┘
└──────────────────────────────┘                                │ XDG_RUNTIME_DIR=
                                                                ▼ /run/user/<uid>
                      chatbox's user manager (linger) ┌────────────────────────────┐
                      ─────────────────────────────── │ PipeWire + pipewire-pulse  │
                      bt-speaker-reconnect.service ──►│ + WirePlumber (bluez on,   │
                      (bluetoothctl connect loop)     │   80-disable-logind.conf)  │
                                                      └─────────────┬──────────────┘
                                                                    ▼ A2DP
                                                             BT speaker
```

## Users and units

Everything runs as the one system user **`chatbox`** (spec §4, D6), so there are no cross-user socket or SQLite permissions to get wrong.

| Unit / process | Kind | Runs as | Notes |
|---|---|---|---|
| `chatbox.service` | system unit | `chatbox` | The kiosk. `RuntimeDirectory=chatbox` (`/run/chatbox`, mode 0700, holds `mpv.sock`), `StateDirectory=chatbox` (reads the library). A setup-written drop-in orders it after `user-runtime-dir@<uid>.service` only: chat never waits for audio. |
| mpv | child of the kiosk | `chatbox` | `--idle --no-video --input-ipc-server=/run/chatbox/mpv.sock`. Gets `XDG_RUNTIME_DIR=/run/user/<uid>` to reach PipeWire. Respawned with backoff (0.5/1/2/5 s) if it dies. |
| `chatbox-indexer.service` | system unit | `chatbox` | `chatbox-indexer --config /etc/chatbox.conf`. Writes `/var/lib/chatbox/library.db` + `art/`. Low CPU/IO priority, read-only view of the OS and `/srv/music`. Logs to the journal. |
| PipeWire, pipewire-pulse, WirePlumber | user units | `chatbox` | Started at boot by `loginctl enable-linger chatbox`. |
| `bt-speaker-reconnect.service` | user unit | `chatbox` (group `bluetooth`) | [deploy/bt-speaker/](../deploy/bt-speaker/README.md): keeps the paired speaker connected. |

`setup-pi-system.sh` sets all of this up (idempotent): packages (`mpv ffmpeg sqlite3 libsqlite3-dev pipewire pipewire-pulse wireplumber libspa-0.2-bluetooth bluez`), `/srv/music`, `chatbox` into `bluetooth`, the admin user (`$SUDO_USER`) into `chatbox`, linger, the kiosk drop-in, the BT reconnect unit + WirePlumber override (moving them off the admin user if an earlier install put them there), and enables the indexer. `deploy-to-pi.sh` builds and installs both binaries, both system units and the BT unit, and restarts them.

Music config keys (`music_dir`, `music_db`, `music_art_dir`, `music_volume_max`, `music_volume_default`, `default_app`) go **above `[contacts]`** in `/etc/chatbox.conf`; see [pi.md](pi.md#etcchatboxconf) and spec §10. Comments on their own lines only.

## Adding music

Lay it out as `Artist/Album/NN Title.ext` (mp3, m4a/aac, ogg/opus, flac) with a `cover.jpg` (or `folder.jpg` / `front.png`) in each album folder. Tags win; the path is the fallback. `Compilations/<Album>/` becomes "Various Artists", and `_inbox/` is a drop zone where untagged files land under "Unknown".

The admin user is in group `chatbox`, and `/srv/music` is setgid, so new files stay readable by the kiosk. From another machine:

```bash
# -rt (not -a): don't carry Mac owners/groups/permissions over
# --mkpath: create a new Artist/ folder too (without it rsync only makes the last level)
rsync -rtv --mkpath --progress "Fruit Salad Party/" pi@chatbox.local:"/srv/music/The Wiggly Bananas/Fruit Salad Party/"
```

`--mkpath` needs rsync 3.2.3 or newer (`brew install rsync`). macOS's built-in `/usr/bin/rsync` is openrsync and doesn't have it; with that one, `ssh pi@chatbox.local 'mkdir -p "/srv/music/The Wiggly Bananas"'` first.

Or share `/srv/music` over Samba (with `force group = chatbox`, `create mask = 0664`, `directory mask = 2775`). The indexer notices within about 5 s of the last file landing; the music app reloads on its own.

## On-Pi checklist

After `setup-pi-system.sh` + `deploy-to-pi.sh` + a reboot. These are the checks that need the real hardware.

```bash
# 0. Units and users are in place
systemctl is-active chatbox.service chatbox-indexer.service
loginctl show-user chatbox -p Linger                     # Linger=yes
id chatbox                                              # … bluetooth …
id "$USER"                                              # … chatbox …
stat -c '%A %U:%G' /srv/music                           # drwxrwsr-x chatbox:chatbox
sudo ls -l /run/chatbox/                                # mpv.sock, srwx… chatbox
CB="sudo -u chatbox XDG_RUNTIME_DIR=/run/user/$(id -u chatbox)"
$CB systemctl --user is-active pipewire wireplumber bt-speaker-reconnect
$CB wpctl status | sed -n '/Sinks:/,/Sources:/p'         # the BT speaker is a sink (default *)

# 1. Music plays through the BT speaker, as chatbox
ps -o user=,pid=,args= -C mpv                           # chatbox … --input-ipc-server=/run/chatbox/mpv.sock
#    → on the kiosk: Esc, ← to the Music tile, Enter; pick an album, Enter (Now Playing opens). Sound comes out of the speaker.
$CB wpctl status | grep -A3 'Streams:'                   # mpv stream → the BT sink

# 2. The indexer picks up an rsynced album within ~5 s  (from another machine)
rsync -rtv --mkpath "Some Album/" pi@chatbox.local:"/srv/music/Some Artist/Some Album/"
sudo journalctl -u chatbox-indexer -f                   # "indexer: pass done … +N …, generation G (changed)" ~5 s after the copy
sudo -u chatbox sqlite3 -readonly /var/lib/chatbox/library.db "select value from meta where key='generation'"
#    → the album shows up in the music app without a restart.

# 3. The kiosk survives mpv dying
sudo pkill mpv
sudo grep 'player:' /var/log/chatbox.err | tail -3       # "mpv lost …, respawn in 500 ms", "started mpv"
#    → the kiosk stays up, chat keeps working; the song resumes where it was
#      (it was playing → plays on; paused → stays paused).

# 4. Speaker drop-out: switch the speaker off and on again
sudo journalctl _SYSTEMD_USER_UNIT=bt-speaker-reconnect.service -n 5   # "disconnected …", then "reconnected" within ~20 s
```

## Troubleshooting

- **Where the logs are.** Kiosk (and so the player): `sudo tail -f /var/log/chatbox.err`, not the journal; grep `player:` for mpv start/respawn/audio errors and `library:` for the DB. Indexer: `sudo journalctl -u chatbox-indexer`. BT loop: `sudo journalctl _SYSTEMD_USER_UNIT=bt-speaker-reconnect.service _UID=$(id -u chatbox)`.
- **`player: mpv audio output failed … is PipeWire running for this user and XDG_RUNTIME_DIR right`.** mpv can't reach chatbox's PipeWire. Check linger (`loginctl show-user chatbox -p Linger`), that `/run/user/$(id -u chatbox)/pipewire-0` exists, and `$CB systemctl --user status pipewire wireplumber` (with `CB` from the checklist). The player pauses and retries on the next play; nothing needs restarting once PipeWire is up.
- **`player: cannot start mpv`.** mpv isn't installed (`setup-pi-system.sh` installs it). The kiosk keeps running and retries every 5 s.
- **Plays, but silent / from HDMI.** The BT speaker isn't the default sink in chatbox's session: `$CB wpctl status`. If another user's WirePlumber (an install that ran as the admin user) holds the speaker, re-run `sudo bash ~/chatbox-deploy/bt-speaker/install.sh`, then `bluetoothctl disconnect <MAC>` and let the loop reconnect. See [deploy/bt-speaker/README.md](../deploy/bt-speaker/README.md).
- **"No music yet" on the kiosk.** Either there is no library DB yet, or it's empty:
  1. the indexer isn't running or isn't installed: `systemctl status chatbox-indexer` (a `ConditionPathExists` skip means `/usr/local/bin/chatbox-indexer` or `/etc/chatbox.conf` is missing; the binary isn't built without `libsqlite3-dev`);
  2. it found no playable files: each pass logs `indexer: pass done … N files …`. Check `music_dir` in `/etc/chatbox.conf` (keys after `[contacts]` are read as contacts, not settings; and the unit hides `/home`, so keep music outside it);
  3. ffprobe fails on the files: `cannot read tags of …` in the journal (a failed file is retried next pass);
  4. the kiosk reads a different `music_db` than the indexer writes: both use `/etc/chatbox.conf`.
- **Rebuild the library from scratch.** `sudo systemctl stop chatbox-indexer && sudo rm -rf /var/lib/chatbox/library.db* /var/lib/chatbox/art && sudo systemctl start chatbox-indexer`. The kiosk shows "No music yet" until the first pass commits.
