#!/usr/bin/env bash
# setup-synapse.sh — bootstrap a fresh Synapse homeserver for chatbox.
#
# Run on the machine that will host Synapse (any Linux or macOS box with
# Docker and docker-compose):
#   export SERVER_NAME=matrix.example.org
#   bash homeserver/setup-synapse.sh
#
# It generates the Synapse config (open registration stays off), starts the
# container on http://127.0.0.1:8008 (SYNAPSE_PORT to change), registers the `kiosk` account and any
# family accounts you type in, and prints the kiosk's access token.
#
# TLS is not handled here: put a reverse proxy in front (Caddy, nginx, ...),
# see Caddyfile.example and docs/homeserver.md. SERVER_NAME is the public
# name clients reach (https://$SERVER_NAME) and becomes the second half of
# every user ID (@kiosk:$SERVER_NAME); it can't be changed later.
#
# SAFETY: refuses to run if homeserver/files/homeserver.db already exists,
# because it wipes files/ (database, signing key, media). To deliberately
# start over: FORCE_WIPE=1.

set -euo pipefail

SERVER_NAME="${SERVER_NAME:-}"
if [ -z "$SERVER_NAME" ]; then
    echo "ERROR: SERVER_NAME is not set, e.g.: export SERVER_NAME=matrix.example.org"
    exit 1
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FILES="$SCRIPT_DIR/files"
SYNAPSE_PORT="${SYNAPSE_PORT:-8008}"     # host port, as in docker-compose.yml
export SYNAPSE_PORT
HS_URL="http://127.0.0.1:$SYNAPSE_PORT"
CONTAINER_URL="http://localhost:8008"   # Synapse as seen from inside its container
KIOSK_USER="${KIOSK_USER:-kiosk}"

compose() { (cd "$SCRIPT_DIR" && docker-compose "$@"); }

echo "=== Synapse setup for $SERVER_NAME ==="

# ---------------------------------------------------------------------------
# 1. Preflight
# ---------------------------------------------------------------------------
echo "[1/5] Preflight checks..."
docker info &>/dev/null || { echo "ERROR: Docker is not available."; exit 1; }
command -v docker-compose &>/dev/null || { echo "ERROR: docker-compose not found."; exit 1; }

if [ -f "$FILES/homeserver.db" ] && [ "${FORCE_WIPE:-0}" != "1" ]; then
    echo "ERROR: $FILES/homeserver.db exists: this looks like a live server."
    echo "This script wipes files/ (all messages, accounts, signing key). Refusing."
    echo "To change an existing install, edit files/homeserver.yaml instead."
    echo "To really start from scratch: FORCE_WIPE=1 bash $0"
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Clean slate
# ---------------------------------------------------------------------------
echo "[2/5] Cleaning up..."
compose down 2>/dev/null || true
find "$FILES" -mindepth 1 ! -name .gitignore -exec rm -rf {} + 2>/dev/null || true
mkdir -p "$FILES"

# ---------------------------------------------------------------------------
# 3. Generate the config (the image's own generator)
# ---------------------------------------------------------------------------
echo "[3/5] Generating config..."
docker run --rm \
    -v "$FILES:/data" \
    -e SYNAPSE_SERVER_NAME="$SERVER_NAME" \
    -e SYNAPSE_REPORT_STATS=no \
    docker.io/matrixdotorg/synapse:latest generate

# The generated listener is plain HTTP on 8008 with x_forwarded: true (the
# reverse proxy sets X-Forwarded-For), and enable_registration is off: the
# only way to make an account is the shared-secret admin registration below.
[ -f "$FILES/homeserver.yaml" ] || {
    echo "ERROR: no $FILES/homeserver.yaml after generate."
    echo "Is $FILES visible to the Docker VM? (Docker Desktop / Colima share your home folder by default.)"
    exit 1
}
grep -q '^enable_registration: true' "$FILES/homeserver.yaml" \
    && { echo "ERROR: open registration is on in homeserver.yaml"; exit 1; }
echo "  Done (files/homeserver.yaml)."

# ---------------------------------------------------------------------------
# 4. Start Synapse
# ---------------------------------------------------------------------------
echo "[4/5] Starting Synapse..."
compose up -d
for i in $(seq 1 30); do
    curl -fsS "$HS_URL/health" &>/dev/null && break
    echo "  waiting... ($i)"
    sleep 1
done
curl -fsS "$HS_URL/_matrix/client/versions" | grep -q versions \
    || { echo "ERROR: Synapse not responding on $HS_URL"; compose logs synapse; exit 1; }
echo "  Synapse is up on $HS_URL."

# ---------------------------------------------------------------------------
# 5. Accounts
# ---------------------------------------------------------------------------
echo "[5/5] Creating accounts..."

# register_new_matrix_user ships in the image and reads
# registration_shared_secret from homeserver.yaml.
register_user() {
    local user="$1" pass="$2" admin_flag="$3"
    compose exec -T synapse register_new_matrix_user \
        -c /data/homeserver.yaml -u "$user" -p "$pass" "$admin_flag" "$CONTAINER_URL" </dev/null
}

KIOSK_PASS=$(openssl rand -hex 16)
register_user "$KIOSK_USER" "$KIOSK_PASS" --no-admin

LOGIN_RESP=$(curl -sS -X POST "$HS_URL/_matrix/client/v3/login" \
    -H "Content-Type: application/json" \
    -d "{\"type\":\"m.login.password\",\"identifier\":{\"type\":\"m.id.user\",\"user\":\"$KIOSK_USER\"},\"password\":\"$KIOSK_PASS\",\"initial_device_display_name\":\"chatbox\"}")
TOKEN=$(echo "$LOGIN_RESP" | python3 -c "import sys,json; print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null || true)
[ -n "$TOKEN" ] || { echo "  ERROR: login failed: $LOGIN_RESP"; exit 1; }

echo ""
echo "  @$KIOSK_USER:$SERVER_NAME password:     $KIOSK_PASS"
echo "  @$KIOSK_USER:$SERVER_NAME access_token: $TOKEN"
echo ""

FAMILY_USERS=()
echo "  Family member usernames (one per line, empty to finish):"
while true; do
    read -rp "  Username: " username || break
    [ -z "$username" ] && break
    family_pass=$(openssl rand -hex 16)
    register_user "$username" "$family_pass" --no-admin
    echo "    @$username:$SERVER_NAME  password: $family_pass"
    FAMILY_USERS+=("$username")
done

echo ""
echo "=========================================="
echo "  Homeserver:  https://$SERVER_NAME  (once your reverse proxy is up)"
echo "  Kiosk token: $TOKEN"
echo "=========================================="
echo ""
echo "Next steps:"
echo "  1. Reverse proxy with TLS in front of $HS_URL (homeserver/Caddyfile.example)."
echo "  2. Family: sign in to a Matrix app (e.g. Element) with homeserver https://$SERVER_NAME"
echo "  3. Rooms + the [contacts] block for /etc/chatbox.conf:"
echo "     bash homeserver/setup-matrix-rooms.sh https://$SERVER_NAME $KIOSK_USER <kiosk_password> ${FAMILY_USERS[*]:-<user> ...}"
echo ""
