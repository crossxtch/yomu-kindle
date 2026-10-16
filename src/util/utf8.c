#include "utf8.h"

int yomu_utf8_next(const char **text) {
  const unsigned char *s;
  int need;
  int cp;
  int i;
  if (!text || !*text) {
    return -1;
  }
  s = (const unsigned char *)*text;
  if (*s == 0) {
    return -1;
  }
  if (*s < 0x80) {
    *text = (const char *)(s + 1);
    return (int)*s;
  }
  if ((*s & 0xE0) == 0xC0) {
    need = 2;
    cp = *s & 0x1F;
  } else if ((*s & 0xF0) == 0xE0) {
    need = 3;
    cp = *s & 0x0F;
  } else if ((*s & 0xF8) == 0xF0) {
    need = 4;
    cp = *s & 0x07;
  } else {
    *text = (const char *)(s + 1);
    return 0xFFFD;
  }
  for (i = 1; i < need; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      *text = (const char *)(s + 1);
      return 0xFFFD;
    }
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  if ((need == 2 && cp < 0x80) || (need == 3 && cp < 0x800) || (need == 4 && cp < 0x10000) ||
      cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    *text = (const char *)(s + 1);
    return 0xFFFD;
  }
  *text = (const char *)(s + need);
  return cp;
}
