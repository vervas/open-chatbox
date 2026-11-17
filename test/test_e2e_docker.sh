#!/usr/bin/env bash
# test_e2e_docker.sh — Docker-based end-to-end integration test
# Runs inside the e2e Docker service container.
# Assumes the mock Matrix server is running at http://mock:8008

set -euo pipefail

MOCK_URL="http://mock:8008"
TOKEN="test_token_123"
ROOM_ID="!testroom:localhost"
BUILD_DIR="${BUILD_DIR:-/app/pi/build-docker}"
BINARY="$BUILD_DIR/chatbox"
TEST_CONFIG="/app/test/fixtures/chatbox.conf"

LOG="$(mktemp)"   # chatbox's stderr

pass() { echo "[PASS] $*"; }
fail() { echo "[FAIL] $*" >&2; [ -s "$LOG" ] && { echo "--- chatbox log (tail) ---" >&2; tail -40 "$LOG" >&2; }; exit 1; }

# Wait up to $2 seconds for chatbox's log to match the regex $1.
wait_log() {
    for _ in $(seq 1 "$2"); do grep -Eq "$1" "$LOG" && return 0; sleep 1; done
    return 1
}

echo "=== Chatbox Docker E2E Test ==="

# Step 1: Verify mock server is reachable (wait up to 30s for it to start)
echo "Step 1: Verify mock server..."
for i in $(seq 1 30); do
    RESULT=$(curl -sf -H "Authorization: Bearer $TOKEN" "$MOCK_URL/_matrix/client/v3/account/whoami" 2>/dev/null) && break
    echo "  Waiting for mock server... ($i/30)"
    sleep 1
done
[ -n "$RESULT" ] || fail "Mock server unreachable after 30s"
echo "$RESULT" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'user_id' in d" || fail "Unexpected mock response"
pass "Mock server is up"

# Step 2: Build chatbox if binary is missing, then verify it exists
echo "Step 2: Verify/build chatbox binary..."
if [ ! -f "$BINARY" ]; then
    echo "  Binary not found, building..."
    mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR" && cmake /app/pi -DCHATBOX_BACKEND=SDL2 -DCMAKE_BUILD_TYPE=Release > /dev/null && make -j"$(nproc)" > /dev/null
    cd /app
fi
test -f "$BINARY" || fail "chatbox binary not found at $BINARY"
pass "chatbox binary exists"

# Step 2b: Run keyboard navigation tests (headless, no network needed)
echo "Step 2b: Run keyboard navigation tests..."
"$BUILD_DIR/test_keyboard" /app/test/fixtures/chatbox.conf 2>&1 || fail "keyboard navigation tests failed"
pass "keyboard navigation tests passed"

# Step 3: Start chatbox in background (headless SDL2), offline at boot: the
# first two syncs fail, as when the network isn't up yet.
echo "Step 3: Start chatbox in background (first 2 syncs fail)..."
curl -sf -X POST "$MOCK_URL/_test/fail_sync?count=2" > /dev/null || fail "mock: fail_sync"
SDL_VIDEODRIVER=dummy "$BINARY" "$TEST_CONFIG" 2>"$LOG" &
CHATBOX_PID=$!
echo "  PID: $CHATBOX_PID"
sleep 1

kill -0 "$CHATBOX_PID" 2>/dev/null || fail "chatbox exited immediately (crash?)"
pass "chatbox started without immediate crash"

# Step 3b: the session starts with whoami; once sync gets through, the
# initial sync seeds the timelines and the open chat (the first contact)
# shows its history, with no /messages fetch and no retry give-up.
echo "Step 3b: Wait for the first chat's history after the failed syncs..."
wait_log "chat: history shown for !testroom:localhost, 2 events" 25 \
    || fail "first chat's history never shown"
WHOAMI_LINE=$(grep -n "logged in as" "$LOG" | head -1 | cut -d: -f1)
SEED_LINE=$(grep -n "chat: !testroom:localhost seeded" "$LOG" | head -1 | cut -d: -f1)
[ -n "$WHOAMI_LINE" ] && [ -n "$SEED_LINE" ] && [ "$WHOAMI_LINE" -lt "$SEED_LINE" ] \
    || fail "whoami must answer before the initial sync seeds"
pass "whoami, then initial sync, then history (after 2 failed syncs)"

# Step 4: Send 3 test messages via mock Matrix API
echo "Step 4: Send 3 messages to mock..."
for i in 1 2 3; do
    curl -sf -X PUT \
        -H "Authorization: Bearer $TOKEN" \
        -H "Content-Type: application/json" \
        "$MOCK_URL/_matrix/client/v3/rooms/$(python3 -c 'import urllib.parse; print(urllib.parse.quote("!testroom:localhost"))')/send/m.room.message/e2e-test-$i" \
        -d "{\"msgtype\":\"m.text\",\"body\":\"E2E test message $i\"}" \
        > /dev/null
done
pass "3 messages sent to mock"

# Step 5: Wait for a poll cycle
echo "Step 5: Waiting 5 seconds for poll cycle..."
sleep 5

# Step 6: Verify chatbox still running (no crash)
echo "Step 6: Verify chatbox still running..."
kill -0 "$CHATBOX_PID" 2>/dev/null || fail "chatbox crashed during polling"
pass "chatbox still running after 5 seconds"

# Step 6b: the 3 messages reached the open chat's timeline once each.
wait_log "chat: !testroom:localhost \+3 new, 0 dup" 5 \
    || fail "the 3 new messages did not arrive exactly once"
