#ifndef YOMU_TYPEFACE_H
#define YOMU_TYPEFACE_H

/* Book type size and face. Stored in home/type.txt.
 * The UI face (filenames, menus) is separate. */

#ifdef __cplusplus
extern "C" {
#endif

enum { TYPE_STEP_MIN = -3, TYPE_STEP_MAX = 3, TYPE_FACE_MAX = 24 };

typedef struct TypeFace {
  char path[512]; /* empty path is 標準, the built-in search */
  char label[64];
} TypeFace;

/* Missing file or a NULL home resets to step 0 and 標準. */
void type_load(const char *home);

int type_step(void);
const char *type_step_label(void);
/* Empty string is 標準. */
const char *type_font(void);

/* Pixel em for a page of this size, after the saved step and a fit clamp. */
int type_em(int w, int h);

/* 1 when the saved value changes. */
int type_set_step(int step);
int type_set_font(const char *path);

/* out[0] is always 標準 when cap > 0. Later rows are faces that exist. */
int type_faces(const char *home, TypeFace *out, int cap);

#ifdef __cplusplus
}
#endif

#endif
