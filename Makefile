# Host compile for this layer. Later commits add the tests and the Kindle target.

CC ?= cc
CXX ?= c++
SRC_INCLUDES := -Isrc/util -Isrc/ui -Isrc/book -Isrc/net -Isrc/kindle -Isrc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -O2 -D_FILE_OFFSET_BITS=64
CFLAGS += -Ithird_party $(SRC_INCLUDES)
PUFF_CFLAGS ?= -std=gnu11 -O2 -w -D_FILE_OFFSET_BITS=64
TS_CXXFLAGS ?= -std=gnu++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
	-Wno-sign-compare -Wno-missing-field-initializers -Wno-unused-function \
	-Wno-unknown-warning-option -Wno-unused-but-set-variable -D_FILE_OFFSET_BITS=64 \
	-Isrc/typeset/compat -Isrc/typeset -Ithird_party -Ithird_party/freetype/include \
	$(SRC_INCLUDES)
FT_CFLAGS ?= -std=gnu11 -O2 -w -DFT2_BUILD_LIBRARY -D_FILE_OFFSET_BITS=64 \
	-Ithird_party/freetype/include

BUILD := build
HOST := $(BUILD)/host

OBJS := \
	build/host/util/log.o \
	build/host/util/utf8.o \
	build/host/ui/draw.o \
	build/host/ui/font.o \
	build/host/stb_truetype.o

.PHONY: all clean

all: $(OBJS)

$(HOST)/%.o: src/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(HOST)/puff.o: third_party/puff.c
	mkdir -p $(dir $@)
	$(CC) $(PUFF_CFLAGS) -c -o $@ $<

$(BUILD)/filepage.c: src/net/filepage.html
	mkdir -p $(dir $@)
	xxd -i -n filepage_html $< > $@.tmp
	sed 's/^unsigned /const unsigned /' $@.tmp > $@
	rm -f $@.tmp

$(HOST)/filepage.o: $(BUILD)/filepage.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(HOST)/stb_truetype.o: third_party/stb_truetype.c
	mkdir -p $(dir $@)
	$(CC) $(PUFF_CFLAGS) -Ithird_party -c -o $@ $<

$(HOST)/%.o: src/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(TS_CXXFLAGS) -c -o $@ $<

$(HOST)/ft/%.o: third_party/freetype/src/%.c
	mkdir -p $(dir $@)
	$(CC) $(FT_CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)
