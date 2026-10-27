---
name: yomu-typeset
description: >
  Constraints for yomu's Japanese typesetter.
  Use when editing src/ingest, src/typeset, src/paint, atom or page-index files, face CSS, FreeType collection faces, or the host typeset tests, or when the user runs /yomu-typeset.
---

# Typesetter

Follow the struct, rebuild, and face-list rules in `AGENTS.md`.

## Cache magics

Atom files are IRAF, `kAtomMagic` `0x46415249` in `src/ingest/AtomFile.h`. A mismatch rebuilds the file. Character, group, and 縦中横 records store a 囲み罫 id after `startEm`, then a background gray, then the CSS border width in px, then an em width as a percent of the em, a rem flag, a side mask, and a family byte. 0 is none. Gray 0 is no fill. A value of 2 or more is the 8-bit gray, and black is stored as 2. A px width of 0 keeps the em thickness. A positive em width is that percent; the rem flag uses the body em. A side mask of 0 draws every side. Family 0 is the body face and 2 is gothic. A column break stores the 行頭 em, then a before-gap percent and an after-gap percent. The page index does not store these.
KF8 plan files are MRP6, `MobiBook::kPlanMagic`. Each stylesheet flow is scanned on its own and the plan stores those face rules, including an em width, a partial-border mask, a family, and a writing-mode guard. Family 0 inherits, 1 is serif, and 2 is sans. Mode 0 is any chapter, 1 is horizontal, and 2 is vertical. The PalmDOC text length is only the first FDST flow. Unpack through the last flow. An older plan rebuilds the atoms of that `.mobi`, `.azw3`, or `.azw`.
yomu's `TypesetBook` is `src/book/TypesetBook.h`. The X4 Classic class is `../yomu-mini/lib/Book/TypesetBook.h`.
A book path is stored in 1024 bytes, the same size as the shelf path in `src/ui/screen.h`. `ZipArchive` keeps that copy so it can reopen the file. A longer path fails open with `path long`.
`pageCount()` returns the pages indexed so far, clamped at 65535.
A finished page index is TIDA, `TypesetBook::kIndexMagic` `0x41444954`. A partial index is TIDB, `kIndexPartial` `0x42444954`. An older index magic is rebuilt because the column length changed. The record size stays 7 bytes. Reading atoms from one page record must land on the next. A shifted offset can still parse as a character, and the first, middle, and last records can be real boundaries while another page is not, so those breaks are rebuilt. The magic stays, so an aligned index is kept.
`faceIndex` 0 keeps the path-only page-index hash. A positive index mixes `face ^= (uint32_t)faceIndex * 0x9E3779B9u`.

## Bold and size

`kEmphasisBold` is `0x80` on the atom emphasis byte. The low 7 bits are the mark: 1 sesame, 2 傍線, 3 open sesame, 4 dot, 5 open dot, 6 circle, 7 open circle, 8 double circle, 9 open double circle, 10 triangle. Layout copies the bold bit to `kRunBold` (64) and masks the mark with `0x7f`.
Paint bold with `FT_LOAD_NO_BITMAP`, then `FT_Outline_Embolden(&gs->outline, (FT_Pos)px * 4)`, then render, then the 90° bitmap rotate. Leave the advance at the loaded value.
`Atom.startEm` for a character, a group, or 縦中横 is a percent of the body em. Store 0 for 100. Otherwise store 40–200.

## Margin

`LayoutOptions.margin` is the top and bottom only. `TypesetBook::open` sets 16, then shrinks it when `height - 32` is shorter than one em. A full-page picture uses that inset. A column picture does not. A pre-paginated spine item marks its picture as its own page.

## Face CSS

Rules live in `src/ingest/FaceCss.cpp`. A selector is an element, a class, or both. A leading `.vrtl`, `html.vrtl`, or `.vertical-rl` keeps the subject for a vertical chapter. `.hltr`, `html.hltr`, or `.horizontal-tb` keeps it for a horizontal chapter. Any other descendant is dropped. The cap is `kMaxFaceRules` (1536). Specificity is 1 for an element, 10 for a class, and 11 for both.
Read at most `kMaxCssLinks` (8) linked sheets, each at most `kMaxCssBytes` (2MB). `@import` in a kept sheet is another sheet, still inside that cap. A comment hides an import. A `<style>` block is kept up to that size.
`font-family` uses the first generic token. A token that starts with `sans` or `gothic` is family 2. A token that starts with `serif` is family 1. `sans-serif-ja` is sans.
`weight` 0 inherits, 1 is normal, and 2 is bold. `b` and `strong` are bold unless a rule says normal. An inline style wins.
`pct` 0 inherits. Otherwise it is a percent of the parent, or of the body when `root` is set (`rem`). The parser clamps a declared percent to 40–250. The computed percent stored on the atom is 0 at 100, otherwise 40–200.
A four-side `border` or `border-style` is paint `box` 2 and starts a 囲み罫. The side mask stays 0, so paint draws every side. One visible side is 傍線, `kEmLine`, in the traditional slot: a side property such as `border-bottom`, or a four-value `border-style` with a single visible side. Two or three visible sides are a 囲み罫 that draws only those sides. The mask bits are top, right, bottom, left. A width in px is stored as px. A width in em or rem is a percent of the em, and rem uses the body em. 0 leaves the em thickness. A width that lives in its own rule is kept and merged with the style rule of the same selector. Calibre writes underlines as one side. Paint `box` 1 is explicit none and does not start a rule. The atom stores the 囲み罫 id from the cache section, not this paint value.
The contents list prefers the EPUB nav document, then the NCX. The fragment after `#` is kept.

## Collections

In `third_party/freetype/include/freetype/config/ftmodule.h`, register `psnames`, `psaux`, `pshinter`, and `cff` before the TrueType driver. List those sources before `sfnt` in the Makefile.
`FtFont::load(path, faceIndex)` has no default argument. `faceIndex < 0` keeps the first face that contains あ (U+3042) and 一 (U+4E00). One collection face is one row in 文字.
Gothic runs use a second `FtFont` on `TypesetBook`, appended after `faceIndex`. It is not a 文字 row. The device file is `TBGothicMed_213.ttf`. The host file is Hiragino Sans GB. A failed load paints the body face. Layout sets `kRunGothic` from atom family 2. Bold still emboldens that face.

## Host checks

The 文字 screen test is 200×400 at scale 2. The size row is y 32..80. Face rows start at y 80. The second face is y 128..176. A tap at x 40, y 150 selects that face.
The paging test is 200×260. 下 is a tap at x 150, y 208.
`tests/test_typeset.cpp` skips the collection check when the Hiragino file is unreadable. That path is NFD.
