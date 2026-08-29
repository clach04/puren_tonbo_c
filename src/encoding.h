#ifndef ENCODING_H
#define ENCODING_H

#include <windows.h>

#define MAX_ENCODINGS 8

UINT encoding_name_to_cp(const char *name);
int encoding_to_wide(const unsigned char *bytes, int len, UINT cp, wchar_t **out_w, int *out_wlen);
int wide_to_encoding(const wchar_t *wstr, int wlen, UINT cp, char **out_bytes, int *out_len);
UINT encoding_detect_bom(const unsigned char *bytes, int len, int *bom_len);

#endif
