# Design: one look for Home, Chat, Music and Photos

How the kiosk's design language (the "sticker kit") came about: three rounds of throwaway HTML drafts and the feedback on each, then a Photos draft. It is a record, not a spec: the firmware (`pi/src/ui_theme.h`) is the source of truth. The HTML drafts themselves are not part of this repository.

Before the sticker kit, the switcher, chat, emoji panel, music columns, Now Playing and "No music yet" each had their own colours, header and focus style.

## 1. Sticker book vs night light

Two proposals sharing one set of rules (one frame, one colour per app, three item states, Esc is back, dead ends bounce, pictures first, the other app peeks in):

- **A, Sticker book:** light page, thick ink outlines, focused item turns marker-yellow and lifts on a hard shadow.
- **B, Night light:** deep indigo, no outlines, a glowing yellow focus ring.

Both had a global top bar and a key-hint bar, plus a few suggested key changes.

Feedback: keep it light, but B's calm was better; the top and bottom bars are too much; hints should be a build option; drop the key suggestions; one Tab should swap friends; no status text on bubbles (the pale → blue → red colours are enough).

## 2. Calm light

One calm light theme, slim per-app headers, hints as an option, Tab swaps friends, colour-only bubble states. Music in two layouts: A columns (as before), B a grid of album covers with an album page.

Feedback: go with the grid; bring back the sticker fun, but toned down. The sticker Home switcher was the favourite; framing every album cover was too much.

## 3. Final sticker kit

The sticker look scaled by how many of a thing are on screen:

- **Full stickers** (thick ink outline + solid offset shadow) only for the few big things: Home tiles, Play, Send, the volume card, the "No music yet" badge.
- **Many things** (album cards, songs, friends, emoji) stay flat; the focused one gets a soft app tint, a thin ink outline and a small solid shadow.
- Covers unframed, rounded.

Implemented in the firmware (`pi/src/ui_theme.h`) with these changes:

- The dashed line between the chat's friend list and the conversation became a white rounded shelf for the friends, with the page colour around it.
- Andika has no Greek, so reading text is Didact Gothic (also made for teaching children their letters); titles are M PLUS Rounded 1c.

## 4. Photos

The sticker kit applied to a third app, Photos, with green as its colour (`#2FA37A`, soft `#C3EAD9`, ink `#146B4B`; text on it in ink). A grid of photos four across, each cropped to a 4:3 card with its caption, like the album grid; a viewer that shows the whole photo (a 3:1 panorama is letterboxed) in one big sticker, with round arrows at the sides and "N of M" under it. Grid: arrows move, edges bounce, Enter opens; viewer: ← → previous / next, Esc back.

Implemented in the firmware ([docs/photos.md](photos.md)) with these changes:

- The captions are the date the photo was sent (photos come from chat and have no titles).
- Delete (with a "Delete this photo?" sticker: Enter / Esc), "No photos yet", and a green Photos tile in the switcher, none of which the draft shows.
