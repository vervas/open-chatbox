# Music player + app shell: spec

Status: **spec, binding**, written before the implementation and kept as its contract. This is the single source of truth the implementation tasks (§13) worked from.

"Mockup A" below was a throwaway browser demo of the original UX; the look and the album grid (D13) come from later design drafts ([design.md](design.md)). Neither is part of this repository: this file is the contract.

Audience: a 6-year-old. Artwork does most of the work, and text is secondary. Every screen must be usable by someone who can't read well yet.

Target: 1280×800 framebuffer, keyboard only. No touch, no mouse. **Nothing is reachable only by mouse or touch.** The SDL dev build still creates a mouse indev, but no feature may depend on it, and tests drive keys only.

---

## 0. Decision record

Each decision is referenced below as D1…D13.

| # | Decision | Why |
|---|---|---|
| D1 | The switcher key is **Esc**, layered: the visible app gets Esc first; if it doesn't use it, the shell toggles the switcher. Win/Menu (`0x04`) stays the chat emoji toggle and does nothing in the switcher. In music it toggles Now Playing ⇄ the album grid (added after T10, §8.2–§8.3). | Esc is already mapped on both drivers (no patch change, so no clean-rebuild gotcha), browsers can capture it (the mockup and the firmware share one key map), chat needs no retraining, and "Esc = get me out" is one simple rule. Browsers can't capture the Win key. |
| D2 | One-shot keys use a **450 ms window** that a **release resets**: a one-shot key event is dropped when the same key produced an event (accepted or dropped) less than 450 ms earlier *with no release of that key in between*. | Every separate press acts, however fast ("if I navigate too fast they get swallowed"), while a held key still fires exactly once on both backends. |
| D3 | **No mouse, focus isolation.** Each app and the switcher own a separate `lv_group`. The keyboard indev points at the visible one. | Tab or an arrow can never move focus into a hidden app. |
| D4 | **Play semantics** (full rules in §7.3): Enter on a song plays and **opens Now Playing** at once (Enter on an album opens its page, D13), so ←/→ switch tracks with no extra key. **Enter only ever plays, never pauses:** on a song or album it plays from 0:00 (even the current song), and on Now Playing it restarts the current song from 0:00. **Space is the only pause key** (playing ↔ paused; stopped or nothing loaded → nothing). **Enter is the only key that plays:** → on a song does nothing (a bounce, like every dead end). On Now Playing ← follows the iPod rule, → on the last track just bounces (the song plays on), ↑/↓ are volume, and Esc closes Now Playing. | Enter works like other players: "double Enter on the same song starts from the beginning", and a kid never pauses by accident with it. Pause/resume has one key of its own. Landing on Now Playing makes "next song" one key away ("on enter it should go on the player mode so switching tracks should be instantly possible"); ←/→ for previous/next and ↑/↓ for volume match the arrows' direction. Esc = "get me out" (D1) also backs out of Now Playing. |
| D5 | Space, PgUp and PgDn are **music keys**, not global media keys. They act only while music is visible. | Chat keeps Space = typing and PgUp/PgDn = emoji paging. To pause from chat, the kid switches to music. |
| D6 | **One user, `chatbox`,** runs the kiosk, the indexer, mpv, the PipeWire session and the BT reconnect loop. Music lives in `/srv/music`. | One user means no cross-user socket or file permissions, and SQLite WAL readers can write the `-shm` file. |
| D7 | Tags and art come from **ffprobe/ffmpeg**, not TagLib. With no art, the **UI** draws the fallback cover. | `ffmpeg` is already on the Pi and on most dev machines. The indexer never has to render text. |
| D8 | **inotify only on Linux.** Elsewhere the indexer polls. | macOS (a common dev machine) has no inotify. The 30-minute rescan is the safety net anyway. |
| D9 | **Config: flat keys, parser unchanged,** full-line `#` comments only. | `config.c` has no inline comments and ignores sections other than `[contacts]`. See §10. |
| D10 | **mpv** child process over JSON IPC; the player app owns the queue. Pausing on BT sink loss is **deferred to v2**. | mpv keeps playing through UI hiccups. Sink-loss detection needs more research (§12). |
| D11 | **Switcher v1 has two tiles:** Music and Chat. | "Soon" tiles are dead ends for a kid. |
| D12 | **Music UI = mockup A, reduced to two columns: Albums \| Songs** (+ Now Playing + mini strip + volume overlay). Albums is every album in the library; there is no Artists column. If there is no library yet, a "No music yet" screen. | Mockup A was the clearest of the three mockups (B and C are deleted). The Artists level was dropped: "ditch the artists column. just albums. we figure later grouping for other kinds." Grouping (artists or other kinds) is a v2 item (§12). The mockup still shows three columns; this spec wins. |
| D13 | **The sticker kit** (ui_theme.h, [design.md](design.md)): one light look for the switcher, chat and music; each app owns a colour (music orange, chat blue); anything you can pick is idle, selected (soft tint) or focused (tint + ink ring + small solid shadow); full stickers only for the few big things. **Music browses an album grid**: every album as a big cover card, four across; **Enter on an album opens its page** (cover and title on the left, its songs on the right); Enter on a song plays. This replaces D12's two columns. | The columns were "very scattered" next to chat and the switcher. A grid of big covers is what a kid recognises; "go in with Enter, back with Esc" is one rule for every level (grid → album → Now Playing). |

---

## 1. Media folder

**Decision: nested `Artist/Album/NN Title.ext` under `/srv/music`, with the index built from tags. The folder layout is only a fallback.**

```
/srv/music/                      ← chatbox:chatbox, mode 2775 (D6)
├── The Wiggly Bananas/
│   └── Fruit Salad Party/
│       ├── cover.jpg            ← album art (also folder.jpg / front.png)
│       ├── 01 Peel It Up.mp3
│       └── 02 Monkey Business.mp3
├── Compilations/                ← "Various Artists" albums
│   └── Bedtime Songs/…
└── _inbox/                      ← drop zone for loose files; untagged ones get artist "Unknown"
```

Why nested:
- Album art as a `cover.jpg` next to the tracks is the most reliable art source. Embedded art is hit and miss.
- Parents curate the folder over rsync or Samba. Artist/Album folders are self-explanatory, while a flat folder of 400 mp3s is not.
- The indexer still reads tags first. When tags are missing it uses the path, `Artist/Album/NN Title`, so a mis-tagged file still lands in the right place.

