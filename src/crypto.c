#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "crypto.h"

/* --- extension lookup --- */

static int ext_matches(const char *exts, const char *dot) {
  const char *p = exts;
  size_t dlen = strlen(dot);
  while (*p) {
    const char *tok;
    size_t tlen;
    while (*p == ' ' || *p == '\t') p++;
    tok = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    tlen = (size_t)(p - tok);
    if (tlen == dlen && _strnicmp(tok, dot, tlen) == 0) return 1;
  }
  return 0;
}

const CryptoTool *crypto_tool_for_path(const AppConfig *cfg, const char *path) {
  const char *dot = strrchr(path, '.');
  int i;
  if (!dot) return NULL;
  for (i = 0; i < cfg->crypto_count; i++) {
    if (ext_matches(cfg->crypto[i].extensions, dot)) return &cfg->crypto[i];
  }
  return NULL;
}

int crypto_is_tool_ext(const AppConfig *cfg, const char *path) {
  return crypto_tool_for_path(cfg, path) != NULL;
}

/* --- command building --- */

/* Substitute {FILENAME} with quoted path. Returns 0 on success. Rejects any
 * other {PLACEHOLDER} (fail fast; unknown placeholders are config errors). */
static int build_command(const char *tmpl, const char *path,
                         char *out, size_t outsz, char *errmsg, size_t errsz) {
  size_t o = 0;
  while (*tmpl) {
    if (*tmpl == '{') {
      const char *close = strchr(tmpl, '}');
      size_t plen = close ? (size_t)(close - tmpl) : 0;
      if (close && plen == 9 && _strnicmp(tmpl, "{FILENAME}", 10) == 0) {
        const char *q = strchr(path, ' ') ? "\"" : "";
        int n = snprintf(out + o, outsz - o, "%s%s%s", q, path, q);
        if (n < 0 || (size_t)n >= outsz - o) goto overflow;
        o += (size_t)n;
        tmpl += 10;
        continue;
      }
      snprintf(errmsg, errsz, "Unknown placeholder in command template: %.*s",
               close ? (int)(close - tmpl + 1) : (int)strlen(tmpl), tmpl);
      return 1;
    }
    if (o + 1 >= outsz) goto overflow;
    out[o++] = *tmpl++;
  }
  out[o] = '\0';
  return 0;
overflow:
  snprintf(errmsg, errsz, "Command line too long (max %d)", (int)outsz);
  return 1;
}

/* --- environment --- */

/* Build an env block: parent environment plus name=pass. Returns malloc'd
 * block (free with free()) or NULL on failure. */
static char *build_env_block(const char *name, const char *pass) {
  LPCH parent = GetEnvironmentStringsA();
  char *block, *w;
  size_t total, passlen;
  LPCH p;
  if (!parent) return NULL;
  passlen = strlen(name) + strlen(pass) + 2;
  total = passlen + 1;
  for (p = parent; *p; ) {
    size_t len = strlen(p) + 1;
    total += len;
    p += len;
  }
  block = (char *)malloc(total + 1);
  if (!block) { FreeEnvironmentStringsA(parent); return NULL; }
  w = block;
  for (p = parent; *p; ) {
    size_t len = strlen(p) + 1;
    memcpy(w, p, len);
    w += len;
    p += len;
  }
  FreeEnvironmentStringsA(parent);
  memcpy(w, name, strlen(name));
  w += strlen(name);
  *w++ = '=';
  memcpy(w, pass, strlen(pass));
  w += strlen(pass);
  *w++ = '\0';
  *w = '\0';
  return block;
}

/* --- process plumbing --- */

/* Thread body: drain a pipe handle (HANDLE* passed via param) until EOF. */
static DWORD WINAPI drain_stdout_thread(LPVOID param) {
  HANDLE h = *(HANDLE *)param;
  char buf[4096];
  DWORD n;
  while (h && ReadFile(h, buf, sizeof(buf), &n, NULL) && n > 0) { /* discard */ }
  return 0;
}

typedef struct {
  HANDLE hProcess;
  HANDLE hStdout; /* read end, may be NULL */
  HANDLE hStderr; /* read end, may be NULL */
  HANDLE hStdin;  /* write end, may be NULL */
} ChildProc;

static void child_close(ChildProc *c) {
  if (c->hProcess) CloseHandle(c->hProcess);
  if (c->hStdout) CloseHandle(c->hStdout);
  if (c->hStderr) CloseHandle(c->hStderr);
  if (c->hStdin) CloseHandle(c->hStdin);
  memset(c, 0, sizeof(*c));
}

/* Spawn cmd with optional stdin/stdout pipes and captured stderr.
 * want_stdin: 1 = caller writes plaintext to c->hStdin.
 * want_stdout: 1 = caller reads plain text from c->hStdout.
 * env_block: optional environment block (passed through, not freed). */
