# Builds one .gba per folder PARTDIR/partNN  (PARTDIR/part01 -> build/part01.gba, ...).
# The film is stored as parts/film (too big for one cartridge); `python3 tools/split_parts.py parts/film gen`
# cuts it into ROM-sized parts. The GitHub workflow does that automatically, then runs `make PARTDIR=gen`.
DEVKITARM ?= /opt/devkitpro/devkitARM
CC      := $(DEVKITARM)/bin/arm-none-eabi-gcc
OBJCOPY := $(DEVKITARM)/bin/arm-none-eabi-objcopy
GBAFIX  := $(shell which gbafix 2>/dev/null || echo /opt/devkitpro/tools/bin/gbafix)

PARTDIR ?= parts
PARTS   := $(sort $(notdir $(wildcard $(PARTDIR)/part[0-9]*)))
ASSETS  := menu_bg.bin menu_pal.bin secret_bg.bin secret_pal.bin extra_frames.bin extra_idx.bin extra_palette.bin extra_audio.bin extra_state.bin
NEEDED  := frames1a.bin frames1b.bin frames2.bin frames_idx.bin palette.bin audio_a.bin audio_b.bin audio_state.bin
CFLAGS  := -O2 -mthumb -mthumb-interwork -Wall

MISSING := $(filter-out $(wildcard $(ASSETS)),$(ASSETS))
ifneq ($(MISSING),)
$(error Missing file(s) in the repo root: $(MISSING)  (menu / hidden-clip files, see README.md))
endif

ifeq ($(PARTS),)
$(error No $(PARTDIR)/partNN folders found. Run: python3 tools/split_parts.py parts/film $(PARTDIR))
endif

ROMS := $(addprefix build/,$(addsuffix .gba,$(PARTS)))

all: $(ROMS)
.SECONDEXPANSION:

# .incbin in main.c finds the .bin pieces through -Wa,-I (the part directory); the menu + hidden-clip .bin files sit in the repo root, where the assembler looks by default
build/%.elf: main.c adpcm.h $$(wildcard $(PARTDIR)/$$*/*.bin) $(ASSETS)
	@for f in $(NEEDED); do test -f $(PARTDIR)/$*/$$f || { echo "ERROR: $(PARTDIR)/$*/$$f is missing"; exit 1; }; done
	@mkdir -p build
	$(CC) $(CFLAGS) -Wa,-I$(PARTDIR)/$* -specs=gba.specs main.c -o $@

build/%.gba: build/%.elf
	$(OBJCOPY) -O binary $< $@
	$(GBAFIX) $@ -t"MIND $*" -cMTMP -mXX -r0

clean:
	rm -rf build
.PHONY: all clean