Supported formats are mp3, m4a/aac, ogg/opus and flac, whatever mpv plays. Anything else is ignored.

Why `/srv/music` and not `/home/pi/Music`: the kiosk, the indexer and mpv run as `chatbox` (D6), and the admin user reaches the folder through group `chatbox`. The setgid bit (`2775`) keeps new files in group `chatbox`.

## 2. Index

**Decision: SQLite in WAL mode. A single file at `music_db` (default `/var/lib/chatbox/library.db`).**

JSON was ruled out because:
- The UI would parse all of it at boot and hold it all in RAM.
- Every rescan would rewrite the whole file, and a crash mid-write corrupts it.
- There is no concurrent reader/writer story.

With SQLite, the indexer writes and the UI reads, each with its own connection. WAL lets both happen at once without blocking.

```sql
CREATE TABLE artist (id INTEGER PRIMARY KEY, name TEXT UNIQUE, sort_name TEXT, art_id INTEGER);
CREATE TABLE album  (id INTEGER PRIMARY KEY, artist_id INTEGER REFERENCES artist,
                     title TEXT, year INTEGER, dir TEXT UNIQUE, art_id INTEGER);
CREATE TABLE track  (id INTEGER PRIMARY KEY, album_id INTEGER REFERENCES album,
                     path TEXT UNIQUE, title TEXT, track_no INTEGER, disc_no INTEGER,
                     duration_ms INTEGER, mtime INTEGER, size INTEGER);
CREATE TABLE art    (id INTEGER PRIMARY KEY, sha1 TEXT UNIQUE, src TEXT);  -- thumbnails on disk by id
CREATE TABLE meta   (key TEXT PRIMARY KEY, value TEXT);                     -- schema_version, last_scan, generation
```

- The schema SQL lives in one header shared by the indexer and the reader, so the two can't drift.
- `art_id` is `NULL` when there is no art. The UI then draws the fallback cover (§7.6).
- The UI never decodes full-size JPEGs (and there is no JPEG decoder in `lv_conf.h`, only LodePNG). The indexer writes pre-scaled PNG thumbnails to `music_art_dir/<art_id>_{96,256,512}.png`, the sizes used by the lists, the tiles and Now Playing.
- An artist's art is the art of its newest album that has art. With none, `NULL`.
- `meta.generation` goes up by one after every committed pass that changed something. The UI polls it (§7.7) and reloads what it shows when it changes.
- Ordering: the Albums column (every album) by title, case-insensitive with a leading "The " ignored, then year (unknown last), then id; tracks by disc, then track number, then file name. (Artists by `sort_name`, and an artist's albums by year then title, are still available to the reader for v2 grouping; the v1 UI doesn't show them.)

### WAL and ownership
- The indexer creates the DB and sets `PRAGMA journal_mode=WAL` once. The mode is stored in the file.
- A WAL reader needs write access to the `-shm` file (and the directory, if `-wal`/`-shm` have to be created). Both processes run as `chatbox` and the DB lives in `/var/lib/chatbox`, owned by `chatbox`, so this just works (D6). Never open the DB as another user, and never pass `immutable=1`.
- The UI opens with `SQLITE_OPEN_READONLY` and a short `busy_timeout` (50 ms). It finishes every statement right away (`sqlite3_reset`) and never holds a read transaction across frames, so the indexer's checkpoints can complete.
- Thumbnails are written to a temp name and renamed into place *before* the transaction that references them commits. The UI never sees an `art_id` whose files are missing.

## 3. Indexer service

A small separate daemon, `chatbox-indexer`, run as a systemd unit. It is not part of the UI process, so a slow tag read or a thumbnail resize can never stall a frame. Written in C like the rest of the firmware, it is a separate target in `pi/CMakeLists.txt` and does not link LVGL.

- **Startup:** a full scan. It walks the tree, compares `mtime`+`size` with the `track` rows, and only probes new or changed files. It deletes rows for files that have gone, then albums and artists left empty.
- **Live updates, Linux (D8):** `inotify` on the whole tree (`IN_CLOSE_WRITE`, `IN_MOVED_*`, `IN_DELETE`, `IN_CREATE` on directories, adding watches for new directories). Events are **debounced for 5 s**, so copying in an album (rsync, Samba) triggers one pass instead of hundreds.
- **Safety net, Linux:** a full rescan every 30 minutes. inotify misses changes made through some network-share paths, and its queue can overflow (`IN_Q_OVERFLOW` triggers a rescan right away).
- **Other platforms (e.g. macOS for development):** no inotify. A periodic rescan only, interval set by a flag.
- **Transactions:** each pass runs in one transaction. The UI only ever sees a whole library, never half a scan. `meta.generation` goes up only after a committed pass that changed something. A pass with no changes does not bump it.
- **Tags (D7):** `ffprobe -v error -print_format json -show_format -show_streams <file>`, parsed with cJSON. It gives title, artist, album_artist, album, track, disc, date and the duration. Each spawn has a timeout; a file that fails to probe is skipped and retried next pass.
- **Path fallback:** a missing tag is filled from `Artist/Album/NN Title.ext`. Files under `_inbox/` with no artist tag get artist "Unknown". The album artist is `album_artist`, else the track artist; albums under `Compilations/` get "Various Artists".
- **Art (D7):** `cover|folder|front.(jpg|jpeg|png)` in the album folder (case-insensitive) → else embedded art from the first track that has it → else none (`art_id NULL`). `ffmpeg` extracts and scales to square 96/256/512 PNGs. `art.sha1` is the hash of the source image bytes, so identical covers are stored once. Thumbnails no longer referenced are deleted after the pass.
- **Interfaces:** the tag reader and the thumbnailer sit behind a small function-pointer interface. The real ones spawn `ffprobe`/`ffmpeg`. Tests stub them.
- **Command line:** `chatbox-indexer [--config /etc/chatbox.conf] [--music-dir D] [--db F] [--art-dir D] [--poll-secs N] [--once]`. Flags override the config keys (§10). `--once` does one pass and exits (tests, manual runs).

## 4. Users, units and permissions (D6)

Everything runs as the system user `chatbox`. There is no second user and no cross-user socket.