static int spawn(const char *cmd, int want_stdin, int want_stdout,
                 char *env_block, ChildProc *c, char *errmsg, size_t errsz) {
  STARTUPINFO si;
  PROCESS_INFORMATION pi;
  SECURITY_ATTRIBUTES sa;
  HANDLE out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL, in_r = NULL, in_w = NULL;

  memset(c, 0, sizeof(*c));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;

  if (want_stdout && !CreatePipe(&out_r, &out_w, &sa, 0)) {
    snprintf(errmsg, errsz, "CreatePipe (stdout) failed, error %lu", GetLastError());
    return 1;
  }
  if (!CreatePipe(&err_r, &err_w, &sa, 0)) {
    snprintf(errmsg, errsz, "CreatePipe (stderr) failed, error %lu", GetLastError());
    child_close(c);
    if (out_r) CloseHandle(out_r);
    if (out_w) CloseHandle(out_w);
    return 1;
  }
  if (want_stdin && !CreatePipe(&in_r, &in_w, &sa, 0)) {
    snprintf(errmsg, errsz, "CreatePipe (stdin) failed, error %lu", GetLastError());
    if (out_r) CloseHandle(out_r);
    if (out_w) CloseHandle(out_w);
    CloseHandle(err_r); CloseHandle(err_w);
    return 1;
  }

  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE; /* hide console window of console tools */
  si.hStdInput = want_stdin ? in_r : GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = want_stdout ? out_w : GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = err_w;

  ZeroMemory(&pi, sizeof(pi));
  if (!CreateProcessA(NULL, (char *)cmd, NULL, NULL, TRUE,
                      CREATE_NO_WINDOW, env_block, NULL, &si, &pi)) {
    snprintf(errmsg, errsz, "Cannot launch command (error %lu):\n%.200s",
             GetLastError(), cmd);
    if (out_r) CloseHandle(out_r);
    if (out_w) CloseHandle(out_w);
    CloseHandle(err_r); CloseHandle(err_w);
    if (in_r) CloseHandle(in_r);
    if (in_w) CloseHandle(in_w);
    return 1;
  }
  CloseHandle(pi.hThread);

  c->hProcess = pi.hProcess;
  c->hStdout = out_r;
  c->hStderr = err_r;
  c->hStdin = in_w;
  /* Parent must not hold inheritable copies of the child's pipe ends,
   * and the child must not inherit the parent's ends (otherwise the
   * child never sees EOF on its stdin pipe). */
  if (out_r) SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
  if (in_w) SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
  /* parent must close child's ends so EOF is delivered */
  if (out_w) CloseHandle(out_w);
  CloseHandle(err_w);
  if (in_r) CloseHandle(in_r);
  return 0;
}

static int read_all(HANDLE h, char **out, size_t *outlen, size_t cap) {
  char buf[4096];
  DWORD n;
  *out = NULL;
  *outlen = 0;
  for (;;) {
    if (!ReadFile(h, buf, sizeof(buf), &n, NULL) || n == 0) break;
    if (*outlen + n > cap) n = (DWORD)(cap - *outlen); /* truncate */
    {
      char *nb = (char *)realloc(*out, *outlen + n + 1);
      if (!nb) { free(*out); *out = NULL; *outlen = 0; return 1; }
      *out = nb;
      memcpy(*out + *outlen, buf, n);
      *outlen += n;
      (*out)[*outlen] = '\0';
    }
    if (*outlen >= cap) break;
  }
  return 0;
}

static int wait_exit(ChildProc *c, char *errmsg, size_t errsz) {
  DWORD code = (DWORD)-1;
  if (WaitForSingleObject(c->hProcess, 60000) != WAIT_OBJECT_0) {
    snprintf(errmsg, errsz, "Timeout waiting for tool to exit");
    TerminateProcess(c->hProcess, 1);
    return (DWORD)-1;
  }
  if (!GetExitCodeProcess(c->hProcess, &code)) {
    snprintf(errmsg, errsz, "Cannot get exit code, error %lu", GetLastError());
    return (DWORD)-1;
  }
  return (int)code;
}

/* --- public API --- */

