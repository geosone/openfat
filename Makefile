# openfat: host test programs, host tests and the OpenWatcom check.
CC      ?= cc
WATCOM  ?= /opt/watcom
WCC     ?= $(WATCOM)/binl64/wcc
BUILD   := build
CFLAGS  ?= -std=c90 -Wall -Wextra -Werror -pedantic -O2
CPPFLAGS += -Iinclude

HDR      := $(wildcard include/openfat/*.h)
CPSRC    := src/codepage.c src/latin.c src/exfat/upcase.c src/name83.c
FATSRC   := src/fat/fat.c $(CPSRC)
EXFAT    := src/exfat/exfat.c $(CPSRC)
# the UNI_<cp>.TAB files the tests load (mars-unicode-tables)
UNITAB   ?= ../mars-unicode-tables/TAB/unitables
CPPFLAGS += -DUNITAB=\"$(abspath $(UNITAB))\"
SRC      := $(wildcard src/*.c src/*/*.c)
H        := $(BUILD)/host

TESTBINS := $(H)/test_exfat $(H)/test_exfat_write $(H)/test_exchk $(H)/test_exfmt \
	$(H)/test_fatchk $(H)/test_defrag $(H)/test_fat_format $(H)/test_part $(H)/test_fat_write $(H)/test_exfat_big \
	$(H)/ofimg

.PHONY: all test check dos-check clean
all: $(TESTBINS)

# quick tests without images
test: $(H)/test_fat_format $(H)/test_exfat
	$(H)/test_fat_format
	$(H)/test_exfat

# the host tests on images made by mkfs.fat/mkfs.exfat and the Linux drivers
check: all test dos-check
	bash tests/part_host.sh
	bash tests/fat_write.sh
	bash tests/fatchk_host.sh
	bash tests/defrag_host.sh
	bash tests/exchk_host.sh
	bash tests/exfmt_host.sh
	bash tests/exfat_write.sh
	bash tests/exfat_big.sh
	bash tests/ofimg_host.sh

$(H)/test_exfat: tests/exfat_test.c $(EXFAT) $(HDR)
$(H)/test_exfat_write: tests/exfat_write_test.c $(EXFAT) $(HDR)
$(H)/test_exfat_big: tests/exfat_big_test.c $(EXFAT) $(HDR)
$(H)/test_exchk: tests/exchk_test.c src/exfat/exfat_check.c $(EXFAT) $(HDR)
$(H)/test_exfmt: tests/exfmt_test.c src/exfat/exfat_format.c src/exfat/upcase.c $(HDR)
$(H)/test_fatchk: tests/fatchk_test.c src/fat/fat_check.c $(CPSRC) $(HDR)
$(H)/test_defrag: tests/defrag_test.c src/fat/fat_defrag.c $(HDR)
$(H)/test_fat_format: tests/fat_format_test.c src/fat/fat_format.c $(HDR)
$(H)/test_fat_write: tests/fat_write_test.c $(FATSRC) $(HDR)
$(H)/test_part: tests/part_test.c src/part/part.c $(HDR)
# the host tool for images (tools/ofimg.c)
$(H)/ofimg: tools/ofimg.c src/exfat/exfat_format.c $(EXFAT) $(HDR)
$(H)/test_fat_write: CPPFLAGS += -DOFAT_FAT_MAXSECT=4096
$(H)/test_exfat_big: CPPFLAGS += -DOFAT_EXFAT_MAXSECT=4096
# a host program with 64-bit printf (the library is checked as C90 elsewhere)
$(H)/test_exfat_big: CFLAGS := $(subst -std=c90,-std=c99,$(CFLAGS))
$(TESTBINS):
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(filter %.c,$^)

# every source as a 16-bit DOS object (small model, 8086), so the library
# stays usable by the DOS drivers and tools
DOSOBJ := $(patsubst src/%.c,$(BUILD)/dos/%.obj,$(SRC))
dos-check: $(DOSOBJ)
$(BUILD)/dos/%.obj: src/%.c $(HDR)
	mkdir -p $(@D)
	WATCOM=$(WATCOM) $(WCC) -q -bt=dos -ms -0 -w4 -we -zq -os -s -iinclude -i$(WATCOM)/h -fo=$@ $<

clean:
	rm -rf $(BUILD)
