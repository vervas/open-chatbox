#!/usr/bin/env bash
# Keep a paired + trusted Bluetooth speaker connected.
# Polls every BT_SPEAKER_INTERVAL seconds; if the speaker isn't connected,
# tries `bluetoothctl connect`. Logs only on state changes.
set -u

MAC="${BT_SPEAKER_MAC:?set BT_SPEAKER_MAC}"
INTERVAL="${BT_SPEAKER_INTERVAL:-20}"
VOLUME="${BT_SPEAKER_VOLUME:-}"   # e.g. 0.8; empty = leave WirePlumber's remembered volume

connected=unknown
while true; do
  if bluetoothctl info "$MAC" 2>/dev/null | grep -q "Connected: yes"; then
    [ "$connected" != yes ] && echo "speaker $MAC connected"
    connected=yes
  else
    [ "$connected" = yes ] && echo "speaker $MAC disconnected, will retry every ${INTERVAL}s"
    connected=no
    if timeout 15 bluetoothctl connect "$MAC" 2>&1 | grep -q "Connection successful"; then
      echo "speaker $MAC reconnected"
      connected=yes
      if [ -n "$VOLUME" ]; then
        sleep 3   # give WirePlumber a moment to create the sink
        wpctl set-volume @DEFAULT_AUDIO_SINK@ "$VOLUME"
      fi
    fi
  fi
  sleep "$INTERVAL"
done