pass "3 new messages, 0 duplicates"

# Step 6c: a sync repeating the whole timeline (overlap) adds nothing.
echo "Step 6c: Replay the timeline in the next sync..."
curl -sf -X POST "$MOCK_URL/_test/replay_sync" > /dev/null || fail "mock: replay_sync"
wait_log "chat: !testroom:localhost \+0 new, 5 dup" 10 \
    || fail "a repeated timeline was not deduplicated"
pass "repeated timeline: 0 new, 5 duplicates dropped"

# Step 6d: photos (docs/photos.md). A photo sent from a parent's phone (an
# m.image) is downloaded and saved into photos_dir, once; a "photo" that
# isn't one is skipped; on a server without authenticated media the legacy
# download endpoint is used.
PHOTOS_DIR=$(sed -n 's/^photos_dir *= *//p' "$TEST_CONFIG")
ROOM_Q=$(python3 -c 'import urllib.parse; print(urllib.parse.quote("!testroom:localhost"))')
if command -v ffmpeg > /dev/null && [ -n "$PHOTOS_DIR" ]; then
    echo "Step 6d: Photos from chat..."
    IMG="$(mktemp -d)/e2e.jpg"
    ffmpeg -loglevel error -f lavfi -i "testsrc=size=320x240:rate=1" -frames:v 1 "$IMG" \
        || fail "ffmpeg could not make a test jpeg"
    upload() {   # $1 file, $2 content type -> mxc uri
        curl -sf -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: $2" \
            --data-binary @"$1" "$MOCK_URL/_matrix/media/v3/upload?filename=x" \
            | python3 -c "import sys,json; print(json.load(sys.stdin)['content_uri'])"
    }
    inject_image() {   # $1 mxc uri
        curl -sf -X POST "$MOCK_URL/_test/inject?room=$ROOM_Q" -H "Content-Type: application/json" \
            -d "{\"msgtype\":\"m.image\",\"body\":\"../../IMG_0001.jpg\",\"url\":\"$1\",\"info\":{\"mimetype\":\"image/jpeg\",\"w\":320,\"h\":240}}" \
            > /dev/null || fail "mock: inject"
    }
    before=$(ls "$PHOTOS_DIR" 2>/dev/null | grep -c '\.jpg$' || true)
    inject_image "$(upload "$IMG" image/jpeg)"
    wait_log "photos: saved [0-9]{8}-[0-9]{6}-[0-9a-f]{16}\.jpg" 15 || fail "the chat photo was not saved"
    after=$(ls "$PHOTOS_DIR" | grep -c '\.jpg$' || true)
    [ "$after" -eq $((before + 1)) ] || fail "expected one new photo in $PHOTOS_DIR ($before -> $after)"
    ls -a "$PHOTOS_DIR" | grep -q '^\.incoming-' && fail "a temp file was left in $PHOTOS_DIR"
    curl -sf "$MOCK_URL/_test/media_downloads" | grep -q '"endpoint": *"v1"' \
        || fail "the authenticated media endpoint was not used"
    pass "chat photo saved into photos_dir via the authenticated endpoint"

    echo "not a photo" > "${IMG%.jpg}.txt"
    inject_image "$(upload "${IMG%.jpg}.txt" image/jpeg)"
    wait_log "is not a JPEG or PNG" 15 || fail "a fake photo was not refused"
    pass "a non-photo is refused"

    curl -sf -X POST "$MOCK_URL/_test/media_v1?enabled=0" > /dev/null || fail "mock: media_v1"
    inject_image "$(upload "$IMG" image/jpeg)"
    for _ in $(seq 1 15); do
        [ "$(grep -Ec 'photos: saved .*\.jpg' "$LOG")" -ge 2 ] && break
        sleep 1
    done
    [ "$(grep -Ec 'photos: saved .*\.jpg' "$LOG")" -ge 2 ] || fail "no legacy-endpoint fallback"
    curl -sf "$MOCK_URL/_test/media_downloads" | grep -q '"endpoint": *"legacy"' \
        || fail "the legacy media endpoint was not used"
    curl -sf -X POST "$MOCK_URL/_test/media_v1?enabled=1" > /dev/null
    pass "old server: the legacy download endpoint"
    kill -0 "$CHATBOX_PID" 2>/dev/null || fail "chatbox crashed on photos"
else
    echo "Step 6d: skipped (no ffmpeg or no photos_dir in the config)"
fi

# Step 7: Use test_api to validate API client
echo "Step 7: Run test_api..."
"$BUILD_DIR/test_api" "$MOCK_URL" "$TOKEN" 2>&1 | grep -q "0 failure" || fail "test_api failed"
pass "test_api test passed"

# Step 8: Verify mock received the sent messages
echo "Step 8: Verify mock received sent messages..."
SENT=$(curl -sf -H "Authorization: Bearer $TOKEN" "$MOCK_URL/_test/sent_messages")
COUNT=$(echo "$SENT" | python3 -c "import sys,json; d=json.load(sys.stdin); print(len(d.get('data',[])))")
test "$COUNT" -gt 0 || fail "No sent messages recorded in mock"
pass "Mock has $COUNT sent message(s)"

# Cleanup: kill chatbox
echo "Cleanup: stopping chatbox..."
kill "$CHATBOX_PID" 2>/dev/null || true
wait "$CHATBOX_PID" 2>/dev/null || true
rm -f "$LOG"

echo ""
echo "=== All steps passed ==="
exit 0