| Unit / process | Kind | User | Directories and access |
|---|---|---|---|
| `chatbox.service` (kiosk) | system unit | `chatbox` | `RuntimeDirectory=chatbox` → `/run/chatbox/` (holds `mpv.sock`). `StateDirectory=chatbox` → `/var/lib/chatbox/` (reads `library.db`, writes its `-shm`, reads `art/`). Reads `/srv/music` (through mpv). |
| mpv | child of the kiosk | `chatbox` | `/run/chatbox/mpv.sock`. Gets `XDG_RUNTIME_DIR=/run/user/<getuid()>` if unset, to reach PipeWire. Reads `/srv/music`. |
| `chatbox-indexer.service` | system unit | `chatbox` | Shares `StateDirectory=chatbox`: writes `library.db`, `library.db-wal`/`-shm`, `art/`. Reads `/srv/music`. |
| PipeWire, pipewire-pulse, WirePlumber | user units | `chatbox` (linger enabled) | `/run/user/<uid>/`. The WirePlumber override `80-disable-logind.conf` goes in `~chatbox/.config/wireplumber/wireplumber.conf.d/`. |
| `bt-speaker-reconnect.service` | user unit | `chatbox` (member of `bluetooth`) | [deploy/bt-speaker/](../deploy/bt-speaker/README.md). |

- `/srv/music` is `chatbox:chatbox`, mode `2775`. The admin user joins group `chatbox` to rsync or Samba files in.
- `loginctl enable-linger chatbox` starts the user manager at boot, so `/run/user/<uid>` and PipeWire exist without a login. The kiosk can start before PipeWire is ready; mpv's respawn backoff (§9) covers that.
- Kiosk logs still go to `/var/log/chatbox.err`. The indexer logs to the journal.

## 5. App shell and multitasking

**Decision: one LVGL process (the current chatbox binary grows a shell), with playback in an mpv child process.**

```
┌──────────── chatbox (single thread, LVGL main loop) ────────────┐
│ shell: app registry, key filter, switcher overlay, key routing   │
│  ├── app "chat"    ─ matrix async dispatcher (curl_multi timer)  │
│  └── app "music"   ─ library reader (sqlite), mpv IPC client     │
└──────────────────────────────────────────────────────────────────┘
        │ JSON IPC over a unix socket         │ read-only
        ▼                                     ▼
      mpv --idle (audio only) ──► PipeWire ──► BT speaker
                                  library.db ◄── chatbox-indexer (daemon)
```

- **Only one app is visible at a time.** Each app owns an `lv_obj` root. Switching hides one root and shows the other. Nothing is destroyed, so the chat's scroll position, the player's cursor and so on are all kept.
- **Every app keeps running in the background.** The app's timers (`async_tick_cb`, the mpv poll, the generation poll) are registered by the shell and not tied to visibility. Chat keeps syncing while music is on screen, and music keeps playing while chat is on screen.
- Because playback lives in **mpv**, audio carries on through UI hiccups. The player talks to it over the JSON IPC socket, which is non-blocking, and polls from an `lv_timer`. This fits the single-thread rule.
- Why not two processes? Only one process can own `/dev/fb0` and the keyboard, and a compositor is overkill here.
- The boot app is `default_app` from the config (§10). `home` boots chat with the switcher open over it.
- Chat boots into the first contact's conversation; after that, coming back to chat finds the contact that was open. With no contacts in the config, chat shows "No contacts yet" (like music's "No music yet").

### App contract (`app.h`)

```c
typedef struct app {
    const char   *id;                       /* "chat", "music" */
    void        (*create)(lv_obj_t *root);  /* build the UI once, hidden */
    lv_group_t *(*group)(void);             /* this app's own focus group (D3) */
    void        (*on_show)(void);
    void        (*on_hide)(void);
    bool        (*on_key)(uint32_t key);    /* true = handled, stop */
    void        (*tile_render)(lv_obj_t *tile); /* live tile content (T9) */
    int         (*badge)(void);             /* unread count, 0 = none */
} app_t;
```

### Focus isolation (D3)
- Chat, music and the switcher each own an `lv_group`. The shell points the keyboard indev at the group of whatever is visible (`lv_indev_set_group`). A hidden app's widgets are never in the active group, so Tab (`LV_KEY_NEXT`) and arrows can't reach them.
- Music's group holds a single focusable key-sink object, so Tab/`LV_KEY_NEXT`/`LV_KEY_PREV` have nowhere to go and are ignored.
- Code that today calls `lv_group_get_default()` (sidebar, chat, `display_sleep.c`) must use the right group. In particular the lock overlay must be added to the group the keyboard indev is using *now*, or it won't swallow the wake key.

### Key routing
Every key event goes through these steps, in order:
1. **Display locked or off:** the lock overlay takes the key and the display wakes (the existing wake-swallow). The shell and the apps see nothing, for the whole press (until the key is released). The key filter still records the key's time (step 2), so a one-shot key held down to wake the display does not act after the wake. Its release resets the record like any release, so the next press acts at once.
2. **Key filter (D2):** record the time; if the key is one-shot in the current context (§8.1) and the same key had an event less than 450 ms ago with no release of it since, drop it. A release of a key clears its record (§8.1). A dropped key does nothing anywhere.
3. **Switcher open:** the switcher handles the key (§6). Nothing reaches the apps.
4. **Visible app:** `on_key(key)`. If it returns true, stop.
5. **Shell fallback:** an unhandled Esc opens the switcher. Anything else is ignored.

The mechanism that feeds LVGL key events through these steps (an event callback on the key-receiving objects, or an indev-level hook) is T4's choice. The behaviour is the contract.

## 6. Switcher (D11)

- A full-screen overlay in the sticker look (D13): the page colour, "Hi there!" and a clock at the top, then **two large sticker tiles**: Music (left, orange) and Chat (right, blue), each with a thick ink ring and a bold label bottom-left.
- The focused tile gets a thicker ring and a big solid ink shadow. The currently visible app's tile wears a yellow "Now" sticker. When the switcher opens, focus starts on that tile.
- ←/→ move between the tiles. ↑/↓ bounce. Enter opens the focused app. Esc closes the switcher and returns to the current app unchanged. Enter on the current app's tile does the same as Esc.
- **Live tiles (T9):** Music shows the current album art, the track title and a playing/paused glyph, or a plain music-note face when nothing is loaded. Chat shows the number of contacts with unread messages and the newest sender's avatar, or a speech-bubble icon when there are none.
- **Animation:** tiles fly in with the Metro "turnstile" stagger when opening (about 250 ms). A picked tile zooms to fill the screen (about 200 ms). Input is never blocked: any key during either animation first jumps it to the end state, then the key is processed normally (including the key filter) in the new state.

