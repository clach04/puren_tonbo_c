#ifndef CONFIG_H
#define CONFIG_H

#include "ini.h"
#include "encoding.h"

#define CFG_FILENAME "tonbo.ini"

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
  char ext_name[EXT_EDITORS][64+1];   /* [external] name_1..name_9 */
  char ext_exe[EXT_EDITORS][260];   /* [external] exe_1..exe_9 MAX_PATH */
} AppConfig;

/* Resolve config file path: --config <path>, or search current dir then
   USERPROFILE for CFG_FILENAME.  Returns a static buffer (caller copies). */
const char *config_resolve_path(const char *cli_path);

/* Return the path currently used for saving config. */
const char *config_get_save_path(void);

void config_load(AppConfig *cfg, const char *path);
void config_save(const AppConfig *cfg, const char *path);
int config_equal(const AppConfig *a, const AppConfig *b);

#endif
