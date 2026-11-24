# ThermoDeck: static ARMv5TE (i.MX28 / ARM926EJ-S) build with musl.
#
#   make                 build build/thermodeck
#   make host            build a host binary (build-host/thermodeck) for compile checks
#   make tests           build the host test programs in build-host/tests/
#   make format          reformat the C sources with clang-format (.clang-format)
#   make format-check    list files that aren't formatted (exit 1 if any)
#   make clean           delete build outputs (incl. generated sources in gen/)
#   make distclean       also delete the downloaded third_party/ inputs
#
# Source layout: every module is a directory with its public headers in include/<name>/
# (included as "<name>/header.h") and its .c files beside that. Each module builds into
# its own static library, build/lib/lib<name>.a (objects in build/obj/).
#   lib/<name>   reusable building blocks that know nothing about ThermoDeck's features
#   app/<name>   the features (Wi-Fi, Spotify, web server, UI); app/main.c wires them up
#
# Nothing third-party or generated is kept in git:
#   third_party/   pinned downloads, fetched by tools/fetch_deps.sh on the first build
#   gen/           trust_anchors.c (CA bundle via BearSSL's brssl), font_data.c (DejaVu via
#                  tools/gen_font.c) and icon_data.c (MDI icons in tools/mdi_icons.txt via
#                  tools/gen_icons.c), built with host tools in build-tools/

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

NO_FETCH_GOALS := clean distclean format format-check

# Fetch third_party/ before anything below globs it (skipped for clean / format targets).
ifeq ($(filter $(NO_FETCH_GOALS),$(MAKECMDGOALS)),)
ifneq ($(shell sh tools/fetch_deps.sh >&2 && echo ok),ok)
$(error could not fetch third_party sources (see tools/fetch_deps.sh))
endif
endif

# ---- modules ----

.DEFAULT_GOAL := all
OBJ := $(BUILD)/obj

LIB_MODULES := core json net gfx input
APP_MODULES := wifi spotify hass web ui
MODULE_DIRS := $(addprefix lib/,$(LIB_MODULES)) $(addprefix app/,$(APP_MODULES))

# generated sources belong to the module that uses them
GEN_SRC_net := $(GEN)/trust_anchors.c
GEN_SRC_gfx := $(GEN)/font_data.c $(GEN)/icon_data.c

INCLUDES := $(foreach d,$(MODULE_DIRS),-I$(d)/include) -I$(BEARSSL)/inc -I$(QRCODE) -I$(STB)

CFLAGS := -Os -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
          -ffunction-sections -fdata-sections -pthread -MMD -MP $(ARCHFLAGS) $(INCLUDES) \
          -DBUILD_EPOCH=$(shell date +%s)L
LDFLAGS := -static -pthread -Wl,--gc-sections $(ARCHFLAGS)
LDLIBS := -lm

# module dir -> its .c files (recursively, e.g. app/ui/screens/) / objects / archive
mod_src = $(shell find $(1) -name '*.c' -not -path '*/include/*') $(GEN_SRC_$(notdir $(1)))
mod_obj = $(patsubst %.c,$(OBJ)/%.o,$(call mod_src,$(1)))
mod_lib = $(BUILD)/lib/lib$(1).a

define module_rule
$(call mod_lib,$(notdir $(1))): $(call mod_obj,$(1))
	@mkdir -p $$(dir $$@)
	@rm -f $$@
	$(AR) rcs $$@ $$^
endef
$(foreach d,$(MODULE_DIRS),$(eval $(call module_rule,$(d))))

# third-party libraries
BSSL_SRC := $(shell find $(BEARSSL)/src -name '*.c')
BSSL_OBJ := $(patsubst %.c,$(OBJ)/%.o,$(BSSL_SRC))
BSSL_CFLAGS := -Os -ffunction-sections -fdata-sections $(ARCHFLAGS) -I$(BEARSSL)/inc -I$(BEARSSL)/src

$(call mod_lib,bearssl): $(BSSL_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $^

$(call mod_lib,qrcodegen): $(OBJ)/$(QRCODE)/qrcodegen.o
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $^

# Link order doesn't matter inside the group (app modules call into each other, e.g. the
# services call ui_wake()).
ALL_LIBS := $(foreach m,$(APP_MODULES) $(LIB_MODULES) qrcodegen bearssl,$(call mod_lib,$(m)))
link_group = -Wl,--start-group $(1) -Wl,--end-group

all: $(BUILD)/thermodeck

$(BUILD)/thermodeck: $(OBJ)/app/main.o $(ALL_LIBS)
	$(CC) $(LDFLAGS) -o $@.unstripped $< $(call link_group,$(ALL_LIBS)) $(LDLIBS)
	$(STRIP) -o $@ $@.unstripped
	@ls -l $@

$(OBJ)/$(BEARSSL)/%.o: $(BEARSSL)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(BSSL_CFLAGS) -c $< -o $@

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)

# ---- host test programs (make tests) ----
# Each one links only the modules it exercises; the rest is faked in the test itself.

TESTS := host_test web_host art_host ui_host hass_host
TEST_LIBS_host_test := net json core bearssl
TEST_LIBS_web_host := web spotify hass gfx net json core bearssl
TEST_LIBS_art_host := spotify gfx net json core bearssl
TEST_LIBS_ui_host := ui hass gfx net json core qrcodegen bearssl
TEST_LIBS_hass_host := hass net json core bearssl

define test_rule
$(BUILD)/tests/$(1): $(OBJ)/tests/$(1).o $(foreach m,$(TEST_LIBS_$(1)),$(call mod_lib,$(m)))
	@mkdir -p $$(dir $$@)
	$(CC) -pthread -o $$@ $$< $$(call link_group,$$(filter %.a,$$^)) $(LDLIBS)
endef
$(foreach t,$(TESTS),$(eval $(call test_rule,$(t))))

host-tests: $(addprefix $(BUILD)/tests/,$(TESTS))

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

$(TOOLS)/gen_icons: tools/gen_icons.c $(STB)/stb_truetype.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -I$(STB) -o $@ $< -lm

$(GEN)/icon_data.c: $(TOOLS)/gen_icons tools/mdi_icons.txt third_party/mdi/materialdesignicons-webfont.ttf
	@mkdir -p $(dir $@)
	$(TOOLS)/gen_icons third_party/mdi tools/mdi_icons.txt > $@.tmp
	mv $@.tmp $@

$(GEN)/font_data.c: $(TOOLS)/gen_font third_party/dejavu/DejaVuSans.ttf third_party/dejavu/DejaVuSans-Bold.ttf
	@mkdir -p $(dir $@)
	$(TOOLS)/gen_font third_party/dejavu > $@.tmp
	mv $@.tmp $@

# ---- host builds ----

HOST_VARS := BUILD=build-host CROSS= ARCHFLAGS= TOOLCHAIN=

host:
	$(MAKE) $(HOST_VARS)

tests:
	$(MAKE) $(HOST_VARS) host-tests

# ---- formatting (clang-format: `pip install clang-format`) ----

CLANG_FORMAT ?= clang-format
FORMAT_SRC := $(shell find lib app tests tools -name '*.[ch]' 2>/dev/null)

format:
	$(CLANG_FORMAT) -i $(FORMAT_SRC)

format-check:
	@bad=0; for f in $(FORMAT_SRC); do \
	  $(CLANG_FORMAT) $$f | cmp -s - $$f || { echo "needs formatting: $$f"; bad=1; }; \
	done; exit $$bad

clean:
	rm -rf build build-host $(TOOLS) $(GEN)

distclean: clean
	rm -rf third_party

.PHONY: all host tests host-tests format format-check clean distclean
