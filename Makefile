# Builds one .gba per directory in parts/ (parts/part01 -> build/part01.gba, ...).
DEVKITARM ?= /opt/devkitpro/devkitARM
CC      := $(DEVKITARM)/bin/arm-none-eabi-gcc
OBJCOPY := $(DEVKITARM)/bin/arm-none-eabi-objcopy
GBAFIX  := $(shell which gbafix 2>/dev/null || echo /opt/devkitpro/tools/bin/gbafix)

CFLAGS := -O2 -mthumb -mthumb-interwork -Wall
PARTS  := film
ROMS   := $(addprefix build/,$(addsuffix .gba,$(PARTS)))

all: $(ROMS)
.SECONDEXPANSION:

# .incbin in main.c finds the .bin pieces (frames1a/1b/2, audio_a/b, idx, palette, state) through -Wa,-I (the part directory)
build/%.elf: main.c adpcm.h $$(wildcard parts/$$*/*.bin)
	@mkdir -p build
	$(CC) $(CFLAGS) -Wa,-Iparts/$* -specs=gba.specs main.c -o $@

build/%.gba: build/%.elf
	$(OBJCOPY) -O binary $< $@
	$(GBAFIX) $@ -t"MIND $*" -cMTMP -mXX -r0

clean:
	rm -rf build
.PHONY: all clean
