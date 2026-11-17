#!/usr/bin/env bash
# setup-pi-system.sh — One-time system setup for the Chatbox kiosk Pi.
#
# Run this ON the Pi as root, via sudo from the admin user, e.g.:
#   ssh pi@<PI_HOST> 'sudo bash -s' < deploy/setup-pi-system.sh
# or, after deploy-to-pi.sh has synced deploy/ to ~/chatbox-deploy/:
#   ssh pi@<PI_HOST> 'sudo bash ~/chatbox-deploy/setup-pi-system.sh'
# The second form also finds bt-speaker/ and the unit files next to it
# (piped via 'bash -s' it looks in ~$SUDO_USER/chatbox-deploy/ instead).
#
# Safe to re-run: every step is idempotent.
#
# This script does NOT install X11 or Wayland. The app runs directly on the
# Linux framebuffer (DRM/KMS) with libinput for keyboard input.

set -euo pipefail

echo "=== Chatbox Pi System Setup ==="

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run as root (sudo bash ...)." >&2
    exit 1
fi

# The admin user = whoever ran us through sudo (joins group chatbox so they
# can rsync music into /srv/music). Empty when run as plain root.
ADMIN_USER="${SUDO_USER:-}"
[ "${ADMIN_USER}" = "root" ] && ADMIN_USER=""

# Where the synced deploy/ tree is: next to this script if it is a real
# file, else ~ADMIN/chatbox-deploy (deploy-to-pi.sh syncs it there).
DEPLOY_DIR=""
SELF="${BASH_SOURCE[0]:-}"
if [ -n "${SELF}" ] && [ -f "${SELF}" ] && [ -d "$(dirname "${SELF}")/bt-speaker" ]; then
    DEPLOY_DIR="$(cd "$(dirname "${SELF}")" && pwd)"
elif [ -n "${ADMIN_USER}" ]; then
    ADMIN_HOME="$(getent passwd "${ADMIN_USER}" | cut -d: -f6)"
    if [ -d "${ADMIN_HOME}/chatbox-deploy" ]; then
        DEPLOY_DIR="${ADMIN_HOME}/chatbox-deploy"
    fi
fi

# ---------------------------------------------------------------------------
# 1. Install build dependencies
# ---------------------------------------------------------------------------
echo "[1/7] Installing system packages..."
apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    pkg-config \
    libdrm-dev \
    libgbm-dev \
    libinput-dev \
    libevdev-dev \
    libxkbcommon-dev \
    libcurl4-openssl-dev \
    libsqlite3-dev \
    libjpeg-dev \
    libpng-dev \
    sqlite3 \
    linux-perf \
    sysstat \
    mpv \
    ffmpeg \
    pipewire \
    pipewire-pulse \
    wireplumber \
    libspa-0.2-bluetooth \
    bluez
echo "      Packages installed."

# ---------------------------------------------------------------------------
# 2. Create the chatbox system user
# ---------------------------------------------------------------------------
echo "[2/7] Creating 'chatbox' user..."
if ! id -u chatbox &>/dev/null; then
    useradd \
        --system \
        --create-home \
        --home-dir /home/chatbox \
        --shell /bin/bash \
        --groups input,video,render,bluetooth \
        chatbox
    echo "      User 'chatbox' created."
else
    # Make sure the user is in the required groups even if it existed before
    usermod -aG input,video,render,bluetooth chatbox
    echo "      User 'chatbox' already exists — groups updated."
fi

# ---------------------------------------------------------------------------
# 3. Configure auto-login on tty1 via getty override
#    This ensures that when tty1 is started, it logs in as 'chatbox'
#    automatically (no password prompt), and then .bash_profile runs the app.
# ---------------------------------------------------------------------------
echo "[3/7] Configuring auto-login on tty1..."
GETTY_OVERRIDE_DIR=/etc/systemd/system/getty@tty1.service.d
mkdir -p "${GETTY_OVERRIDE_DIR}"
cat > "${GETTY_OVERRIDE_DIR}/autologin.conf" <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin chatbox --noclear %I $TERM
EOF
systemctl daemon-reload
systemctl enable getty@tty1
echo "      Auto-login configured."

