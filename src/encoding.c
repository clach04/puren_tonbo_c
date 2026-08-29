#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "encoding.h"

typedef struct { const char *name; UINT cp; } EncMapping;

static const EncMapping enc_map[] = {
  {"utf8", CP_UTF8}, {"utf-8", CP_UTF8},
  {"cp1252", 1252}, {"latin1", 28591}, {"iso-8859-1", 28591},
  {"cp1250", 1250}, {"cp1251", 1251}, {"cp932", 932}, {"shiftjis", 932},
  {"cp949", 949}, {"euc-kr", 949}, {"gb2312", 936}, {"gbk", 936},
  {"big5", 950}, {"cp1253", 1253}, {"cp1254", 1254}, {"cp1255", 1255},
  {"cp1256", 1256}, {"cp1257", 1257}, {"cp1258", 1258},
  {"iso-8859-2", 28592}, {"iso-8859-15", 28605},
  {NULL, 0}
};

static void str_lower(char *dst, const char *src, int dstsize) {
  int i;
  for (i = 0; i < dstsize - 1 && src[i]; i++)
    dst[i] = (char)tolower((unsigned char)src[i]);
  dst[i] = '\0';
}

UINT encoding_name_to_cp(const char *name) {
  char lower[64];
  int i;
  str_lower(lower, name, sizeof(lower));
  for (i = 0; enc_map[i].name; i++) {
    if (strcmp(lower, enc_map[i].name) == 0)
      return enc_map[i].cp;
  }
  return 0;
}

int encoding_to_wide(const unsigned char *bytes, int len, UINT cp, wchar_t **out_w, int *out_wlen) {
  int wlen;
  DWORD flags = (cp == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0;
  wlen = MultiByteToWideChar(cp, flags, (const char *)bytes, len, NULL, 0);
  if (wlen == 0) return 0;
  *out_w = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
  if (!*out_w) return 0;
  wlen = MultiByteToWideChar(cp, flags, (const char *)bytes, len, *out_w, wlen);
  if (wlen == 0) { free(*out_w); *out_w = NULL; return 0; }
  (*out_w)[wlen] = L'\0';
  *out_wlen = wlen;
  return 1;
}

int wide_to_encoding(const wchar_t *wstr, int wlen, UINT cp, char **out_bytes, int *out_len) {
  int blen;
  blen = WideCharToMultiByte(cp, 0, wstr, wlen, NULL, 0, NULL, NULL);
  if (blen == 0) return 0;
  *out_bytes = (char *)malloc(blen + 1);
  if (!*out_bytes) return 0;
  blen = WideCharToMultiByte(cp, 0, wstr, wlen, *out_bytes, blen, NULL, NULL);
  if (blen == 0) { free(*out_bytes); *out_bytes = NULL; return 0; }
  (*out_bytes)[blen] = '\0';
  *out_len = blen;
  return 1;
}

UINT encoding_detect_bom(const unsigned char *bytes, int len, int *bom_len) {
  if (len >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
    *bom_len = 3;
    return CP_UTF8;
  }
  if (len >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
    *bom_len = 2;
    return 1200;
  }
  if (len >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
    *bom_len = 2;
    return 1201;
  }
  *bom_len = 0;
  return 0;
}
