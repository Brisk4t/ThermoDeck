# ThermoDeck: static ARMv5TE (i.MX28 / ARM926EJ-S) build with musl.
#
#   make                 build build/thermodeck
#   make host            build a host binary (build-host/thermodeck) for compile checks
#   make fonts           regenerate src/font_data.c (needs Pillow)

TOOLCHAIN ?= $(HOME)/opt/toolchains/armv5-eabi--musl--stable-2025.08-1
CROSS ?= $(TOOLCHAIN)/bin/arm-linux-
CC := $(CROSS)gcc
AR := $(CROSS)ar
STRIP := $(CROSS)strip

BUILD ?= build
ARCHFLAGS ?= -march=armv5te -mtune=arm926ej-s -marm -mfloat-abi=soft

BEARSSL := third_party/bearssl
QRCODE := third_party/qrcodegen

CFLAGS := -Os -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
          -ffunction-sections -fdata-sections -pthread $(ARCHFLAGS) \
          -I$(BEARSSL)/inc -I$(QRCODE) -Isrc -DBUILD_EPOCH=$(shell date +%s)L
LDFLAGS := -static -pthread -Wl,--gc-sections $(ARCHFLAGS)
LIBS := -lm

APP_SRC := $(wildcard src/*.c) $(QRCODE)/qrcodegen.c
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

host:
	$(MAKE) BUILD=build-host CROSS= ARCHFLAGS= TOOLCHAIN=

fonts:
	python3 tools/gen_font.py > src/font_data.c

clean:
	rm -rf build build-host

.PHONY: all host fonts clean
