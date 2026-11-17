# BT speaker auto-reconnect

Keeps the Pi connected to a Bluetooth speaker so music
survives the speaker sleeping, going out of range, or being power-cycled.
Runs as the kiosk user `chatbox`, in the same PipeWire session as mpv
([docs/player-spec.md](../../docs/player-spec.md) §4, D6).

| File | Goes to (on the Pi, owned by `chatbox`) |
|---|---|
| `bt-speaker-reconnect.sh` | `~chatbox/.local/bin/bt-speaker-reconnect` |
| `bt-speaker-reconnect.service` | `~chatbox/.config/systemd/user/` (user unit, enabled, needs linger) |
| `80-disable-logind.conf` | `~chatbox/.config/wireplumber/wireplumber.conf.d/` |

Install / update: `setup-pi-system.sh` and `deploy-to-pi.sh` both run
`install.sh` on the Pi (as root, idempotent). By hand, on the Pi:
`sudo bash ~/chatbox-deploy/bt-speaker/install.sh`.

Prerequisites (done by `setup-pi-system.sh`): `chatbox` in group `bluetooth`,
`loginctl enable-linger chatbox`, packages `bluez pipewire pipewire-pulse
wireplumber libspa-0.2-bluetooth`.

Setting the speaker: pair and trust it on the Pi (`bluetoothctl`: `scan on`,
`pair <MAC>`, `trust <MAC>`), put its address in `BT_SPEAKER_MAC` in
`bt-speaker-reconnect.service`, and run `install.sh` (or deploy again).

Migration from an install that ran as the admin user: `install.sh` disables and stops
`bt-speaker-reconnect.service` for any other user under `/home` that still has
it, and renames that user's `80-disable-logind.conf` to
`*.disabled-by-chatbox` so their WirePlumber stops grabbing the speaker. Two
sessions monitoring bluez fight over the A2DP sink.

How it works: a loop checks `bluetoothctl info` every 20 s; when disconnected it
runs `bluetoothctl connect` and, on success, sets the sink volume to 0.8 (the
fixed system volume; the kid's volume is mpv's, capped by `music_volume_max`).
The speaker must already be **paired and trusted**. This doesn't pair. Pairing
is stored system-wide by bluez, so a pairing made as the admin user still holds.

Troubleshooting (run as the admin user on the Pi):
- Status: `sudo -u chatbox XDG_RUNTIME_DIR=/run/user/$(id -u chatbox) systemctl --user status bt-speaker-reconnect`.
- Logs: `sudo journalctl _SYSTEMD_USER_UNIT=bt-speaker-reconnect.service _UID=$(id -u chatbox) -f`.
- `br-connection-profile-unavailable` → chatbox's WirePlumber bluez monitor is
  off: the override is missing or WirePlumber wasn't restarted. Or another
  user's WirePlumber holds the speaker (see Migration).
- `br-connection-page-timeout` → the speaker isn't answering: off, flat battery,
  not in Bluetooth mode, or connected to a phone. The service keeps retrying.
- Connected but silent after the migration → `bluetoothctl disconnect <MAC>`;
  the loop reconnects within 20 s into chatbox's session.
- Speaker won't pair at all → reset it (see its manual), then re-pair on the Pi:
  `bluetoothctl remove <MAC>`, `scan on`, `pair`, `trust`, `connect`.

Switching speakers: pair + trust the new one on the Pi, set `BT_SPEAKER_MAC` in
`bt-speaker-reconnect.service`, re-run `install.sh`, and `bluetoothctl untrust`
the old one so it can't connect by itself and take over the default sink.
