# Monochrome PS3 — Makefile for PSL1GHT v2
# Also supports local builds without PSL1GHT via HOST_CC for testing.
.SUFFIXES:

# ── Detect environment ───────────────────────────────────────────────
ifdef PSL1GHT
include $(PSL1GHT)/Makefile.base
CC      := ppu-gcc
CFLAGS  := -O2 -Wall -Wextra -Werror=return-type --std=gnu99 \
           -mcpu=cell -mtune=cell -fno-strict-aliasing
LDFLAGS :=
LIBS    := -L$(PS3DEV)/portlibs/ppu/lib \
           -ltiny3d -lgcm_sys -lio -laudio -lnet -lsysutil \
           -lmbedtls -lmbedcrypto -lmbedx509 -lcurl -lm
FSELF   := make_fself

CFILES  := main.c ui.c audio.c flac_decoder.c network.c cJSON.c lv2.c platform_ps3.c
SFILES  := entry.s
else
# Host-native build for testing on PC (no PSL1GHT required)
CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -Werror=return-type --std=gnu99 -g
LDFLAGS :=
LIBS    := -lwinhttp -lwinmm -lgdi32 -luser32 -lws2_32 -lm
FSELF   :=

CFILES  := main.c ui.c audio.c flac_decoder.c network.c cJSON.c platform_pc.c
SFILES  :=
endif

# ── Paths ────────────────────────────────────────────────────────────
BUILD     := build
SOURCE    := src
INCLUDE   := include
PKG_DIR   := pkg_build

TITLE     := Monochrome Music
APPID     := MCRM00001
CONTENTID := UP0001-$(APPID)_00-MONOCHROMEPS3001

INCLUDES  := -I$(INCLUDE)
ifdef PSL1GHT
INCLUDES  += -I$(PS3DEV)/portlibs/ppu/include
endif

# ── Source files ─────────────────────────────────────────────────────
OFILES := $(addprefix $(BUILD)/,$(CFILES:.c=.o) $(SFILES:.s=.o))
ELF    := $(BUILD)/monochrome.elf
EBOOT  := $(PKG_DIR)/USRDIR/EBOOT.BIN

# ── Targets ──────────────────────────────────────────────────────────
.PHONY: all clean pkg test sfo assets

all: $(ELF)

$(ELF): $(OFILES)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@
	@echo "[OK] ELF built: $@"

$(BUILD)/%.o: $(SOURCE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: $(SOURCE)/%.s
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ── PS3 packaging (requires PSL1GHT) ────────────────────────────────
ifdef PSL1GHT
eboot: $(ELF)
	@mkdir -p $(dir $(EBOOT))
	$(FSELF) $< $(EBOOT)
	@echo "[OK] EBOOT.BIN signed."

pkg: eboot sfo assets
	python3 tools/pkg_builder.py $(PKG_DIR) $(APPID).pkg
	@echo "[OK] $(APPID).pkg ready for PS3 installation."
else
pkg: sfo assets
	python3 tools/pkg_builder.py $(PKG_DIR) $(APPID).pkg
	@echo "[OK] $(APPID).pkg built (host test mode — needs real EBOOT.BIN for PS3)."
endif

sfo:
	python3 tools/sfo_builder.py $(PKG_DIR)/PARAM.SFO

assets:
	python3 tools/generate_assets.py

# ── Quick test on host ───────────────────────────────────────────────
test: $(ELF)
	@echo "[TEST] Running on host…"
	./$(ELF)

clean:
	rm -rf $(BUILD) $(EBOOT) $(APPID).pkg
