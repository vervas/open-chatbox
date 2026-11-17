# Keyboard

Chatbox is tested with the Redragon K709 Pro. Any USB keyboard (or one with a USB wireless dongle) should work: the kiosk reads every keyboard libinput finds, and it rescans every 2 s, so keyboards can be plugged in and out.

The keys chatbox uses beyond letters and digits: Esc, Tab, Enter, the arrows, Backspace, Delete, Space, PgUp/PgDn and the Win (Super) or Menu key. The full key map is in [player-spec.md](player-spec.md) §8.

**Digits and `!@#…` do nothing, letters work.** On compact keyboards an Fn lock can turn the number row into F1–F12, which the chat ignores. Toggle it back as your keyboard's manual describes. To check from the Pi, run `sudo evtest` on the keyboard's `/dev/input/eventN`: pressing 1 reports `KEY_F1` (code 59) when the lock is on and `KEY_1` (code 2) when it is off.

**Win does nothing.** Many gaming keyboards have a Windows-key lock (often Fn + Win). The chat emoji panel and music's Now Playing toggle use Win, so with the lock on they stop working.

**Delete** deletes the selected photo in the Photos app; it asks first (Enter = yes, Esc = no). It reaches the kiosk as LVGL's `LV_KEY_DEL` on both keyboard drivers without the LVGL patch: SDL maps it, and libinput's xkb gives the Delete keysym's character, 0x7F.
