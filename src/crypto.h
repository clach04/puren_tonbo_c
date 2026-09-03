#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>
#include "config.h"

/* External encryption binary (or tool) support.
 *
 * Configured via [crypto.NAME] ini sections (see config.h CryptoTool).
 * Decrypt: child writes plain text to stdout, tonbo reads via pipe.
 * Encrypt: tonbo writes plain text to child's stdin, child writes the
 * encrypted {FILENAME} output file itself.
 * Password is never on the command line: when password_env is set, tonbo
 * spawns the child with that env var containing the GUI-entered password.
 * safe_save / paranoid_save are not supported for external tools
 * (TODO add support for command line tools that support that).
 */

/* Return the CryptoTool owning path's extension, or NULL. */
const CryptoTool *crypto_tool_for_path(const AppConfig *cfg, const char *path);

/* Return non-zero if path's extension matches any configured tool. */
int crypto_is_tool_ext(const AppConfig *cfg, const char *path);

/* Decrypt path via tool. On success returns 0 and sets *out/*outlen
 * (caller frees *out). On failure returns non-zero and fills errmsg. */
int crypto_decrypt_file(const CryptoTool *tool, const char *path,
                        const char *pass, char **out, size_t *outlen,
                        char *errmsg, size_t errsz);

/* Encrypt data to path via tool (child reads plaintext from stdin and
 * writes the encrypted file). Returns 0 on success, else fills errmsg.
 * NOTE: not yet verified end-to-end (child process did not exit in smoke
 * test); GUI wiring is decrypt-only until fixed. */
int crypto_encrypt_file(const CryptoTool *tool, const char *path,
                        const char *data, size_t len, const char *pass,
                        char *errmsg, size_t errsz);

#endif
