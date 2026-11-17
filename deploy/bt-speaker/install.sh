#!/usr/bin/env bash
# install.sh — install the BT speaker auto-reconnect for the kiosk user.
#
# Runs ON the Pi as root. setup-pi-system.sh and deploy-to-pi.sh call it;
# by hand:
#   sudo bash ~/chatbox-deploy/bt-speaker/install.sh
#
# Installs, for user `chatbox` (docs/player-spec.md §4, D6):
#   bt-speaker-reconnect.sh      -> ~chatbox/.local/bin/bt-speaker-reconnect
#   bt-speaker-reconnect.service -> ~chatbox/.config/systemd/user/ (enabled)
#   80-disable-logind.conf       -> ~chatbox/.config/wireplumber/wireplumber.conf.d/
#
# Migration: earlier versions ran this as the admin user. Any other user
# under /home that still has bt-speaker-reconnect.service gets it disabled,
# stopped and renamed to *.disabled-by-chatbox, and its WirePlumber seat
# override renamed the same way, so that user's WirePlumber stops claiming
# the speaker. Runs once (the renamed files no longer match); nothing fails
# if absent.
#
# Idempotent. Needs the chatbox user and linger (setup-pi-system.sh).

set -euo pipefail

KIOSK_USER="${KIOSK_USER:-chatbox}"
UNIT=bt-speaker-reconnect.service
WP_CONF=80-disable-logind.conf
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "bt-speaker: run as root (sudo)" >&2
    exit 1
fi
if ! id -u "${KIOSK_USER}" &>/dev/null; then
    echo "bt-speaker: user '${KIOSK_USER}' does not exist; run setup-pi-system.sh first" >&2
    exit 1
fi

# systemctl --user for another user: talk to that user's manager directly
# through $XDG_RUNTIME_DIR/systemd/private (no D-Bus session needed).
# Returns 1 without doing anything if the user manager isn't running.
user_ctl() {
    local user="$1"; shift
    local uid
    uid="$(id -u "${user}")"
    systemctl is-active --quiet "user@${uid}.service" 2>/dev/null || return 1
    runuser -u "${user}" -- env XDG_RUNTIME_DIR="/run/user/${uid}" \
        systemctl --user "$@"
}

# ---------------------------------------------------------------------------
# 1. Migrate away from an earlier install (as the admin user, e.g. `pi`)
# ---------------------------------------------------------------------------
kiosk_home="$(getent passwd "${KIOSK_USER}" | cut -d: -f6)"
for old_unit in /home/*/.config/systemd/user/"${UNIT}"; do
    [ -e "${old_unit}" ] || continue
    old_home="${old_unit%/.config/systemd/user/"${UNIT}"}"
    [ "${old_home}" = "${kiosk_home}" ] && continue
    old_user="$(stat -c %U "${old_home}")"
    echo "bt-speaker: migrating from user '${old_user}' (${old_unit})"
    user_ctl "${old_user}" disable --now "${UNIT}" 2>/dev/null || true
    rm -f "${old_home}/.config/systemd/user/default.target.wants/${UNIT}"
    mv -f "${old_unit}" "${old_unit}.disabled-by-chatbox"
    old_wp="${old_home}/.config/wireplumber/wireplumber.conf.d/${WP_CONF}"
    if [ -e "${old_wp}" ]; then
        mv -f "${old_wp}" "${old_wp}.disabled-by-chatbox"
        user_ctl "${old_user}" restart wireplumber.service 2>/dev/null || true
        echo "bt-speaker: '${old_user}' no longer monitors bluez (${old_wp}.disabled-by-chatbox)"
    fi
done

# ---------------------------------------------------------------------------
# 2. Install for the kiosk user
# ---------------------------------------------------------------------------
bin_dir="${kiosk_home}/.local/bin"
unit_dir="${kiosk_home}/.config/systemd/user"
wants_dir="${unit_dir}/default.target.wants"
wp_dir="${kiosk_home}/.config/wireplumber/wireplumber.conf.d"

install -d -o "${KIOSK_USER}" -g "${KIOSK_USER}" -m 755 \
    "${kiosk_home}/.local" "${bin_dir}" \
    "${kiosk_home}/.config" "${kiosk_home}/.config/systemd" "${unit_dir}" "${wants_dir}" \
    "${kiosk_home}/.config/wireplumber" "${wp_dir}"

wp_changed=0
cmp -s "${here}/${WP_CONF}" "${wp_dir}/${WP_CONF}" || wp_changed=1

install -o "${KIOSK_USER}" -g "${KIOSK_USER}" -m 755 \
    "${here}/bt-speaker-reconnect.sh" "${bin_dir}/bt-speaker-reconnect"
install -o "${KIOSK_USER}" -g "${KIOSK_USER}" -m 644 \
    "${here}/${UNIT}" "${unit_dir}/${UNIT}"
install -o "${KIOSK_USER}" -g "${KIOSK_USER}" -m 644 \
    "${here}/${WP_CONF}" "${wp_dir}/${WP_CONF}"

# Enable by symlink: works whether or not the user manager is running yet.
ln -sfn "../${UNIT}" "${wants_dir}/${UNIT}"
chown -h "${KIOSK_USER}:${KIOSK_USER}" "${wants_dir}/${UNIT}"

# ---------------------------------------------------------------------------
# 3. Apply now if the user manager is up (linger starts it at boot)
# ---------------------------------------------------------------------------
if user_ctl "${KIOSK_USER}" daemon-reload; then
    if [ "${wp_changed}" -eq 1 ]; then
        user_ctl "${KIOSK_USER}" restart wireplumber.service || true
    fi
    user_ctl "${KIOSK_USER}" restart "${UNIT}" || true
    user_ctl "${KIOSK_USER}" --no-pager status "${UNIT}" 2>/dev/null | head -5 || true
else
    echo "bt-speaker: ${KIOSK_USER}'s user manager is not running; ${UNIT} starts at next boot (linger)"
fi
echo "bt-speaker: installed for ${KIOSK_USER}"
