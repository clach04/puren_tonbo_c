#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "config.h"
#include "encoding.h"

/* --- config path resolution --- */
static char g_config_path[MAX_PATH]; /* where we last resolved/load/save */

const char *config_resolve_path(const char *cli_path) {
  char buf[MAX_PATH];

  if (cli_path && cli_path[0]) {
    /* explicit --config argument: use as-is (may be absolute or relative) */
    GetFullPathNameA(cli_path, sizeof(g_config_path), g_config_path, NULL);
    return g_config_path;
  }

  /* Search order: current dir first, then USERPROFILE */
  GetCurrentDirectoryA(sizeof(buf), buf);
  snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "\\%s", CFG_FILENAME);
  if (GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES) {
    strncpy(g_config_path, buf, sizeof(g_config_path) - 1);
    g_config_path[sizeof(g_config_path) - 1] = '\0';
    return g_config_path;
  }

  {
    const char *home = getenv("USERPROFILE");
    if (home && home[0]) {
      snprintf(buf, sizeof(buf), "%s\\%s", home, CFG_FILENAME);
      if (GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES) {
        strncpy(g_config_path, buf, sizeof(g_config_path) - 1);
        g_config_path[sizeof(g_config_path) - 1] = '\0';
        return g_config_path;
      }
    }
  }

  /* not found anywhere — default to current directory location */
  GetCurrentDirectoryA(sizeof(buf), buf);
  snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "\\%s", CFG_FILENAME);
  strncpy(g_config_path, buf, sizeof(g_config_path) - 1);
  g_config_path[sizeof(g_config_path) - 1] = '\0';
  return g_config_path;
}

const char *config_get_save_path(void) {
  return g_config_path;
}

static void defaults(AppConfig *cfg) {
  cfg->win_x = 100;
  cfg->win_y = 100;
  cfg->win_w = 800;
  cfg->win_h = 600;
  cfg->tree_w = 200;
  cfg->last_dir[0] = '\0';
  cfg->word_wrap = 0;
  cfg->safe_save = 1;
  cfg->paranoid_save = 0;
  cfg->password_timeout = 0;
  cfg->persist_window = 1;
  cfg->sort_dirs_first = 1;
  cfg->fuzzy_search = 1;
  cfg->encoding_count = 1;
  cfg->encoding_cps[0] = CP_UTF8;
}

void config_load(AppConfig *cfg, const char *path) {
  ini_t *ini;
  const char *v;
  defaults(cfg);
  ini = ini_load(path);
  if (!ini) return;
  v = ini_get(ini, "window", "x"); if (v) cfg->win_x = atoi(v);
  v = ini_get(ini, "window", "y"); if (v) cfg->win_y = atoi(v);
  v = ini_get(ini, "window", "w"); if (v) cfg->win_w = atoi(v);
  v = ini_get(ini, "window", "h"); if (v) cfg->win_h = atoi(v);
  v = ini_get(ini, "window", "tree_w"); if (v) cfg->tree_w = atoi(v);
  v = ini_get(ini, "general", "last_dir");
  if (v) { strncpy(cfg->last_dir, v, sizeof(cfg->last_dir) - 1); cfg->last_dir[sizeof(cfg->last_dir) - 1] = '\0'; }
  v = ini_get(ini, "view", "word_wrap"); if (v) cfg->word_wrap = atoi(v);
  v = ini_get(ini, "view", "fuzzy_search"); if (v) cfg->fuzzy_search = atoi(v);
  v = ini_get(ini, "general", "safe_save"); if (v) cfg->safe_save = atoi(v);
  v = ini_get(ini, "general", "paranoid_save"); if (v) cfg->paranoid_save = atoi(v);
  v = ini_get(ini, "general", "password_timeout"); if (v) cfg->password_timeout = atoi(v);
  v = ini_get(ini, "general", "persist_window"); if (v) cfg->persist_window = atoi(v);
  v = ini_get(ini, "general", "sort_dirs_first"); if (v) cfg->sort_dirs_first = atoi(v);
  {
    int i;
    for (i = 0; i < EXT_EDITORS; i++) {
      char key[16];
      sprintf(key, "name_%d", i + 1);
      v = ini_get(ini, "external", key);
      if (v) { strncpy(cfg->ext_name[i], v, sizeof(cfg->ext_name[i]) - 1); cfg->ext_name[i][sizeof(cfg->ext_name[i]) - 1] = '\0'; }
      sprintf(key, "exe_%d", i + 1);
      v = ini_get(ini, "external", key);
      if (v) { strncpy(cfg->ext_exe[i], v, sizeof(cfg->ext_exe[i]) - 1); cfg->ext_exe[i][sizeof(cfg->ext_exe[i]) - 1] = '\0'; }
    }
  }
  if (!cfg->persist_window) { cfg->win_x = 100; cfg->win_y = 100; cfg->win_w = 800; cfg->win_h = 600; }
  v = ini_get(ini, "general", "encoding_list");
  if (v) {
    char tmp[256];
    char *tok, *ctx;
    int n = 0;
    strncpy(tmp, v, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    tok = strtok_s(tmp, ",", &ctx);
    while (tok && n < MAX_ENCODINGS) {
      UINT cp = encoding_name_to_cp(tok);
      if (cp) cfg->encoding_cps[n++] = cp;
      tok = strtok_s(NULL, ",", &ctx);
    }
    if (n > 0) cfg->encoding_count = n;
  }
  ini_free(ini);
}

void config_save(const AppConfig *cfg, const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "[window]\nx=%d\ny=%d\nw=%d\nh=%d\ntree_w=%d\n", cfg->win_x, cfg->win_y, cfg->win_w, cfg->win_h, cfg->tree_w);
  fprintf(f, "[general]\nlast_dir=%s\nsafe_save=%d\nparanoid_save=%d\npassword_timeout=%d\npersist_window=%d\nsort_dirs_first=%d\n", cfg->last_dir, cfg->safe_save, cfg->paranoid_save, cfg->password_timeout, cfg->persist_window, cfg->sort_dirs_first);
  fprintf(f, "[view]\nword_wrap=%d\nfuzzy_search=%d\n", cfg->word_wrap, cfg->fuzzy_search);
  {
    int i, wrote = 0;
    for (i = 0; i < EXT_EDITORS; i++) {
      if (cfg->ext_name[i][0] && cfg->ext_exe[i][0]) {
        if (!wrote) { fprintf(f, "[external]\n"); wrote = 1; }
        fprintf(f, "name_%d=%s\nexe_%d=%s\n", i + 1, cfg->ext_name[i], i + 1, cfg->ext_exe[i]);
      }
    }
  }
  fclose(f);
}

int config_equal(const AppConfig *a, const AppConfig *b) {
  return a->win_x == b->win_x && a->win_y == b->win_y
    && a->win_w == b->win_w && a->win_h == b->win_h
    && a->tree_w == b->tree_w
    && strcmp(a->last_dir, b->last_dir) == 0
    && a->word_wrap == b->word_wrap
    && a->safe_save == b->safe_save
    && a->paranoid_save == b->paranoid_save
    && a->password_timeout == b->password_timeout
    && a->persist_window == b->persist_window
    && a->sort_dirs_first == b->sort_dirs_first
    && a->fuzzy_search == b->fuzzy_search
    && a->encoding_count == b->encoding_count
    && memcmp(a->ext_name, b->ext_name, sizeof(a->ext_name)) == 0
    && memcmp(a->ext_exe, b->ext_exe, sizeof(a->ext_exe)) == 0;
}
