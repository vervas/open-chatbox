#!/usr/bin/env bash
# setup-matrix-rooms.sh — create a direct room between the kiosk account and
# each family member, and print the [contacts] block for /etc/chatbox.conf.
#
# Run after the family members' accounts exist (setup-synapse.sh or
# register_new_matrix_user, docs/homeserver.md). They accept the invite in
# their Matrix app.
#
# Usage:
#   bash homeserver/setup-matrix-rooms.sh <homeserver_url> <kiosk_user> <kiosk_password> <user1> [user2] ...
#
# Example:
#   bash homeserver/setup-matrix-rooms.sh https://matrix.example.org kiosk 's3cret' parent1 parent2
#
# Only rooms for the users named are created; naming a user who already has
# a room with the kiosk gives them a second one. Each run logs in once (a new
# kiosk session); it does not print or replace the kiosk's access token.

set -euo pipefail

if [ $# -lt 4 ]; then
    echo "Usage: $0 <homeserver_url> <kiosk_user> <kiosk_password> <username1> [username2] ..."
    echo "Example: $0 https://matrix.example.org kiosk 's3cret' parent1 parent2"
    exit 1
fi

HOMESERVER="${1%/}"
KIOSK_USER="$2"
KIOSK_PASS="$3"
shift 3
USERS=("$@")

json() { python3 -c "import sys,json; print(json.load(sys.stdin)$1)"; }

# The server name (the part after ':' in user IDs) can differ from the URL's
# host, so ask the server.
LOGIN_RESP=$(curl -fsS -X POST "$HOMESERVER/_matrix/client/v3/login" \
    -H "Content-Type: application/json" \
    -d "{\"type\":\"m.login.password\",\"identifier\":{\"type\":\"m.id.user\",\"user\":\"$KIOSK_USER\"},\"password\":\"$KIOSK_PASS\",\"initial_device_display_name\":\"setup-matrix-rooms\"}")
TOKEN=$(echo "$LOGIN_RESP" | json "['access_token']")
KIOSK_ID=$(echo "$LOGIN_RESP" | json "['user_id']")
SERVER_NAME="${KIOSK_ID#*:}"

echo "=== Matrix room setup ==="
echo "Homeserver: $HOMESERVER"
echo "Kiosk:      $KIOSK_ID"
echo ""

ROOM_IDS=()
for user in "${USERS[@]}"; do
    full_user="@${user}:${SERVER_NAME}"
    echo "Creating a room with $full_user..."
    ROOM_RESP=$(curl -fsS -X POST "$HOMESERVER/_matrix/client/v3/createRoom" \
        -H "Authorization: Bearer $TOKEN" \
        -H "Content-Type: application/json" \
        -d "{
            \"is_direct\": true,
            \"invite\": [\"$full_user\"],
            \"preset\": \"trusted_private_chat\",
            \"name\": \"Chat with $user\"
        }")
    room_id=$(echo "$ROOM_RESP" | json "['room_id']")
    echo "  $room_id"
    ROOM_IDS+=("$room_id")
done

# This session was only needed to create the rooms.
curl -fsS -X POST "$HOMESERVER/_matrix/client/v3/logout" \
    -H "Authorization: Bearer $TOKEN" >/dev/null || true

echo ""
echo "=== Add to the [contacts] section of /etc/chatbox.conf ==="
echo "# (renumber N to follow your existing contacts; up to 10)"
n=1
for i in "${!USERS[@]}"; do
    user="${USERS[$i]}"
    display_name="$(echo "${user:0:1}" | tr '[:lower:]' '[:upper:]')${user:1}"
    echo "contact.$n.name = $display_name"
    echo "contact.$n.room_id = ${ROOM_IDS[$i]}"
    echo ""
    n=$((n + 1))
done
echo "Then restart the kiosk (sudo systemctl restart chatbox). Family members"
echo "accept the room invite in their Matrix app."