## 7. Music app (D12, D13: the album grid)

### 7.1 Layout
- 1280×800. A 68 px header ("Albums"; chat's peek pill on the right when a contact wrote) over one of two pages:
  - **The grid:** every album as a **card** (280 px): the 256 px cover (thumbnail or fallback cover) and the title (up to two lines), four across, a playing equaliser after the title of the playing album. The focused card has the focus look (soft orange, ink ring, solid shadow).
  - **The album page:** the selected album's 256 px cover, title, artist (unless "Various Artists") and "year · N songs" on the left; its **song rows** (88 px: track number, the album's cover at 64 px, the title up to two lines) on the right, the focused one with the focus look.
- A **mini now-playing strip** is always visible along the bottom of both pages (104 px): art thumbnail, title, "artist · album", elapsed/total, a play/pause sticker and a progress line. With nothing loaded it says "Nothing playing yet" / "Pick a song and press Enter".
- **Now Playing** covers everything: huge art (512 px), "Now Playing"/"Paused", title, artist, album, "Song N of M", a progress bar with elapsed and remaining time, and three transport stickers in keyboard order: ⏮ (←), the orange ⏸/▶ (Space), ⏭ (→). Paused, a white pause sticker sits on the cover.
- A **volume overlay** shows for 1.5 s after PgUp/PgDn (and ↑/↓ on Now Playing): the percentage, 20 segments, and the part above `music_volume_max` hatched out. At the cap it says "that's the loudest!", at 0 "silent".
- Text is at least 28 px (list titles 28 px, strip title 36 px, Now Playing title 48 px). Every row shows artwork first. Text is never the only way to identify something.

### 7.2 Hierarchy
```
the grid (every album)  ─Enter─►  the album page (its songs)  ─Enter─►  Now Playing
                        ◄─Esc/←─                               ◄─Esc──
```
There is no Artists, Genres, Playlists or search. Albums are ordered by title (case-insensitive, a leading "The " ignored), then year with an unknown year last, then id; a list holds at most 512 albums (`LIBRARY_MAX_ALL_ALBUMS`, the rest are logged and not shown). Grouping albums (by artist or otherwise) is deferred to v2 (§12).

### 7.3 Play semantics (D4)
"Current track" means the track the player has loaded, playing or paused.

- **Enter only plays, never pauses.** Space is the only pause/resume key.
- **Enter on an album (a card):** open its page (§7.4 landing). Nothing plays.
- **Enter on a song:** queue = the whole album; play from that song from 0:00, **even if it is the current song** (playing, paused or stopped: it restarts); open Now Playing. ← on Now Playing can then still reach earlier tracks.
- **→ on a song:** nothing: a small right bounce, the same feedback as every dead end. It plays nothing, resumes nothing and doesn't open Now Playing, whether the song is current (playing, paused or stopped) or not, or nothing is loaded. On the grid ←/→ move between cards.
- If nothing in the album can be played (no path for any track), Enter on a song bounces and stays on the album page.
- **Enter on Now Playing:** the current song again from 0:00, playing (from playing, paused or stopped). So Enter Enter on a song plays it and then starts it from the beginning again, like other players.
- **Space** (grid, album page, Now Playing, "No music yet"): playing → pause; paused → resume; stopped (end of queue) or nothing loaded → nothing. Space never replays the queue.
- **← on Now Playing (iPod rule):** more than 3 s into the track, or on the first track of the queue → restart the track from 0:00. Otherwise → the previous track from 0:00.
- **→ on Now Playing:** the next track from 0:00. On the last track: a small bounce, nothing else; the song plays on (no stop).
- ←/→ on Now Playing keep the playing/paused state (skipping while paused stays paused). Stopped counts as paused at 0:00: ← loads the previous track paused; → bounces.
- **↑/↓ on Now Playing:** volume ±5 %, exactly like PgUp/PgDn (overlay, cap, "silent"); they repeat when held.
- **End of queue (natural end of the last track only):** playback stops. The last track stays current, at 0:00, paused; the progress resets. There is no repeat in v1. Space then does nothing; Enter on a song plays it; Enter on Now Playing plays that last track again from 0:00.
- **Esc on Now Playing:** back to the playing album's page, with the playing album and song selected (music consumes this Esc; the switcher doesn't open). **Esc on an album page:** back to the grid (music consumes it). **Esc on the grid:** the switcher.

### 7.4 Selection memory
- **Albums:** one selection. At boot, the first album.
- **Songs:** remembered per album. Moving the grid's selection onto an album, or opening its page, lands on the playing song if it's in that album, else the song last selected there, else the first.
- **Back up (← or Esc)** from an album page keeps both selections, so you land on the card you came from.
- **Esc from Now Playing** selects the playing album and song and shows its page (D4).
- **Enter** moves no selection; it opens Now Playing, and leaving Now Playing lands on the playing album and song.
- **Library reload** (generation change): selections are kept by id. If the selected item has gone, the selection stays at the same index, clamped to the list.
- The **currently playing** item is marked on both pages: the album's card and the song's row get a small equaliser glyph (animated while playing, still while paused). A kid can always find "where my song is".

### 7.5 "No music yet"
If the library DB doesn't exist yet, or has no tracks, music shows a friendly full-screen "No music yet" message with a big music note. It keeps polling (every 2 s). When the library appears, the grid shows with the first album focused. Keys: §8.4.

### 7.6 Fallback covers (D7)
An album (or, on Now Playing, an artist) with `art_id NULL` gets a cover drawn by the UI: a diagonal LVGL gradient whose two hues are seeded from a hash of the title, plus the title's first letter, big and white. Deterministic: the same title always gets the same cover. The UI never shows an empty square.

### 7.7 Library polling
- The music app polls `meta.generation` every 2 s from an `lv_timer`. When it changes, it re-queries the lists it shows (§7.4 keeps the selections).
- A track that vanishes from the library keeps playing if mpv can still read it. The queue holds ids and paths, so it isn't touched by a reload.

### 7.8 Kid-proofing
- No destructive actions, no dialogs, no settings in the kid UI. `music_volume_max` and the library contents are set by parents through the config and the filesystem.
- Nothing needs a double-press or a long-press. Held keys are safe (§8.1).

