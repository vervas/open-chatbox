FROM debian:bookworm-slim

# ffmpeg and mpv are test-only: with them on PATH, test_indexer runs its real
# ffprobe/ffmpeg integration test and test_player its real-mpv suite (both
# skip themselves otherwise). Bookworm's mpv/ffmpeg are the versions the Pi
# runs, so the Linux-only IPC and probe paths get exercised off the Pi.
RUN apt-get update && apt-get install -y \
    build-essential cmake git pkg-config \
    libdrm-dev libgbm-dev libinput-dev libevdev-dev \
    libxkbcommon-dev libcurl4-openssl-dev libsqlite3-dev \
    libjpeg-dev libpng-dev \
    libsdl2-dev \
    python3 python3-pip python3-flask \
    curl jq \
    && apt-get install -y --no-install-recommends ffmpeg mpv \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
