#pragma once

/* TypesetBook maps displayHeight -> page width and displayWidth -> page height,
 * which is how the Xteink board describes a portrait logical page. yomu's
 * buffer is already the logical page, so ts_board_set stores the page size
 * on those two fields. */

struct BoardConfig {
  int displayWidth;
  int displayHeight;
  static BoardConfig ACTIVE;
};

extern "C" void ts_board_set(int page_w, int page_h);