## 8. Keyboard contract

This section is exhaustive. A key not listed for a context is ignored there. Mockup A demos these rules; where it differs, this section wins.

Custom keycodes (from `pi/patches/lvgl-keyboard-drivers.patch`, unchanged): Win/Menu/Super → `0x04`, PageUp → `0x05`, PageDown → `0x06`, Escape → `LV_KEY_ESC`, Tab → `LV_KEY_NEXT`.

### 8.1 Key repeat and the one-shot rule (D2)

Facts:
- **Pi:** libinput never auto-repeats, and the LVGL libinput driver (`lv_libinput.c`) releases every key immediately after its press **except ↑ ↓ ← → and Backspace**. So on the Pi only those five keys can be held at all. For them LVGL re-sends `LV_EVENT_KEY` after `LV_INDEV_DEF_LONG_PRESS_TIME` (400 ms, plus up to one or two 33 ms indev reads) and then every 100 ms. The 100 ms is hard-coded in `lv_indev.c`; the 200 ms in `lv_conf.h` is never used.
- **SDL (desktop):** LVGL's SDL driver turns every `SDL_KEYDOWN`, OS auto-repeats included, into a press followed at once by a fake release, and it ignores `SDL_KEYUP`. Space arrives as `SDL_TEXTINPUT` `" "`. At the LVGL level a repeat is indistinguishable from a real press, and the indev's release edges say nothing.

The rule:
- The shell keeps, **per key**, the time of that key's last event, accepted or dropped. This record is global: it doesn't reset when the context changes.
- A **real release** of a key clears that key's record (`key_filter_release`). So every separate press acts, however fast.
- A **one-shot** key event is dropped when that key's previous event was less than **450 ms** earlier (`KEY_FILTER_WINDOW_MS`) with no release in between, i.e. it is a repeat of a key that is still held. Exactly 450 ms is accepted.
- A non-one-shot key is always accepted, but its time is still recorded.

Where the real release comes from (`shell.c`):
- **Pi (libinput) and the test indev:** the read hook's own release edge. It is real: the driver holds ↑ ↓ ← → Backspace for as long as they are down, and releases every other key at once but never repeats it.
- **SDL:** the indev's releases are fake, so that keyboard is marked `shell_keyboard_use_external_release()` and `main.c` registers an SDL event watch that reports the real `SDL_KEYUP` (`shell_key_released`). The watch runs when SDL *queues* an event, which can be before LVGL has handled an earlier press of the same key (a quick tap's key-down and key-up in one pump). So the watch also reports each queued press (`shell_key_pushed`: every mapped `SDL_KEYDOWN`, repeats included, and each `" "` of an `SDL_TEXTINPUT`), tagged with whether a key-up preceded it, and the shell applies the reset when LVGL delivers that press. A press the watch didn't report falls back to the plain 450 ms window.

