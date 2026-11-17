#!/usr/bin/env bash
# deploy-to-pi.sh — Build and deploy the Chatbox app to a Raspberry Pi.
#
# Run this from your dev machine (repo root):
#   PI_HOST=chatbox.local ./deploy/deploy-to-pi.sh
#
# Requirements:
#   - SSH key-based auth: ssh pi@<PI_HOST> must work without a password prompt
#   - setup-pi-system.sh has already been run on the Pi
#   - /etc/chatbox.conf has been populated on the Pi
#
# Installs two binaries and their system units: chatbox (the kiosk,
# chatbox.service) and chatbox-indexer (the music indexer,
# chatbox-indexer.service), plus the BT speaker reconnect user unit for
# chatbox (deploy/bt-speaker/install.sh).

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
PI_HOST="${PI_HOST:-}"
PI_USER="${PI_USER:-pi}"
REMOTE_DIR="/home/${PI_USER}/chatbox"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# CHATBOX_PROFILE=ON enables main-loop watchdog, section timers, and the
# LVGL FPS / heap on-screen monitors. Off by default — opt in per deploy:
#   CHATBOX_PROFILE=ON PI_HOST=… ./deploy/deploy-to-pi.sh
CHATBOX_PROFILE="${CHATBOX_PROFILE:-OFF}"

if [ -z "${PI_HOST}" ]; then
    echo "ERROR: PI_HOST is not set."
    echo "Usage: PI_HOST=chatbox.local ./deploy/deploy-to-pi.sh"
    exit 1
fi

echo "=== Chatbox Deploy to Pi ==="
echo "    Target:    ${PI_USER}@${PI_HOST}:${REMOTE_DIR}"
echo "    Profiling: ${CHATBOX_PROFILE}"
echo ""

# ---------------------------------------------------------------------------
# 1. Rsync the pi/ directory to the Pi
# ---------------------------------------------------------------------------
echo "[1/4] Syncing source to ${PI_USER}@${PI_HOST}:${REMOTE_DIR}..."
rsync -avz --delete \
    --exclude 'build/' \
    --exclude '.git/' \
    "${REPO_ROOT}/pi/" \
    "${PI_USER}@${PI_HOST}:${REMOTE_DIR}/"
echo "      Sync complete."

# Also sync the deploy directory so the service file and conf example are available
echo "      Syncing deploy/ files..."
rsync -avz \
    "${REPO_ROOT}/deploy/" \
    "${PI_USER}@${PI_HOST}:/home/${PI_USER}/chatbox-deploy/"
echo "      Deploy files synced."

# ---------------------------------------------------------------------------
# 1b. Deploy emoji assets to /usr/share/chatbox/emoji/
# ---------------------------------------------------------------------------
echo "      Deploying emoji assets..."
ssh "${PI_USER}@${PI_HOST}" "sudo mkdir -p /usr/share/chatbox/emoji"
rsync -avz \
    "${REPO_ROOT}/pi/src/assets/emoji/" \
    "${PI_USER}@${PI_HOST}:/tmp/chatbox-emoji/"
ssh "${PI_USER}@${PI_HOST}" "sudo cp /tmp/chatbox-emoji/*.png /usr/share/chatbox/emoji/ && rm -rf /tmp/chatbox-emoji"
echo "      Emoji assets deployed."

# ---------------------------------------------------------------------------
# 1c. Deploy avatar images to /usr/share/chatbox/avatars/
# ---------------------------------------------------------------------------
if [ -d "${REPO_ROOT}/avatars" ]; then
    echo "      Deploying avatar images..."
    ssh "${PI_USER}@${PI_HOST}" "sudo mkdir -p /usr/share/chatbox/avatars"
    rsync -avz \
        "${REPO_ROOT}/avatars/" \
        "${PI_USER}@${PI_HOST}:/tmp/chatbox-avatars/"
    ssh "${PI_USER}@${PI_HOST}" "sudo cp /tmp/chatbox-avatars/*.png /usr/share/chatbox/avatars/ && rm -rf /tmp/chatbox-avatars"
    echo "      Avatar images deployed."
else
    echo "      No avatars/ directory found, skipping avatar deploy."
fi

# ---------------------------------------------------------------------------
# 1d. Ensure HDMI config for a 1280x800 HDMI display
# ---------------------------------------------------------------------------
echo "      Checking /boot/config.txt for HDMI settings..."
ssh "${PI_USER}@${PI_HOST}" bash <<'REMOTE_HDMI'
set -euo pipefail
BOOT_CFG="/boot/config.txt"
if [ ! -f "$BOOT_CFG" ]; then
    BOOT_CFG="/boot/firmware/config.txt"
fi
CHANGED=0
for LINE in "hdmi_cvt=1280 800 60 3" "hdmi_group=2" "hdmi_mode=87"; do
    if ! grep -qF "$LINE" "$BOOT_CFG" 2>/dev/null; then
        echo "$LINE" | sudo tee -a "$BOOT_CFG" > /dev/null
        CHANGED=1
    fi
done
if [ "$CHANGED" -eq 1 ]; then
    echo "HDMI settings added to $BOOT_CFG (reboot needed for display change)"
else
    echo "HDMI settings already present"
fi
REMOTE_HDMI