# ---------------------------------------------------------------------------
# 4. Disable console blanking (kernel boot parameters)
#    Also disable the cursor blink so the screen stays clean.
# ---------------------------------------------------------------------------
echo "[4/7] Disabling console blanking..."

# Add kernel parameters to cmdline.txt if not already present
# (Bookworm: /boot/firmware/cmdline.txt; older releases: /boot/cmdline.txt)
CMDLINE_FILE=/boot/firmware/cmdline.txt
if [ ! -f "${CMDLINE_FILE}" ]; then
    CMDLINE_FILE=/boot/cmdline.txt
fi
if [ -f "${CMDLINE_FILE}" ]; then
    NEEDS_UPDATE=0
    if ! grep -q "consoleblank=0" "${CMDLINE_FILE}"; then
        sed -i 's/$/ consoleblank=0/' "${CMDLINE_FILE}"
        NEEDS_UPDATE=1
    fi
    if ! grep -q "logo.nologo" "${CMDLINE_FILE}"; then
        sed -i 's/$/ logo.nologo/' "${CMDLINE_FILE}"
        NEEDS_UPDATE=1
    fi
    if [ "${NEEDS_UPDATE}" -eq 1 ]; then
        echo "      Updated ${CMDLINE_FILE} (reboot required for effect)."
    else
        echo "      ${CMDLINE_FILE} already configured."
    fi
else
    echo "      WARNING: ${CMDLINE_FILE} not found — skipping kernel param update."
fi

# Also disable cursor blinking via /etc/rc.local (takes effect on next boot)
if ! grep -q "cursor" /etc/rc.local 2>/dev/null; then
    # Ensure rc.local exists and is executable
    if [ ! -f /etc/rc.local ]; then
        cat > /etc/rc.local <<'EOF'
#!/bin/bash
exit 0
EOF
        chmod +x /etc/rc.local
    fi
    sed -i '/^exit 0/i # Disable console cursor blinking\necho 0 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null || true\n' /etc/rc.local
    echo "      /etc/rc.local updated for cursor blink suppression."
fi

# ---------------------------------------------------------------------------
# 5. Create /etc/chatbox.conf from the example if it doesn't exist
# ---------------------------------------------------------------------------
echo "[5/7] Setting up /etc/chatbox.conf..."
CONF_EXAMPLE=/home/chatbox/chatbox/deploy/chatbox.conf.example
if [ -n "${DEPLOY_DIR}" ] && [ -f "${DEPLOY_DIR}/chatbox.conf.example" ]; then
    CONF_EXAMPLE="${DEPLOY_DIR}/chatbox.conf.example"
fi
CONF_DEST=/etc/chatbox.conf

if [ -f "${CONF_DEST}" ]; then
    echo "      /etc/chatbox.conf already exists — skipping."
else
    if [ -f "${CONF_EXAMPLE}" ]; then
        cp "${CONF_EXAMPLE}" "${CONF_DEST}"
        chmod 640 "${CONF_DEST}"
        chown root:chatbox "${CONF_DEST}"
        echo "      Created ${CONF_DEST} from example."
        echo "      IMPORTANT: Edit ${CONF_DEST} with your Matrix homeserver URL, token, and contacts."
    else
        # Write a minimal placeholder config
        cat > "${CONF_DEST}" <<'EOF'
# Chatbox configuration
# Edit this file with your Matrix homeserver details.

homeserver_url = http://CHANGE_ME:8008
access_token = CHANGE_ME

[contacts]
# Add contacts below in the format:
# Display Name = !roomId:server.name
EOF
        chmod 640 "${CONF_DEST}"
        chown root:chatbox "${CONF_DEST}"
        echo "      Created placeholder ${CONF_DEST} — EDIT BEFORE RUNNING."
    fi
fi

# ---------------------------------------------------------------------------
# 6. Create log file with correct permissions
# ---------------------------------------------------------------------------
echo "[6/7] Setting up log file..."
touch /var/log/chatbox.err
chown chatbox:chatbox /var/log/chatbox.err
chmod 644 /var/log/chatbox.err
echo "      /var/log/chatbox.err ready."