Consequences:
- Fast separate presses all act: Enter → on an album opens its page and then bounces (→ on a song does nothing), Enter Enter on an album opens its page and then plays the selected song (Now Playing), Esc Esc on the grid opens and closes the switcher (on an album page: back to the grid, then the switcher), Enter Enter on a song plays it (Now Playing) and then starts it again from 0:00 (Now Playing's Enter; §7.3: Enter never pauses).
- A held one-shot key fires exactly once. Its repeats come with no release in between, each within 450 ms of the previous (dropped) one. On the Pi the first LVGL repeat can land up to about 470 ms after the press, past the window, so the shell also drops a one-shot key event that LVGL re-sends while the key is still physically held (`shell_filter_key` in `shell.c`). On SDL the OS repeats arrive as presses with no key-up between them and the 450 ms window catches them.
- A held key can't act twice across a context change. Holding Enter on the Music tile opens music, and the repeats don't then also open the selected album. Holding Enter on an album opens its page once, and the repeats don't then play a song; holding Enter on a song plays it and opens Now Playing once, and the repeats don't then restart it. Holding Esc on Now Playing closes it once, and the repeats don't then open the switcher. Once it is released, the next press acts at once.
- Dev caveat: if the desktop OS's initial repeat delay is longer than 450 ms, a held key on the SDL build fires a second time at that delay. The Pi is not affected.

One-shot keys per context:

| Context | One-shot keys | Keys that repeat |
|---|---|---|
| Music grid / album page | Space, Enter, ←, →, Esc, Win | ↑ ↓ (a row of cards or a song, about 10/s after 400 ms), PgUp PgDn (on SDL only; on the Pi a held PgUp/PgDn is one step, see above) |
| Now Playing | Space, Enter, ←, → (they skip tracks), Esc, Win | ↑ ↓ (volume, 5 % per step: on the Pi after 400 ms, then every 100 ms), PgUp PgDn (on SDL only, as above) |
| No music yet | Space, Esc | PgUp PgDn |
| Switcher | Enter, ←, →, Esc | ↑ ↓ (bounce only) |
| Chat | Esc | everything else, as today (typing, cursor, focus moves). Chat is unchanged by D5. |

There is no list acceleration (the old "speeds up after 0.5 s" is dropped; lists are short).

### 8.2 Music: the grid and an album page

The grid (every album, four cards across):

| Key | Action | Held | Edges |
|---|---|---|---|
| ← / → | The card before / after, in the same row. The songs follow (landing per §7.4). | one-shot | First / last card of a row: bounce, no wrap. |
| ↑ / ↓ | The card above / below. ↓ into a shorter last row lands on its last card. | repeats | Top / last row: bounce, no wrap. |
| Enter | Open the album's page (landing per §7.4). Plays nothing. | one-shot | — |
| Space | Playing → pause; paused → resume. | one-shot | Nothing loaded, or stopped at the end of the queue: nothing. |
| PgUp | Volume +5 %, overlay for 1.5 s. | repeats (SDL only; one step per press on the Pi) | At `music_volume_max`: stays, overlay says "that's the loudest!". |
| PgDn | Volume −5 %, overlay for 1.5 s. | repeats (SDL only; one step per press on the Pi) | At 0: stays, overlay says "silent". |
| Esc | Open the switcher (music doesn't consume Esc here). Music keeps playing. | one-shot | — |
| Win/Menu | Open Now Playing (the current song, as it is: nothing restarts). | one-shot | Nothing loaded: bounce. |
| Tab, letters, digits, Backspace | Ignored. Tab never moves focus. | — | — |

An album page:

| Key | Action | Held | Edges |
|---|---|---|---|
| ↑ / ↓ | The song before / after. | repeats | First / last song: bounce, no wrap. |
| Enter | Play from 0:00 (§7.3): the album from that song; open Now Playing. Never pauses. | one-shot | The current song (playing, paused or stopped): restarts from 0:00, playing. Nothing playable: bounce. |
| → | Nothing: a bounce (only Enter plays, §7.3). | one-shot | Any song (current or not, playing, paused, stopped, nothing loaded): bounce only. |
| ← | Back to the grid; selections are kept. | one-shot | — |
| Esc | Back to the grid, as ←. Music consumes it (no switcher). | one-shot | A held Esc goes back once; the next press opens the switcher. |
| Space, PgUp, PgDn, Win/Menu | As on the grid. | | |
| Tab, letters, digits, Backspace | Ignored. | — | — |

### 8.3 Music: Now Playing

| Key | Action | Held | Edges |
|---|---|---|---|
| ← | iPod rule: >3 s in, or first track of the queue → restart from 0:00; else previous track from 0:00. Keeps playing/paused. | one-shot | First track: always restarts. Exactly 3 s: previous track. |
| → | Next track from 0:00. Keeps playing/paused. | one-shot | Last track: bounce, nothing else, keeps playing (no stop). Stopped: bounce. |
| ↑ | Volume +5 %, overlay for 1.5 s (as PgUp). | repeats (Pi: after 400 ms, then every 100 ms) | At `music_volume_max`: stays, "that's the loudest!". |
| ↓ | Volume −5 %, overlay for 1.5 s (as PgDn). | repeats | At 0: stays, "silent". |
| Enter | The current song again from 0:00, playing. Never pauses. | one-shot | Paused: restarts and plays. Stopped: plays the last track again from 0:00. |
| Space | Playing → pause; paused → resume. | one-shot | Stopped: nothing. |
| PgUp / PgDn | Volume, as in §8.2. | repeats (SDL only) | As in §8.2. |
| Esc | Close Now Playing: the playing album's page, playing album and song selected. Music consumes it (no switcher). | one-shot | A held Esc closes Now Playing only; the next press goes back to the grid. |
| Win/Menu | Back to the grid, the playing album selected. Keeps playing/paused. | one-shot | A held Win toggles once. |
| Tab, letters, digits, Backspace | Ignored. | — | — |

Now Playing can only be open when a track is current (it opens only after Enter has loaded one), so "nothing playing" never happens here. It opens right after Enter, so ←/→ here are the first keys a kid presses to change songs; they are one-shot (a held ←/→ skips once). The switcher is two more Escs away (album page, grid).

### 8.4 Music: "No music yet"

| Key | Action | Held |
|---|---|---|
| Esc | Open the switcher. | one-shot |
| Space | Playing → pause; paused → resume. Nothing loaded (the normal case) or stopped: nothing. | one-shot |
| PgUp / PgDn | Volume, as in §8.2. | repeats |
| ↑ ↓ ← → Enter Win/Menu Tab letters digits Backspace | Ignored. | — |

### 8.5 Switcher

| Key | Action | Held | Edges |
|---|---|---|---|
| ← | Focus the left tile (Music). | one-shot | On Music: shake. |
| → | Focus the right tile (Chat). | one-shot | On Chat: shake. |
| ↑ / ↓ | Bounce (vertical shake). | repeats | — |
| Enter | Open the focused app (zoom). | one-shot | On the current app's tile: just close. |
| Esc | Close; back to the current app, unchanged. | one-shot | — |
| Space, PgUp, PgDn, Win/Menu, Tab, letters, digits, Backspace | Ignored. Music keys don't reach music while the switcher is up (D5). | — | — |

### 8.6 Chat

The firmware chat keeps today's behaviour (the existing `test_keyboard` suite pins it). The shell adds only the Esc rule.

| Key | Action (firmware) | Held |
|---|---|---|
| Esc | Emoji panel open → close it (chat consumes Esc). Otherwise → open the switcher. | one-shot |
| Win/Menu (`0x04`) | Toggle the emoji panel (only while the text box, the send button or the panel has focus). | as today |
| PgUp / PgDn | Previous/next emoji page while the panel is open; otherwise nothing. Never volume (D5). | as today |
| Space, letters, digits, Backspace | Type in the text box. | Backspace repeats; on the Pi other keys don't (driver release) |
| Enter | Text box: send. Sidebar row: open that contact. Emoji panel: insert the emoji. | as today |
| ↑ / ↓ | Sidebar: previous/next row. Emoji panel: grid navigation. Text box: LVGL default. | repeats |
| ← / → | Text box: move the cursor. Emoji panel: grid navigation. Not one-shot in chat. | repeats |
| Tab | From the text box: the next contact (wrapping); never leaves chat (D3). With one contact: back to the text box. | as today |

**Mockup A's chat is a stand-in**, simpler than the firmware: ↑/↓ change the contact directly (clamped, no wrap); Space, letters, digits and Backspace type (typing focuses the text box first); ←/→ move the text cursor; Enter sends non-empty text; PgUp/PgDn and Win do nothing (there is no emoji panel, so Esc always toggles the switcher); Tab does nothing.

### 8.7 Display locked or off

| Key | Action |
|---|---|
| any | Swallowed by the lock overlay; the display wakes within the next sleep check (≤ 1 s). Keys pressed before the wake completes are swallowed too. The shell does not intercept anything, so Esc does not open the switcher. The key filter records the time of each swallowed key (§5 step 1). |

Music keeps playing while the display is locked or off. The mockup does not model the lock.

### 8.8 Everywhere
- Tab never moves focus out of the visible app or the switcher (D3). In the mockup, Tab is `preventDefault`ed in every context so the browser focus never leaves the stage.
- No key is a chord. Nothing needs a long-press or a double-press.

## 9. Audio backend (D10)

- The shell's player module spawns:
  `mpv --idle=yes --no-video --no-terminal --audio-display=no --input-ipc-server=/run/chatbox/mpv.sock`.
  The socket path is a parameter of the player module (default `/run/chatbox/mpv.sock`, overridable with the `CHATBOX_MPV_SOCKET` environment variable for dev and tests). The mpv binary path is a parameter too, so tests can use a fake.
- If `XDG_RUNTIME_DIR` is unset, the child gets `XDG_RUNTIME_DIR=/run/user/<getuid()>`, so mpv finds PipeWire.
- **IPC:** a non-blocking unix socket, line-delimited JSON. Commands carry a `request_id`. The player observes `pause`, `time-pos` and `idle-active`, and reads events from an `lv_timer`. A read never blocks, a partial line is buffered, and a full send buffer is retried next tick.
- **Queue:** owned by the player module, not by mpv's playlist. It loads one file at a time (`loadfile <path> replace`) and advances on `end-file` with `reason: eof`. An `end-file` with `reason: error` skips to the next track; if every track in the queue fails, it stops.
- **Stop:** at the end of the queue (or ↓ on the last track) the player sends `stop`, keeps the last track as current at 0:00, paused. Resuming reloads that file.
- **Respawn:** if mpv dies or the socket drops, respawn with backoff (0.5, 1, 2, 5 s, then every 5 s). After reconnecting, restore the volume, load the current track, and on `file-loaded` seek to the saved position and restore playing/paused.
- **Volume:** mpv's `volume` property, 0–100, set on every (re)connect. Steps of 5, clamped to `0..music_volume_max`. It starts at `music_volume_default` at boot and survives mpv respawns, but not reboots. The system sink volume stays fixed and is set at deploy time.
- **BT sink loss:** not handled in v1. See §12.

## 10. Config additions (`/etc/chatbox.conf`) (D9)

Flat keys, the parser unchanged:

```ini
# ---------------------------------------------------------------------------
# Music (all optional; defaults shown). Put these ABOVE [contacts].
# music_volume_max: parent-set cap in percent (0-100)
# music_volume_default: volume at boot, clamped to music_volume_max
# default_app: shown at boot: chat, music or home (the switcher, over chat)
# ---------------------------------------------------------------------------
music_dir = /srv/music
music_db = /var/lib/chatbox/library.db
music_art_dir = /var/lib/chatbox/art
music_volume_max = 70
music_volume_default = 40
default_app = chat
```

Why this shape:
- `config.c` treats only lines that **start** with `#` as comments. There is no inline-comment support: `music_volume_max = 70 ; percent` would store the value `70 ; percent`. So comments go on their own lines.
- `config.c` ignores every section header except `[contacts]`: keys under `[music]` or `[shell]` would just be global keys, so sections would only mislead. And every line after `[contacts]` is read as a contact, so the music keys must come **before** `[contacts]`.
- Validation: both volumes clamp to 0..100, and `music_volume_default` is clamped to `≤ music_volume_max`. An unknown `default_app` falls back to `chat` with a stderr warning. Missing keys take the defaults above.
- The indexer reads the same file for `music_dir`, `music_db` and `music_art_dir` (§3). `config_parse` currently fails without `homeserver_url`/`access_token`; the real file always has them, and the indexer's flags cover tests.

## 11. Mockup A (demo only)

- A throwaway browser demo of §7 and §8 (not in this repository) for trying the feel. No tests, no test hooks.
- Keyboard only: no click handlers, and the stage has `pointer-events: none`. It uses the same 450 ms one-shot rule.
- URL parameters: `?speed=20` makes time run 20× faster; `?empty=1` shows the "No music yet" screen; `?app=chat` boots into chat.

## 12. Open items / v2

- **Pause on BT sink loss.** When the speaker drops, PipeWire moves the stream to another sink (often Dummy Output), so mpv keeps "playing" into nothing. Detecting it reliably (watching the default sink with `pw-dump`/`wpctl`, or mpv's `audio-device-list`) needs research. Until then the reconnect loop brings the speaker back and music continues.
- **Grouping albums** (by artist, or other kinds): v1 shows one flat Albums list (D12). The reader still loads artists and an artist's albums (`library_load_artists`, `library_load_albums`) for this.
- Repeat/shuffle.
- More switcher tiles once there are more real apps (no "soon" tiles, D11).
- A lighter fallback for mpv's `loadfile` options (`start=`, `pause=`) once the Pi's mpv version is pinned; v1 seeks on `file-loaded` instead, which works on every version.

## 13. Task breakdown

One commit per task, in this order. Every task keeps the existing tests green (`test_keyboard` with `test/fixtures/chatbox.conf`, `test_async`, `test_msg_cache`, plus every test added by earlier tasks).

### T2 `key_filter` module
- **Goal:** the D2 filter as pure C with no LVGL dependency.
- **Files:** `pi/src/key_filter.{c,h}`, `pi/src/test_key_filter.c`, CMake target `test_key_filter`.
- **API:** `KEY_FILTER_WINDOW_MS 450`; `void key_filter_init(key_filter_t *)`; `bool key_filter_accept(key_filter_t *, uint32_t key, uint32_t now_ms, bool one_shot)`, which always records the time. Fixed-size static table, no allocation, wrap-safe tick arithmetic.
- **Acceptance:** Pi hold pattern (0, 400, 600, 800 … ms) → 1 accept; SDL hold pattern (0, 450-ε, then every 33 ms) → 1; presses 500 ms apart → 2; 449 dropped / 450 accepted; non-one-shot always accepted but recorded (a one-shot event within 450 ms of it is dropped); keys independent; `lv_tick` wraparound at 2³²; table full → oldest entry reused without dropping a fresh key.

### T3 Config keys
- **Goal:** the D9 keys in `config_t`, with defaults, clamping and warnings.
- **Files:** `pi/src/config.{c,h}`, `pi/src/test_config.c` (rewritten).
- **Acceptance:** `test_config` becomes assertion-based and exits non-zero on failure. Cases: every key parsed; defaults when absent; volumes clamped to 0..100; default > max → default = max; unknown `default_app` → `chat`; keys under a `[music]` header are still read (global); keys after `[contacts]` become contacts (documented, not a music key); an inline `; comment` stays in the value (documented). Chat behaviour unchanged.

### T4 App shell
- **Goal:** `app.h` contract, app registry, per-app groups, key routing with the filter (§5), Esc layering, the switcher overlay with static tiles, chat moved behind the contract with no behaviour change, `default_app` at boot. Music is a stub app showing "No music yet".
- **Files:** `pi/src/app.h`, `pi/src/shell.{c,h}`, `pi/src/switcher.{c,h}`, `pi/src/app_chat.c`, `pi/src/app_music.c` (stub), `pi/src/main.c`, `pi/src/display_sleep.c` (overlay joins the active group), `pi/src/test_keyboard.c`.
- **Acceptance:** all existing `test_keyboard` tests pass unchanged. New groups: Esc in chat with the emoji panel open closes only the panel; Esc in chat otherwise opens the switcher focused on Chat; ←/→/↑/↓/Enter/Esc in the switcher per §8.5; returning to chat keeps the selected contact and the typed text; a held Esc (LVGL repeat timing) toggles once; Tab in chat or the switcher never focuses an object of another app; Win, Space and PgUp in the switcher do nothing; while locked, Esc only wakes; `default_app = music` boots into music.

### T5 `chatbox-indexer` daemon
- **Goal:** §2 and §3: schema, scan, ffprobe/ffmpeg behind interfaces, thumbnails, inotify (Linux) or polling, 5 s debounce, one transaction per pass, generation.
- **Files:** `pi/src/indexer_main.c`, `pi/src/indexer_scan.{c,h}`, `pi/src/indexer_probe.{c,h}` (interface + ffprobe/ffmpeg implementations), `pi/src/indexer_watch.{c,h}`, `pi/src/library_schema.h`, `pi/src/test_indexer.c`, `test/fixtures/music/` (a small tree of placeholder files), CMake targets `chatbox-indexer` and `test_indexer`.
- **Acceptance** (stubbed prober and thumbnailer, fake clock): the fixture tree (built at runtime in `$TMPDIR`, nothing committed) produces the expected artists/albums/tracks and ordering; path fallback for missing tags; `_inbox` → "Unknown"; `Compilations` → "Various Artists"; `cover.jpg` beats embedded art; no art → `art_id NULL`; artist art = newest album with art; a rescan with no changes probes nothing and doesn't bump `generation`; one changed file → only it is probed, generation +1; a deleted file → its row, empty album and empty artist gone; a file that fails to probe is skipped (a new file isn't added, a changed file keeps its old row) and retried next pass, the rest of the pass commits — one corrupt file must never freeze the library; three thumbnails per art, orphans removed; debounce: events at 0–4 s → one pass at last event + 5 s; WAL mode and `schema_version` set. Builds and passes on the Mac (poll-only).

### T6 Library reader
- **Goal:** read-only SQLite access from the UI process and generation polling (§2, §7.7).
- **Files:** `pi/src/library.{c,h}`, `pi/src/test_library.c`, CMake target `test_library`.
- **Acceptance:** a missing DB → a "missing" result, and opening succeeds once the DB appears; a generation bump written by another connection is seen while the reader is open (WAL); the reader cannot write; artists/albums/tracks come back in §2 order; art paths resolve to `<art_dir>/<id>_<size>.png` or "none"; a writer holding the DB never blocks the reader longer than `busy_timeout`. List loads may allocate (not a hot path) and free on reload.

### T7 mpv client
- **Goal:** §9: spawn and supervise mpv, non-blocking IPC, the queue, volume clamp, respawn with restore.
- **Files:** `pi/src/player.{c,h}`, `pi/src/test_player.c`, a fake mpv (`pi/src/test_fake_mpv.c`, a tiny program that serves the IPC socket, records commands and emits scripted events), CMake targets `test_player` and `test_fake_mpv`.
- **Acceptance:** the argv is exactly §9's; `XDG_RUNTIME_DIR` is set when unset; playing a queue loads the right file; `eof` advances; the last `eof` → stopped (last track, 0:00, paused); prev/next per §7.3; toggle; resume from stopped reloads; volume clamped to 0 and to the max and sent on connect; killing the fake → respawn with backoff and the queue, track, position and pause state restored; a hung or silent fake never blocks `player_tick` (bounded time); partial JSON lines are reassembled; `reason: error` skips, an all-bad queue stops.

### T8 Music UI
- **Goal:** mockup A in LVGL (§7), wired to the library reader and the player. (Follow-up: the Artists column was dropped for Albums | Songs, and Enter opens Now Playing (D4, D12); artist grouping is deferred to v2, §12.)
- **Files:** `pi/src/ui_music.{c,h}`, `pi/src/app_music.c`, fake library/player for tests, `pi/src/test_keyboard.c`. Fonts through the existing `make fonts` target if a new size is needed.
- **Acceptance:** new `test_keyboard` music groups with a fake player and fake library cover every row of §8.2, §8.3 and §8.4, the D4 cases, §7.4 selection memory, held-key timing (LVGL repeat), a generation change reloading while keeping selections, the fallback cover when `art_id` is NULL, and the "No music yet" → library appears transition. Existing tests still pass.

### T9 Live tiles
- **Goal:** the Music and Chat live tiles (§6).
- **Files:** `pi/src/switcher.c`, `pi/src/app_music.c`, `pi/src/app_chat.c`, `pi/src/test_keyboard.c`.
- **Acceptance:** the Music tile shows the fake player's art, title and play/pause glyph, and the idle face when nothing is loaded; it updates while the switcher is open; the Chat tile shows the number of contacts with unread messages and the newest sender, and follows the sidebar's unread rules (opening the Chat app clears nothing; opening a contact clears that contact).

### T10 Deploy and infra
- **Goal:** §4 on the Pi, and the docs.
- **Files:** `deploy/chatbox.service` (`RuntimeDirectory=chatbox`, `StateDirectory=chatbox`), `deploy/chatbox-indexer.service` (new), `deploy/setup-pi-system.sh` (packages `mpv ffmpeg sqlite3 libsqlite3-dev pipewire pipewire-pulse wireplumber`; `/srv/music` `2775`; `chatbox` into `bluetooth`; admin into `chatbox`; `loginctl enable-linger chatbox`; user units for `chatbox`), `deploy/deploy-to-pi.sh` (installs `chatbox-indexer`), `docker/` Dockerfile deps (`libsqlite3-dev`), `deploy/chatbox.conf.example` (§10 keys above `[contacts]`), `README.md`, `CLAUDE.md`, and `deploy/bt-speaker/` for user `chatbox`.
- **Acceptance:** `systemd-analyze verify` passes on both units (in the Docker build container); `docker-compose run build` and `docker-compose run test` pass; a written on-Pi checklist (music plays through the BT speaker as `chatbox`, the indexer picks up an rsynced album within about 5 s, the kiosk survives `pkill mpv`).