# ---------------------------------------------------------------------------
# 2. Build on the Pi (native ARM, framebuffer backend)
# ---------------------------------------------------------------------------
echo "[2/4] Building on Pi (ARM, FB backend, profile=${CHATBOX_PROFILE})..."
# Unquoted on purpose: REMOTE_DIR / CHATBOX_PROFILE expand locally.
# shellcheck disable=SC2087
ssh "${PI_USER}@${PI_HOST}" bash <<REMOTE_BUILD
set -euo pipefail
cd "${REMOTE_DIR}"
# Re-run CMake from a clean cache so toggling CHATBOX_PROFILE between deploys
# actually rebuilds LVGL with the new define. (lv_conf.h gates monitors on it.)
rm -rf build
mkdir -p build
cd build
cmake -DCHATBOX_BACKEND=FB -DCHATBOX_PROFILE=${CHATBOX_PROFILE} -DCMAKE_BUILD_TYPE=Release ..
make -j4
echo "Build successful."
REMOTE_BUILD
echo "      Pi build complete."

# ---------------------------------------------------------------------------
# 3. Install the binary
# ---------------------------------------------------------------------------
echo "[3/4] Installing binaries to /usr/local/bin/{chatbox,chatbox-indexer}..."
ssh "${PI_USER}@${PI_HOST}" bash <<'REMOTE_INSTALL'
set -euo pipefail
sudo install -m 755 ~/chatbox/build/chatbox /usr/local/bin/chatbox
echo "Binary installed: $(ls -lh /usr/local/bin/chatbox)"
# CMake skips the indexer without libsqlite3-dev (setup-pi-system.sh
# installs it). Chat still deploys; music then shows "No music yet".
if [ -f ~/chatbox/build/chatbox-indexer ]; then
    sudo install -m 755 ~/chatbox/build/chatbox-indexer /usr/local/bin/chatbox-indexer
    echo "Binary installed: $(ls -lh /usr/local/bin/chatbox-indexer)"
else
    echo "WARNING: chatbox-indexer was not built (libsqlite3-dev missing? re-run setup-pi-system.sh)"
fi
# Photos (docs/photos.md): without libjpeg-dev / libpng-dev the kiosk still
# builds, but photos show a placeholder.
if ! pkg-config --exists libjpeg || ! pkg-config --exists libpng; then
    echo "WARNING: libjpeg-dev / libpng-dev missing: photos won't show (re-run setup-pi-system.sh)"
fi
# The default photo folder (setup-pi-system.sh makes it too). The kiosk runs
# as chatbox and can't create it under /srv itself.
if [ ! -d /srv/photos ]; then
    sudo install -d -o chatbox -g chatbox -m 2775 /srv/photos
    echo "Created /srv/photos (chatbox:chatbox 2775)"
fi
REMOTE_INSTALL
echo "      Binaries installed."

# ---------------------------------------------------------------------------
# 4. Install and start the systemd service
# ---------------------------------------------------------------------------
echo "[4/4] Installing systemd services and starting chatbox + chatbox-indexer..."
ssh "${PI_USER}@${PI_HOST}" bash <<'REMOTE_SERVICE'
set -euo pipefail

# Stop any running chatbox (old getty-based or systemd)
sudo killall chatbox 2>/dev/null || true

# Disable getty auto-login on tty1 (no longer needed)
sudo systemctl disable getty@tty1.service 2>/dev/null || true
sudo systemctl stop getty@tty1.service 2>/dev/null || true

# Remove old bash_profile launcher if present
sudo rm -f /home/chatbox/.bash_profile

# Install and enable the systemd service
sudo install -m 644 ~/chatbox-deploy/chatbox.service /etc/systemd/system/chatbox.service
sudo systemctl daemon-reload
sudo systemctl enable chatbox.service
sudo systemctl restart chatbox.service

# Music indexer (logs to the journal: journalctl -u chatbox-indexer)
sudo install -m 644 ~/chatbox-deploy/chatbox-indexer.service /etc/systemd/system/chatbox-indexer.service
sudo systemctl daemon-reload
sudo systemctl enable chatbox-indexer.service
sudo systemctl restart chatbox-indexer.service

# BT speaker reconnect loop for chatbox (idempotent)
sudo bash ~/chatbox-deploy/bt-speaker/install.sh || echo "WARNING: bt-speaker install failed"

sleep 2
if systemctl is-active --quiet chatbox.service; then
    echo "chatbox.service is running"
else
    echo "WARNING: chatbox.service failed to start"
    journalctl -u chatbox.service --no-pager -n 10
fi
if systemctl is-active --quiet chatbox-indexer.service; then
    echo "chatbox-indexer.service is running"
else
    echo "WARNING: chatbox-indexer.service is not running"
    sudo journalctl -u chatbox-indexer.service --no-pager -n 10
fi
REMOTE_SERVICE
echo "      Services installed and started."

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
echo "=== Deploy complete ==="
echo ""
echo "Verify:"
echo "  ssh ${PI_USER}@${PI_HOST} \"pgrep chatbox && echo 'chatbox is running'\""
echo "  ssh ${PI_USER}@${PI_HOST} \"cat /var/log/chatbox.err\""
echo "  ssh ${PI_USER}@${PI_HOST} \"sudo journalctl -u chatbox-indexer -n 20\""
echo ""
echo "To stop for maintenance:"
echo "  ssh ${PI_USER}@${PI_HOST} \"sudo killall chatbox\""
echo ""
