#!/usr/bin/env python3
"""
Mock Matrix homeserver for Chatbox testing.
Mimics a subset of the Matrix client-server API v3.
"""

import json
import time
import uuid
from flask import Flask, request, jsonify, abort, Response

app = Flask(__name__)

# Configuration
VALID_TOKEN = "test_token_123"
PORT = 8008
KIOSK_USER = "@kiosk:localhost"

# In-memory state
sent_messages = []  # Messages sent via PUT /send
sync_counter = 0
fail_syncs_left = 0   # /_test/fail_sync: answer the next N syncs with 503
replay_next = False   # /_test/replay_sync: next incremental sync repeats all

# Media (photos, docs/photos.md): media_id -> (bytes, content type). Uploaded
# with the real upload endpoint; downloaded with the authenticated endpoint
# (Matrix 1.11) or, for clients on old servers, the legacy one.
media = {}
media_v1_enabled = True   # /_test/media_v1?enabled=0: old server, v1 is 404
media_downloads = []      # (endpoint, media_id) of every download, for tests

MOCK_ROOMS = {
    "!testroom:localhost": {
        "name": "TestUser",
        "members": [KIOSK_USER, "@testuser:localhost"],
    },
    "!room2:localhost": {
        "name": "AnotherUser",
        "members": [KIOSK_USER, "@another:localhost"],
    },
}

# Seed messages per room
MOCK_MESSAGES = {
    "!testroom:localhost": [
        {
            "event_id": "$evt001",
            "type": "m.room.message",
            "sender": "@testuser:localhost",
            "origin_server_ts": int(time.time() * 1000) - 300000,
            "content": {"msgtype": "m.text", "body": "Hey, how are you?"},
        },
        {
            "event_id": "$evt002",
            "type": "m.room.message",
            "sender": KIOSK_USER,
            "origin_server_ts": int(time.time() * 1000) - 240000,
            "content": {"msgtype": "m.text", "body": "I'm great, thanks!"},
        },
    ],
    "!room2:localhost": [
        {
            "event_id": "$evt003",
            "type": "m.room.message",
            "sender": KIOSK_USER,
            "origin_server_ts": int(time.time() * 1000) - 600000,
            "content": {"msgtype": "m.text", "body": "Hello!"},
        },
    ],
}


def check_auth():
    """Validate Bearer token. Returns 401 if wrong."""
    auth = request.headers.get("Authorization", "")
    if auth != f"Bearer {VALID_TOKEN}":
        abort(401, description="Invalid token")


@app.errorhandler(401)
def unauthorized(e):
    return jsonify({"errcode": "M_UNKNOWN_TOKEN", "error": str(e)}), 401


@app.errorhandler(404)
def not_found(e):
    return jsonify({"errcode": "M_NOT_FOUND", "error": str(e)}), 404


@app.route("/_matrix/client/v3/sync", methods=["GET"])
def sync():
    check_auth()
    global sync_counter, fail_syncs_left, replay_next
    if fail_syncs_left > 0:
        fail_syncs_left -= 1
        return jsonify({"errcode": "M_UNKNOWN", "error": "test: sync down"}), 503
    sync_counter += 1
    since = request.args.get("since", "")
    replay = bool(since) and replay_next
    replay_next = False

    # Build joined rooms response
    rooms_join = {}
    for room_id, room_info in MOCK_ROOMS.items():
        all_events = list(MOCK_MESSAGES.get(room_id, []))
        # Include sent messages for this room
        for m in sent_messages:
            if m.get("_room_id") == room_id:
                all_events.append(m)

        # On initial sync (no since), return all events
        # On incremental sync, return only new sent messages
        # A replay repeats everything, like a limited sync overlapping what
        # the client has.
        if since and not replay:
            events = [m for m in all_events if m.get("_new", False)]
        else:
            events = all_events

        # Strip internal fields
        clean_events = []
        for e in events:
            ce = {k: v for k, v in e.items() if not k.startswith("_")}
            clean_events.append(ce)

        rooms_join[room_id] = {
            "timeline": {
                "events": clean_events,
                "limited": replay,
                "prev_batch": f"batch_{sync_counter - 1}",
            },
            "state": {"events": []},
        }

    # Mark all sent messages as no longer new
    for m in sent_messages:
        m["_new"] = False

    return jsonify({
        "next_batch": f"batch_{sync_counter}",
        "rooms": {"join": rooms_join},
    })


@app.route("/_matrix/client/v3/rooms/<path:room_id>/messages", methods=["GET"])
def get_messages(room_id):
    check_auth()
    limit = int(request.args.get("limit", 25))
    direction = request.args.get("dir", "b")

    all_msgs = list(MOCK_MESSAGES.get(room_id, []))
    for m in sent_messages:
        if m.get("_room_id") == room_id:
            all_msgs.append(m)

    # Sort by origin_server_ts
    all_msgs.sort(key=lambda m: m.get("origin_server_ts", 0),
                  reverse=(direction == "b"))
    all_msgs = all_msgs[:limit]

    # Strip internal fields
    clean = []
    for e in all_msgs:
        clean.append({k: v for k, v in e.items() if not k.startswith("_")})

    return jsonify({
        "chunk": clean,
        "start": "start_token",
        "end": "end_token",
    })


