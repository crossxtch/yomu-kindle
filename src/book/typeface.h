#ifndef YOMU_TYPEFACE_H
#define YOMU_TYPEFACE_H

/* Book type size and face. Stored in home/type.txt.
 * The UI face (filenames, menus) is separate. */

#ifdef __cplusplus
extern "C" {
#endif

enum { TYPE_STEP_MIN = -3, TYPE_STEP_MAX = 3, TYPE_FACE_MAX = 24 };

typedef struct TypeFace {
  char path[512];
  char label[64];
  int index; /* face inside a collection; 0 is the first face */
} TypeFace;

/* Missing file or a NULL home resets to step 0 and 標準. */
void type_load(const char *home);

int type_step(void);
const char *type_step_label(void);
/* Empty string means no face has been saved. */
const char *type_font(void);
/* Face index saved with type_font. 0 when no face is saved. */
int type_face_index(void);

/* Pixel em for a page of this size, after the saved step and a fit clamp. */
int type_em(int w, int h);

/* 1 when the saved value changes. */
int type_set_step(int step);
/* index is the face inside a collection. An empty path clears the saved face. */
int type_set_font(const char *path, int index);

/* Rows are faces that exist. There is no empty "標準" row. */
int type_faces(const char *home, TypeFace *out, int cap);
/* Row of the face the open book uses. 0 when the list is empty or the file is not listed. */
int type_used_row(const TypeFace *faces, int n);

#ifdef __cplusplus
}
#endif

#endif
