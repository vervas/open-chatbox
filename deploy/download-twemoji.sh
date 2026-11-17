#!/usr/bin/env bash
#
# download-twemoji.sh — Download all Twemoji PNGs and resize for chatbox
#
# Downloads ~3700 72x72 Twemoji PNGs from GitHub, resizes to 48x48,
# and outputs to pi/src/assets/emoji/ for the imgfont callback.
#
set -euo pipefail

REPO="jdecked/twemoji"
TAG="v15.1.0"
URL="https://github.com/${REPO}/archive/refs/tags/${TAG}.tar.gz"
OUTDIR="$(cd "$(dirname "$0")/.." && pwd)/pi/src/assets/emoji"
TMPDIR=$(mktemp -d)

cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

echo "==> Downloading Twemoji ${TAG}..."
curl -fsSL "$URL" -o "$TMPDIR/twemoji.tar.gz"

echo "==> Extracting 72x72 PNGs..."
tar -xzf "$TMPDIR/twemoji.tar.gz" -C "$TMPDIR" --strip-components=1 \
    "twemoji-${TAG#v}/assets/72x72"

SRC="$TMPDIR/assets/72x72"
mkdir -p "$OUTDIR"

# Check for ImageMagick
if command -v magick &>/dev/null; then
    CONVERT="magick"
elif command -v convert &>/dev/null; then
    CONVERT="convert"
else
    echo "ERROR: ImageMagick not found. Install it first." >&2
    exit 1
fi

echo "==> Resizing to 48x48 into ${OUTDIR}..."
count=0
for png in "$SRC"/*.png; do
    base=$(basename "$png")
    # Twemoji uses lowercase hex with dashes for multi-codepoint sequences.
    # Our imgfont callback expects lowercase hex filenames like "1f600.png".
    $CONVERT "$png" -resize 48x48 "$OUTDIR/$base"
    count=$((count + 1))
done

echo "==> Done. ${count} emoji PNGs in ${OUTDIR}"
