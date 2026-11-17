# Chatbox Makefile
#
# Usage:
#   make build    — native build (SDL2 on Mac/Linux, FB on Pi)
#   make run      — build and run the UI
#   make deploy   — build and deploy to Pi (reads PI_HOST/PI_USER from .env)
#   make test     — run keyboard navigation tests
#   make screenshot — capture Pi display to screenshots/<timestamp>.png
#   make diff       — diff two most recent screenshots (or BEFORE=… AFTER=…)
#   make avatar IN=photo.heic OUT=avatars/mom.png  — convert image to avatar PNG
#   make fonts    — regenerate pi/src/font_*.c (needs npx + lv_font_conv)
#   make clean    — remove build artifacts
#
# Create a .env file with:
#   PI_HOST=chatbox.local   (the Pi's hostname or IP)
#   PI_USER=pi              (an admin user with sudo)

SHELL := /bin/bash

# Detect platform
UNAME := $(shell uname -s)
ARCH  := $(shell uname -m)

# Determine backend: FB on aarch64 Linux (Pi), SDL2 everywhere else
ifeq ($(UNAME),Linux)
  ifeq ($(ARCH),aarch64)
    BACKEND := FB
  else
    BACKEND := SDL2
  endif
else
  BACKEND := SDL2
endif

BUILD_DIR := pi/build-native
NPROC     := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
BINARY    := $(BUILD_DIR)/chatbox
TEST_BIN  := $(BUILD_DIR)/test_keyboard
CONFIG    := test/fixtures/chatbox.conf
PI_CONFIG := pi-local/chatbox.conf

# Load .env if it exists
-include .env
export PI_HOST PI_USER

.PHONY: build run run-pi pull-pi-config deploy test screenshot diff avatar fonts clean

build:
	# CHATBOX_PROFILE is not passed here — defaults to OFF on first cmake.
	# CMakeCache.txt in $(BUILD_DIR) sticks any previously-set value,
	# so `make clean && make build` for a deterministic OFF rebuild,
	# or invoke cmake directly with -DCHATBOX_PROFILE=ON.
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake -DCHATBOX_BACKEND=$(BACKEND) ../../pi && make -j$(NPROC)
	@echo "Build complete: $(BINARY) ($(BACKEND))"

run: build
ifeq ($(BACKEND),FB)
	$(BINARY) /etc/chatbox.conf
else
	$(BINARY) $(CONFIG)
endif

pull-pi-config:
	@if [ -z "$(PI_HOST)" ]; then \
		echo "ERROR: PI_HOST not set. Create a .env file with PI_HOST=<host>"; \
		exit 1; \
	fi
	@mkdir -p pi-local avatars
	ssh $${PI_USER:-pi}@$(PI_HOST) "sudo cat /etc/chatbox.conf" > $(PI_CONFIG)
	rsync -avz $${PI_USER:-pi}@$(PI_HOST):/usr/share/chatbox/avatars/ avatars/
	@echo "Pulled config → $(PI_CONFIG), avatars → avatars/"

run-pi: build pull-pi-config
	$(BINARY) $(PI_CONFIG)

deploy:
	@if [ -z "$(PI_HOST)" ]; then \
		echo "ERROR: PI_HOST not set. Create a .env file with PI_HOST=<ip>"; \
		exit 1; \
	fi
	PI_HOST=$(PI_HOST) PI_USER=$${PI_USER:-pi} ./deploy/deploy-to-pi.sh

test: build
	$(TEST_BIN) $(CONFIG)

screenshot:
	@if [ -z "$(PI_HOST)" ]; then \
		echo "ERROR: PI_HOST not set. Create a .env file with PI_HOST=<ip>"; \
		exit 1; \
	fi
	@mkdir -p screenshots
	$(eval SHOT := screenshots/$(shell date +%Y-%m-%d_%H-%M-%S).png)
	@ssh $${PI_USER:-pi}@$(PI_HOST) "sudo cat /dev/fb0" > /tmp/fb_capture.raw
	@ffmpeg -loglevel error -f rawvideo -pixel_format rgb565le -video_size 1280x800 \
		-i /tmp/fb_capture.raw -frames:v 1 -update 1 -y $(SHOT)
	@rm /tmp/fb_capture.raw
	@echo "Saved: $(SHOT)"

