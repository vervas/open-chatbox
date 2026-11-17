# Photos

Photos is the kiosk's third app, next to Chat and Music. Family members send photos from their phones in the chat (Element), the kiosk keeps them in one folder, and the child looks at them full screen. Receive only: the kiosk never sends photos.

Design notes: [design.md](design.md#4-photos). Green is Photos' colour (`UI_C_PHOTOS*` in `pi/src/ui_theme.h`).

## How a photo gets there

```
phone (Element) ──m.image──► Synapse ──/sync──► kiosk chat
                                                   │  photo_inbox: download (mxc://, curl_multi),
                                                   │  check it's a JPEG/PNG, write a temp file, rename
                                                   ▼
                                        /srv/photos/20261115-143012-<hash>.jpg ──► Photos app
```

- **Chat is the only way in.** Every `m.image` in a contact's room is downloaded once and saved in `photos_dir`, the initial sync included, so photos sent while the kiosk was off arrive at the next boot. The chat shows the photo as a picture bubble. Encrypted images (`content.file`) are not supported (the rooms aren't encrypted).
- **Only JPEG and PNG, at most 25 MB.** The bytes decide, not the event's `mimetype`: anything that doesn't start like a JPEG or PNG is skipped and its bubble says "Photo can't be shown". An event whose `info.mimetype` or `info.size` already rules it out isn't downloaded at all.
- **Downloads** go through the async dispatcher (no blocking calls) in a slot of their own beside the main queue, so a slow photo never holds up `/sync` or sends: `GET /_matrix/client/v1/media/download/…` with the access token (Matrix 1.11 authenticated media), falling back to the old `/_matrix/media/v3/download/…` when a server answers 404/400. One photo at a time; a transient failure (network, 5xx, 401, 403, 408, 429) retries after 2, 8, 20, 40 s and then every 60 s for as long as it takes (like a text); after five tries the bubble says "Photo didn't arrive", and if the photo arrives later the bubble shows it after all. A permanent failure (other 4xx, too big, not a photo) is never retried; 401 and 403 are not permanent (a token or server fix makes them work).
- **Saving is atomic.** The bytes go to a hidden temp file (`.incoming-<hash>.tmp`) in the folder, are `fsync`'d and then renamed, so the library never lists half a photo. The file's mtime is set to the time the photo was sent.
- **Names** are `YYYYMMDD-HHMMSS-<16 hex>.jpg|.png`: the send time (UTC) and a hash of the event ID. Nothing from the event (its body, the phone's file name) reaches the file system.
- **Once only.** `photos_dir/.chatbox-seen` lists the events the kiosk is done with (saved or refused), the newest 2048. A photo deleted in the Photos app is not downloaded again by the next sync or the next boot.

## The Photos app

| | Keys |
|---|---|
| **Grid** — every photo, newest first, four across, cropped to 4:3 cards with the date under each | arrows move (edges bounce, no wrap); **Enter** opens the viewer; **Delete** asks to delete the selected photo; **Esc** opens the switcher |
| **Viewer** — the whole photo as large as fits (a 3:1 panorama is letterboxed), round ← → arrows (pale at the ends), the date and "3 of 13" | **← →** previous / next (ends bounce); **Delete** asks to delete it; **Esc** back to the grid |
| **Delete?** — a white sticker over the grid or the viewer | **Enter** deletes, **Esc** keeps it; every other key is ignored |
| **No photos yet** | **Esc** opens the switcher |

- **Delete** is the real Delete key (`LV_KEY_DEL`, 127). It removes the file from the folder and nothing else: the chat keeps its bubble. After deleting in the viewer it shows the next photo (the previous one if it was the last, the grid if none are left).
- ←, →, Enter, Esc and Delete act once per press (a held key doesn't repeat); ↑ ↓ repeat in the grid. The key context is `KEY_CTX_PHOTOS` (`pi/src/key_filter.c`).
- **The screen stays on longer while the viewer is up**: the lock overlay comes after `sleep_photo_viewer_mins` idle (20 by default) instead of `sleep_lock_secs`, then the display powers off after `sleep_off_secs` as usual. Only the viewer, only while it is on screen: the grid, the switcher (even over the viewer) and the other apps use the normal timeout.
- **The switcher** has a green Photos tile with the newest photo and "N photos". With three apps the tiles stand upright (360×520) instead of square.
- **Changes in the folder** show up within 2 s (the folder's mtime is polled; a full rescan runs every 20 s for files that change in place). Photos can be copied in by hand too (`scp` / `rsync` into `/srv/photos`, any `*.jpg`, `*.jpeg`, `*.png`); a file still being copied shows the placeholder until it is complete.

## Memory and drawing

Phone photos are 12 MP and more; decoded at full size one would fill LVGL's 16 MB heap. So the kiosk never gives a photo file to LVGL: `photo_decode.c` decodes each photo straight to the size it is drawn, with libjpeg's DCT scaling (1/2, 1/4, 1/8) doing most of the work, EXIF orientation applied, into ordinary malloc'd memory, and `ui_photo_img.c` wraps that as an LVGL image drawn 1:1.

| Where | Size | Notes |
|---|---|---|
| Grid card | 256×192 (147 KB) | a cache of 32 decoded cards; one decoded per tick, the selected one first |
| Viewer | up to 1168×592 (~2 MB) | decoded on the key press that shows it, into a second buffer, then swapped |
| Chat bubble | up to 360×300 | one per tick; 8 MB for all bubbles. The open chat's bubbles on screen come first, then its newest, then other chats'; past 8 MB a bubble that comes first takes the picture of one that comes last (that one shows the glyph until it's scrolled to) |
| Switcher tile | 284×213 | only when the switcher opens and the newest photo changed |

PNGs have no cheap scaling and are decoded whole first (capped at 24 MP). A JPEG over 120 MP, a CMYK JPEG or anything that doesn't decode shows the placeholder.

## Config

In `/etc/chatbox.conf`, above `[contacts]` ([configuration.md](configuration.md); comments go on their own lines):

```ini
# where chat saves photos and Photos reads them
photos_dir = /srv/photos
# idle minutes before the lock while the viewer is up (0 = no override)
sleep_photo_viewer_mins = 20
```

`sleep_photo_viewer_mins` must be a whole number; garbage keeps 20, it clamps to 0..1440.

## On the Pi

- `/srv/photos` is `chatbox:chatbox 2775`, made by `setup-pi-system.sh` (and by `make deploy` when missing). The admin user is in group `chatbox`, so it can copy photos in or clean up.
- Building needs `libjpeg-dev` and `libpng-dev` (setup installs them). Without them the kiosk still builds and saves chat photos, but shows placeholders (CMake warns; so does `make deploy`).
- Log lines (in `/var/log/chatbox.err`) start with `photos:` (`saved …`, `skipping …`, `… is not a JPEG or PNG`, `… failed 5 times`, `deleted …`) and `matrix_async: media …` for download errors.

## Troubleshooting

| Symptom | Check |
|---|---|
| A photo bubble stays a grey glyph | `grep photos: /var/log/chatbox.err`. `no folder … chat photos are not saved` / `cannot write in`: `photos_dir` is missing or not writable by `chatbox`. `matrix_async: media … (HTTP 401)`: the token. |
| "Photo can't be shown" | Not a JPEG/PNG (HEIC, GIF, WebP), over 25 MB, or refused by the server. Ask for the photo to be sent as JPEG. |
| A deleted photo came back | It was copied in again by hand; chat never re-downloads (`.chatbox-seen`). Removing `.chatbox-seen` makes the next boot download again every deleted photo still in the last 50 events of a room. |
| Photos show placeholders on the Pi but chat saves them | The build lacks libjpeg/libpng: re-run `setup-pi-system.sh`, then `make deploy`. |

Code: `pi/src/photo_lib.{c,h}` (the folder), `photo_decode.{c,h}`, `photo_inbox.{c,h}` (chat → folder), `ui_photos.{c,h}`, `app_photos.c`, `ui_photo_img.{c,h}`, photo bubbles in `ui_chat.c`; tests in `test_photos.c` and `test_keyboard.c` group 34.