@app.route("/_matrix/client/v3/rooms/<path:room_id>/send/<event_type>/<txn_id>",
           methods=["PUT"])
def send_event(room_id, event_type, txn_id):
    check_auth()
    body = request.get_json(force=True, silent=True)
    if not body:
        return jsonify({"errcode": "M_BAD_JSON", "error": "Invalid JSON"}), 400

    event_id = f"$sent-{txn_id}"
    event = {
        "event_id": event_id,
        "type": event_type,
        "sender": KIOSK_USER,
        "origin_server_ts": int(time.time() * 1000),
        "content": body,
        # Synapse returns the transaction ID to the device that sent it:
        # the client matches its local echo with it.
        "unsigned": {"transaction_id": txn_id},
        "_room_id": room_id,
        "_new": True,
    }
    sent_messages.append(event)

    return jsonify({"event_id": event_id})


@app.route("/_matrix/client/v3/account/whoami", methods=["GET"])
def whoami():
    check_auth()
    return jsonify({
        "user_id": KIOSK_USER,
        "device_id": "CHATBOX",
    })


@app.route("/_matrix/client/v3/login", methods=["POST"])
def login():
    body = request.get_json(force=True, silent=True)
    return jsonify({
        "user_id": KIOSK_USER,
        "access_token": VALID_TOKEN,
        "device_id": "CHATBOX",
    })


# ---- Media ----------------------------------------------------------------

@app.route("/_matrix/media/v3/upload", methods=["POST"])
def upload_media():
    check_auth()
    data = request.get_data()
    media_id = uuid.uuid4().hex[:24]
    media[media_id] = (data, request.headers.get("Content-Type", "application/octet-stream"))
    return jsonify({"content_uri": f"mxc://localhost/{media_id}"})


def serve_media(server, media_id, endpoint):
    if server != "localhost" or media_id not in media:
        return jsonify({"errcode": "M_NOT_FOUND", "error": "no such media"}), 404
    media_downloads.append((endpoint, media_id))
    data, ctype = media[media_id]
    return Response(data, mimetype=ctype)


@app.route("/_matrix/client/v1/media/download/<server>/<media_id>", methods=["GET"])
@app.route("/_matrix/client/v1/media/download/<server>/<media_id>/<filename>", methods=["GET"])
def download_media_v1(server, media_id, filename=None):
    if not media_v1_enabled:
        return jsonify({"errcode": "M_UNRECOGNIZED", "error": "Unrecognized request"}), 404
    check_auth()
    return serve_media(server, media_id, "v1")


@app.route("/_matrix/media/v3/download/<server>/<media_id>", methods=["GET"])
@app.route("/_matrix/media/v3/download/<server>/<media_id>/<filename>", methods=["GET"])
def download_media_legacy(server, media_id, filename=None):
    return serve_media(server, media_id, "legacy")


@app.route("/_test/media_v1", methods=["POST"])
def test_media_v1():
    global media_v1_enabled
    media_v1_enabled = request.args.get("enabled", "1") != "0"
    return jsonify({"media_v1": media_v1_enabled})


@app.route("/_test/media_downloads", methods=["GET"])
def test_media_downloads():
    return jsonify({"data": [{"endpoint": e, "media_id": m} for e, m in media_downloads]})


# Test helper: an event from the other member of a room, as if sent from
# their phone (body: the event's content, e.g. an m.image). Shows up in the
# next sync like any new event.
@app.route("/_test/inject", methods=["POST"])
def test_inject():
    room_id = request.args.get("room", "!testroom:localhost")
    content = request.get_json(force=True, silent=True)
    if room_id not in MOCK_ROOMS or not isinstance(content, dict):
        return jsonify({"errcode": "M_BAD_JSON", "error": "room or content"}), 400
    sender = [m for m in MOCK_ROOMS[room_id]["members"] if m != KIOSK_USER][0]
    event_id = f"$inj-{uuid.uuid4().hex[:12]}"
    sent_messages.append({
        "event_id": event_id,
        "type": "m.room.message",
        "sender": sender,
        "origin_server_ts": int(time.time() * 1000),
        "content": content,
        "_room_id": room_id,
        "_new": True,
    })
    return jsonify({"event_id": event_id})


# Test helpers: make the next N syncs fail; make the next incremental sync
# repeat every event (overlap / limited).
@app.route("/_test/fail_sync", methods=["POST"])
def test_fail_sync():
    global fail_syncs_left
    fail_syncs_left = int(request.args.get("count", 1))
    return jsonify({"fail_syncs_left": fail_syncs_left})


@app.route("/_test/replay_sync", methods=["POST"])
def test_replay_sync():
    global replay_next
    replay_next = True
    return jsonify({"replay_next": True})


# Test helper: list all sent messages
@app.route("/_test/sent_messages", methods=["GET"])
def test_sent():
    check_auth()
    clean = []
    for m in sent_messages:
        clean.append({k: v for k, v in m.items() if not k.startswith("_")})
    return jsonify({"data": clean})


if __name__ == "__main__":
    print(f"Starting mock Matrix server on port {PORT}...")
    print(f"Valid token: {VALID_TOKEN}")
    app.run(host="0.0.0.0", port=PORT, debug=False)
