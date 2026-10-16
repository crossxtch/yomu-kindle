#ifndef YOMU_UTF8_H
#define YOMU_UTF8_H

/* Next Unicode scalar in a UTF-8 string, or -1 at the end.
 * One ill-formed byte becomes U+FFFD and is consumed.
 */
int yomu_utf8_next(const char **text);

#endif