diff:
	@if [ -n "$(BEFORE)" ] && [ -n "$(AFTER)" ]; then \
		B="$(BEFORE)"; A="$(AFTER)"; \
	else \
		SHOTS=$$(ls -t screenshots/*.png 2>/dev/null | grep -v diff | head -2); \
		if [ $$(echo "$$SHOTS" | wc -l) -lt 2 ]; then \
			echo "ERROR: Need at least 2 screenshots, or pass BEFORE=… AFTER=…"; exit 1; \
		fi; \
		A=$$(echo "$$SHOTS" | sed -n '1p'); \
		B=$$(echo "$$SHOTS" | sed -n '2p'); \
	fi; \
	OUT=screenshots/diff_$$(date +%Y-%m-%d_%H-%M-%S).png; \
	echo "Diffing: $$B → $$A"; \
	magick compare -highlight-color red -lowlight-color 'rgba(255,255,255,0.1)' \
		-compose src "$$B" "$$A" "$$OUT" 2>&1 | head -1; \
	echo "Saved: $$OUT"

avatar:
	@if [ -z "$(IN)" ] || [ -z "$(OUT)" ]; then \
		echo "Usage: make avatar IN=photo.heic OUT=avatars/mom.png"; exit 1; \
	fi
	@mkdir -p $$(dirname "$(OUT)")
	magick "$(IN)" -background none -resize 84x84 -gravity center -extent 84x84 -extent 128x128 PNG32:"$(OUT)"
	@echo "Saved: $(OUT) ($$(magick identify -format '%wx%h %[channels]' "$(OUT)"))"

# Fonts (the sticker kit, pi/src/ui_theme.h). Sources and licences in pi/fonts/.
#   font_chat_*   reading text: Didact Gothic (made for teaching children their
#                 letters), with DejaVu Sans filling the few glyphs it lacks
#   font_title_*  titles, headers, captions: M PLUS Rounded 1c ExtraBold
#                 (subset to Latin + Greek: the full font is 3.6 MB of CJK)
# A character outside FONT_RANGE draws as a box.
# 0x20-0x7F ASCII · 0xA0-0x17F Latin-1 + Latin Extended-A · 0x0370-0x03FF Greek
# 0x2010-0x2027 dashes, quotes, bullet, … · 0x2030 ‰ · 0x2039-0x203A ‹ › · 0x20AC €
FONT_DIR   := pi/fonts
DEJAVU     := pi/lib/lvgl/scripts/built_in_font/DejaVuSans.ttf
FONT_AWESOME := pi/lib/lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff
FONT_RANGE := 0x20-0x7F,0xA0-0xFF,0x0100-0x017F,0x0370-0x03FF,0x2010-0x2027,0x2030,0x2039-0x203A,0x20AC
# What Didact Gothic doesn't have, from DejaVu
FONT_FILL  := 0x132-0x133,0x149,0x17F,0x370-0x373,0x376-0x377,0x37B-0x37D,0x3D0-0x3D6,0x3D8-0x3FF,0x2010-0x2012,0x2015-0x2017,0x201B,0x201F,0x2024-0x2025,0x2027
FONT_CONV  := npx lv_font_conv@1.5.3 --bpp 4 --format lvgl --no-compress --no-prefilter --force-fast-kern-format

fonts:
	@for s in 28 36 48; do \
		$(FONT_CONV) --size $$s --font $(FONT_DIR)/DidactGothic-Regular.ttf -r $(FONT_RANGE) \
			--font $(DEJAVU) -r $(FONT_FILL) -o pi/src/font_chat_$$s.c && \
		$(FONT_CONV) --size $$s --font $(FONT_DIR)/MPLUSRounded1c-ExtraBold-latin-greek.ttf -r $(FONT_RANGE) \
			-o pi/src/font_title_$$s.c || exit 1; \
	done
	@# Big glyphs drawn at size instead of scaled up (scaling blurs them):
	@# the music note (LV_SYMBOL_AUDIO) and the picture (LV_SYMBOL_IMAGE, both
	@# FontAwesome), and the Chat tile's count.
	$(FONT_CONV) --size 120 --font $(FONT_AWESOME) -r 0xF001,0xF03E -o pi/src/font_icon_120.c
	$(FONT_CONV) --size 96 --font $(FONT_DIR)/MPLUSRounded1c-ExtraBold-latin-greek.ttf -r 0x30-0x39,0x2B \
		-o pi/src/font_count_96.c
	@echo "Fonts generated: pi/src/font_{chat,title}_{28,36,48}.c, font_icon_120.c, font_count_96.c"

clean:
	rm -rf $(BUILD_DIR)
