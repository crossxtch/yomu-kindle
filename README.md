# yomu

A reader for `.epub` and `.txt` books on a jailbroken Kindle. It is a small
standalone program, the same idea as [KOReader](https://github.com/koreader/koreader)'s
Kindle launcher: a KUAL menu entry, the Kindle UI paused, and pages drawn
straight to the e-ink panel. Japanese text is typeset in the app, in vertical
columns, with a mincho or gothic face already on the Kindle.

Filenames and chapter names are UTF-8. yomu draws them
with a Japanese font already on the Kindle: Gothic, then Mincho, then
Code2000, then a face from the Japanese font pack. `YOMU_FONT` names a
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

Put `.epub` and `.txt` files in `/mnt/us/yomu/books` on the USB volume. yomu creates
that folder if it is missing. Subfolders are included. yomu starts in
landscape. On a Paperwhite 11 that is 1648×1236, turned 90° clockwise.
`YOMU_ROTATE=0` uses the upright 1236×1648 panel, and `1` turns the other
way, 270°. Each page is typeset to that panel. Hidden folders are skipped, and
so are `system`, `extensions`, and `lost+found` at the top of `books`. 終了
on the menu exits yomu, and the launcher restores the Kindle UI. The
power button sleeps the Kindle. Idle sleep stays off until 終了. yomu is
still running when the screen wakes. The frontlight stays off for the
session and comes back when yomu exits.

This build was checked on a desktop against a real book. It has not been
run on a Kindle.

## Reading

Tap anywhere on a page to go forward. Swipe down from the top eighth of
the screen to open the menu over the page. Swipe up from the bottom eighth
to go back one page. Any other touch on the page does nothing.

The menu lists 再開, 目次, 本棚, 文字, and 終了. The top line is the current
page and the page count, as in 3 / 120 ページ. Tap that line to type a
page number and jump. 目次 is shown when the book has a chapter table. 再開 returns to the page. 本棚 returns
to the library. 文字 sets the type size and the face. 終了 exits yomu. Tap the page below the menu to close
it. 戻る does the same. The second library row is 終了. 戻る from the
library also exits.

文字 offers 小 and 大 around the fitted size. The steps are 最小, 小さめ,
やや小, 標準, やや大, 大きめ, and 最大. 標準 is the size yomu picks for
the panel. The face list starts with 標準, then the Japanese fonts on
the device, then any `.ttf`, `.ttc`, or `.otf` in `yomu/fonts`. The
choice is kept in `/mnt/us/yomu/type.txt`. Changing it typesets the open
book again and returns to the page. 標準 follows `YOMU_FONT` when that
is set, then the usual mincho and gothic faces.

An illustration in an EPUB is its own page. JPEG, PNG, and GIF are fitted
inside the page. SVG is left out. A picture smaller than 64 pixels on both
sides is shown as its alt text instead. A book keeps at most 512 pictures.

Every screen except the page takes taps only. A swipe there does nothing.
戻る, 上, and 下 appear when that screen needs them. 上 and 下
move a page. The chapter list and 転送 each have a button that leaves
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

The first library row is 転送. While that screen is open, yomu serves
the book folder (`YOMU_ROOT`, normally `/mnt/us/yomu/books`). A browser on the same
Wi-Fi opens the address
shown on the screen. The page lists, uploads, downloads, renames, moves,
and deletes files. Tap 停止 to close the server.

There is no password. Anyone who can reach the Kindle can change files in
that folder until you leave the screen. Names that start with a dot are
hidden, and so are `system`, `lost+found`, and `extensions` at the top of
the folder.

The server listens on port 80, or on 8080 if 80 is taken. `YOMU_PORT`
picks one port and does not try the other. On the Kindle, yomu adds an
iptables rule for that port and removes it when the screen closes. Connect
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
| `YOMU_FONT` | TrueType face for the Japanese UI, filenames, and chapter names. When 文字 is 標準, the book uses this file first. Default is the Kindle Gothic, Mincho, or Code2000 font. |
| `YOMU_HOME` | Log and progress. Default `/mnt/us/yomu`. |
| `YOMU_PORT` | File-transfer port. When unset, yomu tries 80 and then 8080. |
| `YOMU_FULL_EVERY` | Page turns between full refreshes. `1` flashes every page. `0` flashes only the first page and the menus. Default `20`. |
| `YOMU_ROTATE` | `0` upright, `1` landscape at 270°, `2` upside down, `3` landscape at 90° (default). |
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

## Third party

- `third_party/puff.c` and `third_party/puff.h` are Mark Adler's puff (zlib
  contrib), with a `puff_stream` refill callback. yomu uses it to inflate
  EPUB zip members. The notice in those files stays.
- `third_party/font8x8.h` is the public-domain font8x8 basic set.
- `third_party/stb_truetype.h` is Sean Barrett's stb_truetype 1.26, public
  domain. It rasterizes the Japanese font that is already on the device.
