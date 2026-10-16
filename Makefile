# Host tools run on this machine. The Kindle binary is a static armhf build
# (zig cc -target arm-linux-musleabihf) so one file runs on Paperwhite 2 and
# newer, including the firmware 5.16.3 glibc break.
#
# src/util     log and UTF-8
# src/ui       event loop, screens, and UI text
# src/book     library scan, progress, type size, typesetter bridge
# src/net      transfer server, its page, and the host HTTP client
# src/kindle   framebuffer, touch, power, and main
# src/typeset  Japanese layout (crossjp) and FreeType raster
#
# The HTTP client is only for the host tests. The Kindle build serves
# files itself and does not link that client.

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
ZIG ?= zig
KINDLE_TARGET ?= arm-linux-musleabihf

BUILD := build
HOST := $(BUILD)/host
KINDLE := $(BUILD)/kindle

CORE_SRCS := \
	src/util/log.c \
	src/util/utf8.c \
	src/ui/draw.c \
	src/ui/font.c \
	src/book/progress.c \
	src/book/books.c \
	src/ui/screen.c \
	src/ui/screen_library.c \
	src/ui/screen_reader.c \
	src/ui/screen_menu.c \
	src/ui/screen_chapters.c \
	src/ui/screen_jump.c \
	src/ui/screen_type.c \
	src/ui/screen_xfer.c \
	src/book/typeface.c \
	src/ui/app.c \
	src/net/fileserver.c

HOST_SRCS := $(CORE_SRCS) src/net/http.c
KINDLE_SRCS := $(CORE_SRCS) \
	src/kindle/kindle_fb.c \
	src/kindle/kindle_input.c \
	src/kindle/kindle_power.c \
	src/kindle/main_kindle.c

TS_CPPS := \
	src/typeset/AtomFile.cpp \
	src/typeset/Epub.cpp \
	src/typeset/FaceCss.cpp \
	src/typeset/FtFont.cpp \
	src/typeset/HalPosix.cpp \
	src/typeset/HtmlIR.cpp \
	src/typeset/HtmlSax.cpp \
	src/typeset/Kinsoku.cpp \
	src/typeset/Layout.cpp \
	src/typeset/PicturePage.cpp \
	src/typeset/TextIr.cpp \
	src/typeset/TypesetBook.cpp \
	src/typeset/TypesetBookBuild.cpp \
	src/typeset/TypesetBookGray.cpp \
	src/typeset/Utf8.cpp \
	src/typeset/Zip.cpp \
	src/book/tsbook.cpp

FT_SRCS := \
	third_party/freetype/src/base/ftsystem.c \
	third_party/freetype/src/base/ftinit.c \
	third_party/freetype/src/base/ftdebug.c \
	third_party/freetype/src/base/ftbase.c \
	third_party/freetype/src/base/ftbitmap.c \
	third_party/freetype/src/base/ftmm.c \
	third_party/freetype/src/sfnt/sfnt.c \
	third_party/freetype/src/truetype/truetype.c \
	third_party/freetype/src/smooth/smooth.c

HOST_OBJS := $(patsubst src/%.c,$(HOST)/%.o,$(HOST_SRCS)) $(HOST)/puff.o $(HOST)/filepage.o $(HOST)/stb_truetype.o \
	$(patsubst src/%.cpp,$(HOST)/%.o,$(TS_CPPS)) \
	$(patsubst third_party/freetype/src/%.c,$(HOST)/ft/%.o,$(FT_SRCS))
KINDLE_OBJS := $(patsubst src/%.c,$(KINDLE)/%.o,$(KINDLE_SRCS)) $(KINDLE)/puff.o $(KINDLE)/filepage.o \
	$(KINDLE)/stb_truetype.o \
	$(patsubst src/%.cpp,$(KINDLE)/%.o,$(TS_CPPS)) \
	$(patsubst third_party/freetype/src/%.c,$(KINDLE)/ft/%.o,$(FT_SRCS))

.PHONY: all test kindle package clean

all: $(HOST)/test_app

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

$(HOST)/test_app: $(HOST_OBJS) $(HOST)/test_app.o $(HOST)/test_fileserver.o $(HOST)/test_typeset.o
	$(CXX) -O2 -o $@ $(HOST_OBJS) $(HOST)/test_app.o $(HOST)/test_fileserver.o $(HOST)/test_typeset.o -lm

$(HOST)/test_app.o: tests/test_app.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(HOST)/test_fileserver.o: tests/test_fileserver.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(HOST)/test_typeset.o: tests/test_typeset.cpp
	mkdir -p $(dir $@)
	$(CXX) $(TS_CXXFLAGS) -c -o $@ $<

test: $(HOST)/test_app
	$(HOST)/test_app

$(KINDLE)/%.o: src/%.c
	mkdir -p $(dir $@)
	$(ZIG) cc -target $(KINDLE_TARGET) $(CFLAGS) -c -o $@ $<

$(KINDLE)/puff.o: third_party/puff.c
	mkdir -p $(dir $@)
	$(ZIG) cc -target $(KINDLE_TARGET) $(PUFF_CFLAGS) -c -o $@ $<

$(KINDLE)/filepage.o: $(BUILD)/filepage.c
	mkdir -p $(dir $@)
	$(ZIG) cc -target $(KINDLE_TARGET) $(CFLAGS) -c -o $@ $<

$(KINDLE)/stb_truetype.o: third_party/stb_truetype.c
	mkdir -p $(dir $@)
	$(ZIG) cc -target $(KINDLE_TARGET) $(PUFF_CFLAGS) -Ithird_party -c -o $@ $<

$(KINDLE)/%.o: src/%.cpp
	mkdir -p $(dir $@)
	$(ZIG) c++ -target $(KINDLE_TARGET) $(TS_CXXFLAGS) -c -o $@ $<

$(KINDLE)/ft/%.o: third_party/freetype/src/%.c
	mkdir -p $(dir $@)
	$(ZIG) cc -target $(KINDLE_TARGET) $(FT_CFLAGS) -c -o $@ $<

dist/kindle/yomu/yomu: $(KINDLE_OBJS) \
		platform/kindle/yomu.sh platform/kindle/INSTALL.txt \
		platform/kindle/extensions/yomu/config.xml platform/kindle/extensions/yomu/menu.json
	mkdir -p dist/kindle/yomu dist/kindle/extensions/yomu
	$(ZIG) c++ -target $(KINDLE_TARGET) -static -s -o $@ $(KINDLE_OBJS) -lm
	cp platform/kindle/yomu.sh dist/kindle/yomu/yomu.sh
	chmod 755 dist/kindle/yomu/yomu.sh $@
	cp platform/kindle/extensions/yomu/config.xml platform/kindle/extensions/yomu/menu.json \
		dist/kindle/extensions/yomu/
	cp platform/kindle/INSTALL.txt dist/kindle/INSTALL.txt

kindle: dist/kindle/yomu/yomu

package: kindle

clean:
	rm -rf $(BUILD)
