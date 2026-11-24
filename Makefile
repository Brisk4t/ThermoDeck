# ThermoDeck: static ARMv5TE (i.MX28 / ARM926EJ-S) build with musl.
#
#   make                 build build/thermodeck
#   make host            build a host binary (build-host/thermodeck) for compile checks
#   make clean           delete build outputs (incl. generated sources in gen/)
#   make distclean       also delete the downloaded third_party/ inputs
#
# Nothing third-party or generated is kept in git:
#   third_party/   pinned downloads, fetched by tools/fetch_deps.sh on the first build
#   gen/           trust_anchors.c (CA bundle via BearSSL's brssl) and font_data.c
#                  (DejaVu via tools/gen_font.c), built with host tools in build-tools/

TOOLCHAIN ?= $(HOME)/opt/toolchains/armv5-eabi--musl--stable-2025.08-1
CROSS ?= $(TOOLCHAIN)/bin/arm-linux-
CC := $(CROSS)gcc
AR := $(CROSS)ar
STRIP := $(CROSS)strip
HOSTCC ?= cc

BUILD ?= build
TOOLS := build-tools
GEN := gen
ARCHFLAGS ?= -march=armv5te -mtune=arm926ej-s -marm -mfloat-abi=soft

BEARSSL := third_party/bearssl
QRCODE := third_party/qrcodegen
STB := third_party/stb

# Fetch third_party/ before anything below globs it (skipped for clean targets).
ifeq ($(filter clean distclean,$(MAKECMDGOALS)),)
ifneq ($(shell sh tools/fetch_deps.sh >&2 && echo ok),ok)
$(error could not fetch third_party sources (see tools/fetch_deps.sh))
endif
endif

CFLAGS := -Os -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
          -ffunction-sections -fdata-sections -pthread $(ARCHFLAGS) \
          -I$(BEARSSL)/inc -I$(QRCODE) -I$(STB) -Isrc -DBUILD_EPOCH=$(shell date +%s)L
LDFLAGS := -static -pthread -Wl,--gc-sections $(ARCHFLAGS)
LIBS := -lm

GEN_SRC := $(GEN)/trust_anchors.c $(GEN)/font_data.c
APP_SRC := $(wildcard src/*.c) $(GEN_SRC) $(QRCODE)/qrcodegen.c
APP_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(APP_SRC))
BSSL_SRC := $(shell find $(BEARSSL)/src -name '*.c')
BSSL_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(BSSL_SRC))
BSSL_CFLAGS := -Os -ffunction-sections -fdata-sections $(ARCHFLAGS) -I$(BEARSSL)/inc -I$(BEARSSL)/src

all: $(BUILD)/thermodeck

$(BUILD)/thermodeck: $(APP_OBJ) $(BUILD)/libbearssl.a
	$(CC) $(LDFLAGS) -o $@.unstripped $(APP_OBJ) $(BUILD)/libbearssl.a $(LIBS)
	$(STRIP) -o $@ $@.unstripped
	@ls -l $@

$(BUILD)/libbearssl.a: $(BSSL_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/$(BEARSSL)/%.o: $(BEARSSL)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(BSSL_CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- generated sources (host tools) ----

BRSSL_SRC := $(BSSL_SRC) $(wildcard $(BEARSSL)/tools/*.c)
BRSSL_OBJ := $(patsubst %.c,$(TOOLS)/%.o,$(BRSSL_SRC))

$(TOOLS)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(HOSTCC) -O2 -I$(BEARSSL)/inc -I$(BEARSSL)/src -c $< -o $@

$(TOOLS)/brssl: $(BRSSL_OBJ)
	@echo "  HOSTLD $@"
	@$(HOSTCC) -o $@ $^

$(TOOLS)/gen_font: tools/gen_font.c $(STB)/stb_truetype.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -I$(STB) -o $@ $< -lm

$(GEN)/trust_anchors.c: $(TOOLS)/brssl third_party/cacert.pem
	@mkdir -p $(dir $@)
	{ echo '/* Generated from third_party/cacert.pem (Mozilla CA bundle) by brssl ta -- do not edit. */'; \
	  echo '#include "bearssl.h"'; \
	  $(TOOLS)/brssl ta -q third_party/cacert.pem; \
	  echo 'const br_x509_trust_anchor *trust_anchors = TAs;'; \
	  echo 'const size_t trust_anchors_num = TAs_NUM;'; } > $@.tmp
	mv $@.tmp $@

$(GEN)/font_data.c: $(TOOLS)/gen_font third_party/dejavu/DejaVuSans.ttf third_party/dejavu/DejaVuSans-Bold.ttf
	@mkdir -p $(dir $@)
	$(TOOLS)/gen_font third_party/dejavu > $@.tmp
	mv $@.tmp $@

host:
	$(MAKE) BUILD=build-host CROSS= ARCHFLAGS= TOOLCHAIN=

clean:
	rm -rf build build-host $(TOOLS) $(GEN)

distclean: clean
	rm -rf third_party

.PHONY: all host clean distclean
