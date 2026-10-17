---
name: yomu-typeset
description: >
  Constraints for yomu's Japanese typesetter.
  Use when editing the sibling typeset repo, src/paint, atom or page-index files, face CSS, FreeType collection faces, or the host typeset tests, or when the user runs /yomu-typeset.
---

# Typesetter

Follow the struct, rebuild, and face-list rules in `AGENTS.md`.

## Cache magics

Atom files are IRAB, `kAtomMagic` `0x42415249` in `../typeset/src/AtomFile.h`. A mismatch rebuilds the file. Character, group, and 縦中横 records store a 囲み罫 id after `startEm`. 0 is none. The page index does not store the id.
yomu's `TypesetBook` is `../typeset/book/TypesetBook.h`. The X4 Classic class is `../yomu-mini/lib/Book/TypesetBook.h`.
A finished page index is TIDA, `TypesetBook::kIndexMagic` `0x41444954`. A partial index is TIDB, `kIndexPartial` `0x42444954`. An older index magic is rebuilt because the column length changed. The record size stays 7 bytes.
`faceIndex` 0 keeps the path-only page-index hash. A positive index mixes `face ^= (uint32_t)faceIndex * 0x9E3779B9u`.

## Bold and size

`kEmphasisBold` is `0x80` on the atom emphasis byte. The low 7 bits are the mark: 1 sesame, 2 傍線, 3 open sesame, 4 dot, 5 open dot, 6 circle, 7 open circle, 8 double circle, 9 open double circle, 10 triangle. Layout copies the bold bit to `kRunBold` (64) and masks the mark with `0x7f`.
Paint bold with `FT_LOAD_NO_BITMAP`, then `FT_Outline_Embolden(&gs->outline, (FT_Pos)px * 4)`, then render, then the 90° bitmap rotate. Leave the advance at the loaded value.
`Atom.startEm` for a character, a group, or 縦中横 is a percent of the body em. Store 0 for 100. Otherwise store 40–200.

## Margin

`LayoutOptions.margin` is the top and bottom only. `TypesetBook::open` sets 16, then shrinks it when `height - 32` is shorter than one em. A full-page picture uses that inset. A column picture does not.

## Face CSS

Rules live in `../typeset/src/FaceCss.cpp`. A selector is an element, a class, or both. The cap is `kMaxFaceRules` (96). Specificity is 1 for an element, 10 for a class, and 11 for both.
Read at most `kMaxCssLinks` (4) linked sheets, each at most `kMaxCssBytes` (65536). Skip a `<style>` block larger than that.
`weight` 0 inherits, 1 is normal, and 2 is bold. `b` and `strong` are bold unless a rule says normal. An inline style wins.
`pct` 0 inherits. Otherwise it is a percent of the parent, or of the body when `root` is set (`rem`). The parser clamps a declared percent to 40–250. The computed percent stored on the atom is 0 at 100, otherwise 40–200.
A four-side `border` or `border-style` starts a 囲み罫 (`box` 2). One side (`border-bottom`, `border-right`, and the style forms) is 傍線, `kEmLine`. Calibre writes underlines that way. `box` 1 is an explicit none.
The contents list prefers the EPUB nav document, then the NCX. The fragment after `#` is kept.

## Collections

In `third_party/freetype/include/freetype/config/ftmodule.h`, register `psnames`, `psaux`, `pshinter`, and `cff` before the TrueType driver. List those sources before `sfnt` in the Makefile.
`FtFont::load(path, faceIndex)` has no default argument. `faceIndex < 0` keeps the first face that contains あ (U+3042) and 一 (U+4E00). One collection face is one row in 文字.

## Host checks

The 文字 screen test is 200×400 at scale 2. The size row is y 32..80. Face rows start at y 80. The second face is y 128..176. A tap at x 40, y 150 selects that face.
The paging test is 200×260. 下 is a tap at x 150, y 208.
`tests/test_typeset.cpp` skips the collection check when the Hiragino file is unreadable. That path is NFD.