# ---------------------------------------------------------------------------
# 7. Music player (docs/player-spec.md §4, D6): one user, chatbox, runs the
#    kiosk, mpv, the indexer, PipeWire/WirePlumber and the BT reconnect loop.
# ---------------------------------------------------------------------------
echo "[7/7] Setting up music and audio..."
CHATBOX_UID="$(id -u chatbox)"

# /srv/music: chatbox:chatbox, setgid so new files stay in group chatbox.
install -d -o chatbox -g chatbox -m 2775 /srv/music
echo "      /srv/music ready (chatbox:chatbox 2775)."
# /srv/photos (docs/photos.md): photos sent in chat are saved here by the
# kiosk; the Photos app shows (and can delete) what is in it.
install -d -o chatbox -g chatbox -m 2775 /srv/photos
echo "      /srv/photos ready (chatbox:chatbox 2775)."

# The admin rsyncs / copies music in through group chatbox.
if [ -n "${ADMIN_USER}" ] && [ "${ADMIN_USER}" != "chatbox" ]; then
    usermod -aG chatbox "${ADMIN_USER}"
    echo "      '${ADMIN_USER}' is in group chatbox (log in again for it to apply)."
else
    echo "      Not run via sudo: add your admin user to group chatbox by hand"
    echo "      (usermod -aG chatbox <user>) to write to /srv/music."
fi

# Linger: chatbox's user manager (PipeWire, WirePlumber, the BT loop) starts
# at boot without a login, and /run/user/<uid> exists.
loginctl enable-linger chatbox
echo "      Linger enabled for chatbox (uid ${CHATBOX_UID})."

# Order the kiosk after /run/user/<uid> exists. Ordering only, and on the
# (instant) runtime-dir unit, not user@<uid>.service: chat must never wait
# for PipeWire. The player retries mpv/audio on its own. The uid is only
# known here, hence a drop-in instead of a line in chatbox.service.
KIOSK_DROPIN_DIR=/etc/systemd/system/chatbox.service.d
mkdir -p "${KIOSK_DROPIN_DIR}"
cat > "${KIOSK_DROPIN_DIR}/10-user-runtime-dir.conf" <<EOF
# Written by setup-pi-system.sh (chatbox uid ${CHATBOX_UID}).
[Unit]
After=user-runtime-dir@${CHATBOX_UID}.service
EOF
echo "      chatbox.service ordered after user-runtime-dir@${CHATBOX_UID}.service."

# PipeWire user units: Debian enables these globally; make sure.
systemctl --global enable pipewire.socket pipewire-pulse.socket wireplumber.service \
    >/dev/null 2>&1 || true

# BT speaker reconnect loop + WirePlumber seat override for chatbox
# (also migrates it off the admin user if an earlier install put it there).
if [ -n "${DEPLOY_DIR}" ] && [ -f "${DEPLOY_DIR}/bt-speaker/install.sh" ]; then
    bash "${DEPLOY_DIR}/bt-speaker/install.sh"
else
    echo "      WARNING: deploy/bt-speaker not found — BT reconnect not installed."
    echo "      deploy-to-pi.sh installs it, or re-run this script from ~/chatbox-deploy/."
fi

# Indexer: install the unit if we have it and enable it. deploy-to-pi.sh
# also installs the unit + binary and (re)starts it.
if [ -n "${DEPLOY_DIR}" ] && [ -f "${DEPLOY_DIR}/chatbox-indexer.service" ]; then
    install -m 644 "${DEPLOY_DIR}/chatbox-indexer.service" /etc/systemd/system/chatbox-indexer.service
fi
systemctl daemon-reload
if [ -f /etc/systemd/system/chatbox-indexer.service ]; then
    systemctl enable chatbox-indexer.service
    echo "      chatbox-indexer.service enabled."
else
    echo "      chatbox-indexer.service not installed yet — deploy-to-pi.sh installs and enables it."
fi

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
echo "=== Setup complete ==="
echo ""
echo "Next steps:"
echo "  1. Edit /etc/chatbox.conf with your Matrix homeserver URL, token, and contacts."
echo "  2. Run deploy/deploy-to-pi.sh from your dev machine to build and install the app."
echo "  3. Pair + trust the BT speaker once if it never was (bluetoothctl);"
echo "     see deploy/bt-speaker/README.md."
echo "  4. Reboot the Pi: sudo reboot"
echo ""
