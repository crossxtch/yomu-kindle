# yomu

A reader for `.epub`, `.txt`, and DRM-free `.mobi` and `.azw3` books. On a jailbroken Kindle it is a small
standalone program, the same idea as [KOReader](https://github.com/koreader/koreader)'s
Kindle launcher: a KUAL menu entry, the Kindle UI paused, and pages drawn
straight to the e-ink panel. The same reader also builds as an Android app for a
jailbroken Sony DPT-RP1. Japanese text is typeset in the app, in vertical
columns, with a mincho face already on the device.

Filenames and chapter names are UTF-8. yomu draws them
with a Japanese font already on the Kindle: Gothic, then Mincho, then
a face from the Japanese font pack. `YOMU_FONT` names a
`.ttf`, `.ttc`, or `.otf` when you want a different file. Menus, buttons,
and the transfer page are Japanese too. ASCII stays on the 8×8 face. A
name with no letters is still listed as 「本N」 or 「第N章」.

## Install

You need a jailbreak and the hotfix that exposes `/dev/fb0`. Without the
hotfix the reader logs `framebuffer open failed` and exits. Install KUAL on
firmware older than 5.16.3, or KUALA on 5.16.3 and newer (Amazon removed the
Java kindlet). This is the same pair KOReader uses.

The binary is a static armhf musl executable. One file covers Paperwhite 2
and everything newer, including the firmware 5.16.3 glibc split. It does not
run on the soft-float Kindle Touch, Paperwhite 1, or keyboard Kindles.

Copy two folders to the USB root (the volume that shows up as the Kindle
drive):

```
dist/kindle/yomu/          ->  /mnt/us/yomu/
dist/kindle/extensions/yomu/  ->  /mnt/us/extensions/yomu/
```

Eject, then open KUAL or KUALA and choose yomu.

- **yomu** leaves the Kindle framework running and pauses the UI
  (pillow, awesome, and the stock readers), the way KOReader does.
- **yomu no framework** stops `lab126_gui`. That frees RAM. The
  launcher starts the framework again when yomu exits.

Put `.epub`, `.txt`, `.mobi`, and `.azw3` files in `/mnt/us/yomu/books` on the USB volume. A `.azw` file uses the same container. The file has to be DRM-free and reflowable: an encrypted store download, a fixed-layout book, and a KFX file stay unopened. yomu creates
that folder if it is missing. Subfolders are included. yomu starts in
landscape. On a Paperwhite 11 that is 1648×1236, turned 90° clockwise.
That orientation is fixed. `YOMU_ROTATE` is ignored. Each page is typeset to that panel. Hidden folders are skipped, and
so are `system`, `extensions`, and `lost+found` at the top of `books`. 終了
on the library menu exits yomu, and the launcher restores the Kindle UI. The
power button sleeps the Kindle when the framework is still running, so
use the **yomu** item. Idle sleep stays off until 終了. yomu is
still running when the screen wakes. The frontlight stays off for the
session and comes back when yomu exits.

Host tests run on a desktop. This build has also been used on a Paperwhite 11.

## DPT-RP1

`make android` writes `dist/android/yomu.apk`, an armeabi-v7a app for Android 5.1.
Install it with `adb install -r` on a rooted DPT-RP1. The install notes are
`platform/android/INSTALL.txt`. The activity stays in landscape, leaves the
Android framework running, and copies each update into a bitmap. It then asks
the panel to refresh that rectangle: `View.invalidate(Rect, int)` when the
firmware has that method, otherwise the six-argument `postInvalidateDelayed`
KOReader uses for this device (partial GC16, or a full GC16 when yomu asks
for a flash). A plain `View.invalidate` remains if neither method exists.

Books live in `yomu/books` on external storage, usually `/sdcard/yomu/books`.
The 文字 screen scans `/system/fonts` and lists every face that has kana and kanji, plus font files in `yomu/fonts`.
The transfer server tries port 80, then 8080, then 3000, then 4000, then 5000, then any free port. This app cannot bind port 80. The screen shows the address. The power button sleeps the device, and
yomu repaints when the screen comes back.

## Reading

A tap of at most 150 ms turns the page forward. A longer press on the page
does nothing. Swipe down from the top eighth of the screen to open the menu
over the page. Swipe up from the bottom eighth to go back one page. Any other
touch on the page does nothing. On the Kindle, a touch that starts in the
left 6% of the screen is ignored on every screen.

The menu lists 再開, 目次, 本棚, and 文字. The top line is the current
page and the page count, as in 3 / 120 ページ. Tap that line to type a
page number and jump. 目次 is shown when the book has a chapter table. 再開 returns to the page. 本棚 returns
to the library. 文字 sets the type size and the face. Tap the page below the menu to close
it. 戻る does the same. 戻る from the library also exits.

Swipe down from the top eighth of the library to open the same kind of menu.
It lists 転送 and 終了. 転送 opens file transfer. 終了 exits yomu.
Tap below the menu, or 戻る, to close it and stay on the shelf.

文字 offers 小 and 大 around the fitted size. The steps are 最小, 小さめ,
やや小, 標準, やや大, 大きめ, and 最大. 標準 is the size yomu picks for
the panel. The face list scans the device font directories and keeps every
face that has kana and kanji, then any `.ttf`, `.ttc`, `.otf`, or `.otc`
in `yomu/fonts` or `/mnt/us/fonts`. A collection is one row for each such
face. The row in use has the mark. With no saved face that is `YOMU_FONT`
when set, then the usual mincho faces. The path and that face's index are
kept in `/mnt/us/yomu/type.txt`. Changing the row typesets the open
book again and returns to the page. 上 and 下 move a page when the
list is longer than the screen. A gothic file name and Code2000 stay off
this list. Gothic stays on filenames and menus.
The chapter list puts the bar on the chapter that contains the open page.

A chapter keeps the writing direction named on its tag or in its stylesheet.
When the chapter is silent, that stylesheet wins over a package that says
horizontal. A vertical chapter keeps later untitled chapters vertical.

`b` and `strong` are bold unless a rule says normal. `font-weight` and a
`font-size` in em, rem, percent, or a keyword are read from a simple
selector (`strong`, `.big`, `span.big`) in a linked sheet or a style block.
The size stays a percent of the body em. The page leaves 16 pixels clear
at the top and bottom when the panel is tall enough. The sides stay at
the edge.

An illustration stays in the column when both pixel sides are known and
the longer side is at most half the shorter side of the page. Anything
larger is its own page. JPEG, PNG, and GIF are fitted inside that area.
SVG is left out. A picture smaller than 64 pixels on both sides is shown
as its alt text instead. A book keeps at most 512 pictures. A spine of up
to 2048 files opens.

The page and the library open their menu with a downward swipe from the top
eighth. Every other screen takes taps only. A swipe there does nothing.
戻る, 上, and 下 appear when that screen needs them. 上 and 下
move a page. The chapter list and the transfer screen each have a button that leaves
the screen. Keys still move the highlight.

The power button sleeps the Kindle. The page stays up, with スリープ中 in a
box in the middle, until the Kindle wakes. yomu then flashes the page.
The frontlight stays off across that wake.

Page-turn keys and the five-way pad also move. Down and page-down go
forward. Left and right follow the reading direction. A full refresh runs
every 20 page turns. The first page of a book, and every menu, always flashes.
A page turn stays a partial update on a panel whose width is not a multiple
of 8, such as the Paperwhite 11 at 1236 pixels. The panel driver aligns that
rectangle.

Progress is stored in `/mnt/us/yomu/progress.tsv`. The log is
`/mnt/us/yomu/yomu.log`.

## File transfer

転送 is in the library menu. Swipe down from the top of the shelf and choose it.
Opening it serves
the book folder (`YOMU_ROOT`, normally `/mnt/us/yomu/books`). A browser on the same
Wi-Fi opens the address on the screen: `http://yomu.local/` on port 80, or
`http://yomu.local:PORT/` otherwise. The screen also shows the numeric
address. The page lists, uploads, downloads, renames, moves,
and deletes files. A `.ttf`, `.ttc`, `.otf`, or `.otc` upload is stored in
`yomu/fonts`, and the page lists that folder as `fonts`. The 文字 screen
lists those faces. Tap 戻る to leave the screen. The server keeps running
until yomu exits.

There is no password. Anyone who can reach the Kindle can change files in
that folder until yomu exits. Names that start with a dot are
hidden, and so are `system`, `lost+found`, and `extensions` at the top of
the folder.

The server tries port 80, then 8080, then 3000, then 4000, then 5000. If
every one of those is taken, it uses a free port and the screen shows that
address. `YOMU_PORT`
picks one port and does not try the others. On the Kindle, yomu adds
iptables rules for that TCP port and for UDP 5353, and removes them when
yomu exits. Connect
Wi-Fi before opening 転送, and use **yomu** so the framework
stays up and Wi-Fi stays connected. **yomu no framework** can
drop the network. If the screen says 「Wi-Fiアドレスがありません」, connect and open
転送 again.

## If the picture or the touch is wrong

The framebuffer on some models is larger than the visible panel. yomu
typesets each page to the rectangle it treats as the panel. Set that rectangle
in the shell before launching, or export it from `yomu.sh`:

```
YOMU_VIEW=x,y,width,height
```

Other switches:

| Variable | Meaning |
| --- | --- |
| `YOMU_ROOT` | Folder to scan and to serve. Default `/mnt/us/yomu/books`. |
| `YOMU_FONT` | TrueType face for the Japanese UI, filenames, and chapter names. When no face is saved, the book uses this file first. Default is the Kindle Gothic, then Mincho. |
| `YOMU_HOME` | Log and progress. Default `/mnt/us/yomu`. |
| `YOMU_PORT` | File-transfer port. When unset, yomu tries 80, then 8080, then 3000, then 4000, then 5000, then a free port. |
| `YOMU_FULL_EVERY` | Page turns between full refreshes. `1` flashes every page. `0` flashes only the first page and the menus. Default `20`. |
| `YOMU_FB_INVERT` | `1` or `0`. Overrides the black/white sense of the panel. |
| `YOMU_TOUCH_SWAP` | `1` swaps touch X and Y. Default is automatic. |
| `YOMU_TOUCH_FLIP_X` | `1` mirrors touch horizontally. |
| `YOMU_TOUCH_FLIP_Y` | `1` mirrors touch vertically. |

`YOMU_TOUCH_SWAP` and the flip variables must be exactly `0` or `1`.

## Build

Host tools (macOS or Linux):

```
make
make test
```

`make` builds the host tests. `make test` runs them: library scan, Japanese
text, EPUB layout, the reader, and the file-transfer server.

The Kindle binary needs [zig](https://ziglang.org/download/) on `PATH`
(`zig cc -target arm-linux-musleabihf -static`):

```
make kindle
```

That refreshes `dist/kindle/`. The prebuilt `dist/kindle/yomu/yomu` in
this tree is that static binary, so a Kindle can be loaded without a cross
compiler.

The DPT-RP1 APK needs the Android SDK and NDK r27 (API 22, armeabi-v7a):

```
make android
```

`ANDROID_SDK_ROOT` and `ANDROID_NDK` override the SDK and NDK directories.
The default SDK root on this machine is the Homebrew command-line tools
directory. The recipe signs the APK with a debug key under `build/android/`.

## Source

```
src/ui        screens, gestures, and menu text
src/book      shelf, progress, type.txt, and the open-book session
src/ingest    unpack a book into a contents list, atoms, and pictures
src/typeset   lay atoms out into pages
src/paint     paint one page from glyph runs
src/port      file layer shared by the ingest and the renderer
src/kindle    framebuffer, touch, sleep, and main
src/android   DPT-RP1 bitmap, touch, and JNI
src/net       the transfer server and the host HTTP client
src/util      log and UTF-8
platform/kindle   KUAL launcher, copied into dist/kindle
platform/android  APK manifest, activity, and install notes
tests         host tests run by make test
reference/koreader   notes only; it is not built or linked
```

Opening a book runs three stages. `src/ingest` unpacks an EPUB, a DRM-free
MOBI or AZW3, or a text file into a contents list and atoms. `src/typeset`
lays those atoms out into pages. `src/paint` draws one page with FreeType.
`TypesetBook` in `src/book` runs the three stages. The MIT notice is
`src/ingest/LICENSE`.

## Third party

Portions of this software are copyright © 1996-2002, 2006 The FreeType
Project (www.freetype.org). Licensed under the FreeType License, in
`third_party/freetype/FTL.TXT`. yomu builds FreeType 2.13.3 with the
TrueType and CFF raster, not the GPL copy of the same tree.

- `third_party/puff.c` and `third_party/puff.h` are Mark Adler's puff (zlib
  contrib), with a `puff_stream` refill callback. yomu uses it to inflate
  EPUB zip members. The notice in those files stays.
- `third_party/font8x8.h` is the public-domain font8x8 basic set. ASCII
  menus and the page counter use it.
- `third_party/stb_truetype.h` is Sean Barrett's stb_truetype 1.26, public
  domain. It draws the Japanese font already on the device for filenames,
  chapter names, and menus.
- `third_party/stb_image.h` is Sean Barrett's stb_image 2.30, public
  domain. It decodes JPEG, PNG, and GIF pictures in an EPUB.