int crypto_decrypt_file(const CryptoTool *tool, const char *path,
                        const char *pass, char **out, size_t *outlen,
                        char *errmsg, size_t errsz) {
  char cmd[2048];
  char *env = NULL;
  ChildProc c;
  char *stdout_buf = NULL, *stderr_buf = NULL;
  size_t stdout_len = 0, stderr_len = 0;
  int rc = 1, code;

  *out = NULL;
  *outlen = 0;
  if (!tool->decrypt_cmd[0]) {
    snprintf(errmsg, errsz, "No decrypt_cmd configured for .%s", tool->name);
    return 1;
  }
  if (build_command(tool->decrypt_cmd, path, cmd, sizeof(cmd), errmsg, errsz)) return 1;
  if (tool->password_env[0]) {
    env = build_env_block(tool->password_env, pass);
    if (!env) { snprintf(errmsg, errsz, "Out of memory building environment"); return 1; }
  }
  if (spawn(cmd, 0, 1, env, &c, errmsg, errsz)) { free(env); return 1; }
  free(env);

  read_all(c.hStdout, &stdout_buf, &stdout_len, 64 * 1024 * 1024);
  read_all(c.hStderr, &stderr_buf, &stderr_len, 32768);
  code = wait_exit(&c, errmsg, errsz);
  child_close(&c);

  if (code != 0) {
    snprintf(errmsg, errsz, "Decrypt tool exit code %d%s%.3000s", code,
             stderr_buf && stderr_buf[0] ? ":\n" : "",
             stderr_buf && stderr_buf[0] ? stderr_buf : "");
  } else if (!stdout_buf || stdout_len == 0) {
    snprintf(errmsg, errsz, "Decrypt tool produced no output");
  } else {
    *out = stdout_buf;
    *outlen = stdout_len;
    rc = 0;
    stdout_buf = NULL;
  }
  free(stdout_buf);
  free(stderr_buf);
  return rc;
}

int crypto_encrypt_file(const CryptoTool *tool, const char *path,
                        const char *data, size_t len, const char *pass,
                        char *errmsg, size_t errsz) {
  char cmd[2048];
  char *env = NULL;
  ChildProc c;
  char *stderr_buf = NULL;
  size_t stderr_len = 0;
  size_t written = 0;
  int rc = 1, code;

  if (!tool->encrypt_cmd[0]) {
    snprintf(errmsg, errsz, "No encrypt_cmd configured for .%s", tool->name);
    return 1;
  }
  if (build_command(tool->encrypt_cmd, path, cmd, sizeof(cmd), errmsg, errsz)) return 1;
  if (tool->password_env[0]) {
    env = build_env_block(tool->password_env, pass);
    if (!env) { snprintf(errmsg, errsz, "Out of memory building environment"); return 1; }
  }
  if (spawn(cmd, 1, 1, env, &c, errmsg, errsz)) { free(env); return 1; }
  free(env);

  /* Drain child stdout in a thread while writing stdin, so the child can
   * never block on a pipe nobody reads. */
  {
    HANDLE hDrainThread;
    HANDLE hStdoutToDrain = c.hStdout;
    c.hStdout = NULL;
    hDrainThread = CreateThread(NULL, 0, drain_stdout_thread, &hStdoutToDrain, 0, NULL);

    /* write plaintext to child stdin */
    while (written < len) {
      DWORD n = 0;
      if (!WriteFile(c.hStdin, data + written, (DWORD)(len - written), &n, NULL) || n == 0) {
        snprintf(errmsg, errsz, "Write to encrypt tool stdin failed, error %lu", GetLastError());
        child_close(&c);
        CloseHandle(hStdoutToDrain);
        free(stderr_buf);
        return 1;
      }
      written += n;
    }
    CloseHandle(c.hStdin);
    c.hStdin = NULL;

    WaitForSingleObject(hDrainThread, 30000);
    CloseHandle(hDrainThread);
    CloseHandle(hStdoutToDrain);
  }

  read_all(c.hStderr, &stderr_buf, &stderr_len, 32768);
  code = wait_exit(&c, errmsg, errsz);
  child_close(&c);

  if (code != 0) {
    snprintf(errmsg, errsz, "Encrypt tool exit code %d%s%.3000s", code,
             stderr_buf && stderr_buf[0] ? ":\n" : "",
             stderr_buf && stderr_buf[0] ? stderr_buf : "");
  } else {
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
      snprintf(errmsg, errsz, "Encrypt tool did not create output file:\n%.200s", path);
    } else if (attr & FILE_ATTRIBUTE_DIRECTORY) {
      snprintf(errmsg, errsz, "Encrypt output is not a file:\n%.200s", path);
    } else {
      HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, 0, NULL);
      if (hf == INVALID_HANDLE_VALUE) {
        snprintf(errmsg, errsz, "Cannot verify encrypt output file, error %lu", GetLastError());
      } else {
        LARGE_INTEGER sz;
        if (GetFileSizeEx(hf, &sz) && sz.QuadPart > 0) rc = 0;
        else snprintf(errmsg, errsz, "Encrypt output file is empty:\n%.200s", path);
        CloseHandle(hf);
      }
    }
  }
  free(stderr_buf);
  return rc;
}
