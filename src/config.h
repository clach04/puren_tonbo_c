#ifndef CONFIG_H
#define CONFIG_H

#include "ini.h"
#include "encoding.h"

#define CFG_PATH "tonbo.ini"

#define EXT_EDITORS 9

typedef struct {
  int win_x, win_y, win_w, win_h;
  int tree_w;
  char last_dir[260];
  int word_wrap;
  int safe_save;
  int paranoid_save;
  int password_timeout;
  int persist_window;
  int sort_dirs_first;
  int fuzzy_search;
  int encoding_count;
  UINT encoding_cps[MAX_ENCODINGS];
  char ext_name[EXT_EDITORS][64];   /* [external] name_1..name_9 */
  char ext_exe[EXT_EDITORS][260];   /* [external] exe_1..exe_9 */
} AppConfig;

void config_load(AppConfig *cfg, const char *path);
void config_save(const AppConfig *cfg, const char *path);
int config_equal(const AppConfig *a, const AppConfig *b);

#endif
