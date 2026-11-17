#!/usr/bin/env bash
# test_docker.sh — `docker-compose run test`: build into pi/build-docker, run
# every unit test binary, then the e2e script against the mock server.
# Runs inside the Debian build image (docker/Dockerfile).

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-/app/pi/build-docker}"
CONF="/app/test/fixtures/chatbox.conf"
export BUILD_DIR

echo "=== Build (${BUILD_DIR}) ==="
mkdir -p "$BUILD_DIR"
cmake -S /app/pi -B "$BUILD_DIR" -DCHATBOX_BACKEND=SDL2 > /dev/null
cmake --build "$BUILD_DIR" -j"$(nproc)" > /dev/null
echo "Build complete"

failed=0
run() {
    local name="$1"; shift
    echo ""
    echo "=== ${name} ==="
    if "$BUILD_DIR/$name" "$@" > "/tmp/${name}.log" 2>&1; then
        tail -n 3 "/tmp/${name}.log"
    else
        tail -n 40 "/tmp/${name}.log"
        echo "[FAIL] ${name}"
        failed=1
    fi
}

run test_config                 # self-tests (assertions)
run test_config     "$CONF"     # parses the fixture
run test_keyboard   "$CONF"
run test_async
run test_msg_cache
run test_sync_merge
run test_key_filter
run test_lang
run test_player
run test_indexer
run test_library
run test_photos

[ "$failed" -eq 0 ] || { echo ""; echo "Unit tests FAILED"; exit 1; }

echo ""
bash /app/test/test_e2e_docker.sh
