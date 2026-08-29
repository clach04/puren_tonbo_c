# C99 Win32 Note Editor with Encryption

## New GUI Note editor

Plain text notes in directories.
With (considered insecure) encryption support using chi_c, compatible with Tombo.

## Scope

Medium feature set:
  * Tree view sidebar (file system directories, nested) with "Root" node and sorted entries
  * Movable splitter between tree and editor
  * Multi-line text editor pane with status bar
  * Open/save .txt, .md (plain) and .chi, .chs (encrypted) files
  * Unicode text support via configurable encoding list (UTF-8, CP1252, etc.). Tries each encoding in turn
  * BOM detection (UTF-8, UTF-16 LE/BE)
  * Password dialog for encrypt/decrypt (confirm field on encrypt only)
  * Password caching with inactivity timer (configurable timeout, auto-forget, manual forget via Tools menu)
  * Text search (find next/prev, modeless find dialog)
  * Word wrap toggle
  * Unix newline normalization (LF on disk, CRLF in editor)
  * Keyboard shortcuts (Ctrl+N/O/S/F/Z, F3, Shift+F3, Tab, +/-, Enter)
  * INI config file (window size/pos, tree width, last dir, wrap, safe/paranoid save)

## Architecture

```
src/
  main.c          Win32 entry, window proc, message loop, menus, dialogs
  config.c/.h     INI config load/save (rxi/ini adapted for write)
  encoding.c/.h   Character encoding conversion (MultiByteToWideChar/WideCharToMultiByte)
  Makefile         gcc build
```


External dependency (vendored copy):
```
ini.c + ini.h            rxi/ini (MIT license), read-only INI parser
```

## File Details

### `main.c`
Single-file Win32 GUI (~1076 lines). Key components:

  * **WinMain**: Register window classes (PurenTonboClass, PassDialog, FindDialog, Splitter). Clamp window pos/size to screen. Create main window. Message loop with pre-dispatch keyboard handling (global shortcuts intercepted before TranslateMessage).
  * **WndProc**: WM_CREATE (menu, tree, splitter, editor, status bar, load last dir from config), WM_SIZE (layout with DeferWindowPos), WM_NOTIFY (tree double-click, tree Enter/+/− keys), WM_COMMAND (menu actions), WM_GETMINMAXINFO (min 400x300), WM_CLOSE (prompt save, save config), WM_DESTROY.
  * **Menu**: File > New (Ctrl+N), Open (Ctrl+O), Save (Ctrl+S), Save As, Exit; Edit > Undo (Ctrl+Z), Cut (Ctrl+X), Copy (Ctrl+C), Paste (Ctrl+V), Find (Ctrl+F), Find Next (F3), Find Previous (Shift+F3); View > Word Wrap; Tools > Forget Password; Help > About.

  * **Tree view**: `TV_INSERTSTRUCT` with `TVI_SORT` for alphabetical ordering. `FindFirstFile`/`FindNextFile` recursion for subdirectories. When `sort_dirs_first=1` (default), two-pass population: directories inserted first (sorted), then files (sorted). Shows .txt, .md, .chi, .chs files. Directories have lParam=0 (leaf marker), files have lParam pointing to malloc'd full path string. "Root" node at top. Double-click or Enter opens file. +/- keys expand/collapse. Enter toggles expand for directories.
  * **Splitter**: Custom "Splitter" window class between tree and editor. Drag to resize. Cursor changes to `IDC_SIZEWE`. Min tree width 50px. Position persisted in config as `tree_w`.
  * **Editor**: `CreateWindowExW(L"EDIT", ...)` (native Unicode control) with `ES_MULTILINE|ES_WANTRETURN|ES_AUTOVSCROLL|WS_VSCROLL`. Optional `WS_HSCROLL|ES_AUTOHSCROLL` when word wrap off. Font: Consolas 14pt fixed-width. `EM_SETLIMITTEXT(0, 0)` for unlimited text size. Dirty state tracked via `EN_CHANGE` notification; title shows `*` prefix when modified. Tab inserts tab character (`EM_REPLACESEL`), Shift+Tab moves focus to tree. All content set/get via `SetWindowTextW`/`GetWindowTextW`.
  * **Status bar**: `STATUSCLASSNAME` with `SBARS_SIZEGRIP`. Shows current file path or "No file".
  * **File open flow**:
      - Plain (.txt, .md): `fopen` + `fread` into malloc'd buffer, detect BOM, try each encoding via `encoding_to_wide` (UTF-8 strict first, then fallbacks), expand LF to CRLF in wide string, `SetWindowTextW` to edit
      - Encrypted (.chi, .chs): `fopen` + `fread` entire file into memory, `bf01_decrypt_mem` with password, same encoding detection/conversion as plain
      - Open dialog filter: "Text Files (*.txt;*.md)" / "Encrypted (*.chi;*.chs)" / "All Files"
      - After open: set `g_curFile`, update `g_curDir` from path, clear dirty state, update title/status/menu/refresh tree
  * **File save flow**:
      - `GetWindowTextW` from editor, strip CR in wide string (`strip_cr_w`), convert to bytes via `wide_to_encoding` using file's original codepage (or first encoding for new files)
      - Plain (.txt, .md): write directly or via safe_save temp file
      - Encrypted (.chi, .chs): `bf01_encrypt_mem` with password, write cipher or via safe_save temp file
      - Save dialog filter: "Text Files (*.txt)" / "Encrypted (*.chi)" / "All Files"
      - Auto-appends `.txt` if no recognized extension provided
      - After save: clear dirty state, update title/menu, `RefreshTree()` to update filenames
  * **Safe save**: Write to temp file (`file.ext.tmp.YYYYMMDD_HHMMSS` in same directory), delete original, rename temp to original. Preserves original until save confirmed successful. Temp file cleaned up on failure.
  * **Paranoid mode**: Extension of safe save. After writing temp file, reads it back and compares byte-for-byte against source data before delete/rename. Only detects truncation and content changes (not longer files). Controlled by `paranoid_save` config. Debug ifdef `DEBUG_TRUNCATE_SAVE_CORRUPTION_CHECK` writes one fewer byte to test the verify path.
  * **Password dialog**: Custom "PassDialog" window class (not DialogBox). Modal: disables parent window. Two `EDIT` controls with `ES_PASSWORD`. Confirm field enabled only for encrypt, disabled for decrypt. "Show password" checkbox toggles `EM_SETPASSWORDCHAR` (wParam=0 to show, `'*'` to hide) on both fields. Enter triggers OK, Escape triggers Cancel. Returns password via `g_passOk`/buffer. Focus returns to tree after successful password entry.
  * **Password caching**: When `password_timeout > 0`, password is cached in memory after successful encrypt/decrypt. Subsequent operations reuse cached password without prompting. Inactivity timer (1-second `WM_TIMER` tick) clears cache after timeout expires. Every successful encrypt/decrypt resets the timer. Tools > Forget Password clears cache immediately. Cache cleared on app exit (`WM_CLOSE`). Password buffer zeroed with `SecureZeroMemory` on clear.
  * **Find dialog**: Custom "FindDialog" window class. Modeless tool window (`WS_EX_TOOLWINDOW`). Text input + Next/Prev/Close buttons. Uses `EM_FINDTEXT` on the edit control. "Not found" message box on no match. Ctrl+F opens, F3/Shift+F3 for next/prev (from message loop).
  * **Focus policy**: Tree focused on startup. Editor focused after New note. Focus returns to tree after password dialog. Tab/Shift+Tab toggles between tree and editor.
  * **Prompt to save**: `PromptSave()` called before New, Open, tree double-click/Enter. Shows Yes/No/Cancel dialog. Returns IDCANCEL to abort operation.

### `config.c` / `config.h`
Thin wrapper around rxi/ini with write support:

  * **AppConfig struct**: `win_x`, `win_y`, `win_w`, `win_h`, `tree_w`, `last_dir[260]`, `word_wrap`, `safe_save`, `paranoid_save`, `password_timeout`, `persist_window`, `sort_dirs_first`, `encoding_count`, `encoding_cps[MAX_ENCODINGS]`
  * `config_load(cfg, path)` -> fills AppConfig from INI, applies defaults if missing
  * `config_save(cfg, path)` -> writes AppConfig to INI via `fprintf`
  * `config_equal(a, b)` -> returns 1 if two AppConfig structs are identical (field-by-field comparison)
  * **Defaults**: win 800x600 at (100,100), tree_w=200, word_wrap=0, safe_save=1 (on), paranoid_save=0 (off), password_timeout=0 (disabled), persist_window=1 (on), sort_dirs_first=0 (off), encoding_count=1 (UTF-8)
  * **Config path**: hardcoded as `"tonbo.ini"` (same directory as executable)
  * **INI sections**: `[window]` (x, y, w, h, tree_w), `[general]` (last_dir, safe_save, paranoid_save, password_timeout, persist_window, sort_dirs_first, encoding_list), `[view]` (word_wrap)

### `encoding.c` / `encoding.h`
Character encoding conversion module using Win32 APIs:

  * Name-to-codepage lookup table: `"utf8"` -> `CP_UTF8`, `"cp1252"` -> `1252`, etc. (~20 encodings)
  * `encoding_name_to_cp(name)` -> UINT codepage, 0 if unknown
  * `encoding_to_wide(bytes, len, cp, &out_w, &out_wlen)` -> 1=success, 0=invalid. Uses `MultiByteToWideChar` with `MB_ERR_INVALID_CHARS` for UTF-8 strict validation
  * `wide_to_encoding(wstr, wlen, cp, &out_bytes, &out_len)` -> 1=success. Uses `WideCharToMultiByte`
  * `encoding_detect_bom(bytes, len, &bom_len)` -> codepage or 0. Detects UTF-8 BOM (EF BB BF), UTF-16 LE (FF FE), UTF-16 BE (FE FF)


## Key Design Decisions

  * **Single-file GUI** (`main.c`): Keeps things simple. YAGNI applies - no need for separate tree/editor modules until code gets unwieldy.
  * **Win32 API only** (no MFC, no WTL): Direct API calls. Compiles with gcc via mingw.
  * **In-memory crypto**: Uses `bf01_encrypt_mem` / `bf01_decrypt_mem` (not stream functions). Entire file loaded into memory for both plaintext and ciphertext. Requires file to fit in memory (twice). Avoids plaintext on disk.
  * **INI write**: Manual `fprintf` for saving config. rxi/ini is read-only; manual output for the few settings needed.
  * **Tree view**: Manual `FindFirstFile`/`FindNextFile` recursion. No `SHBrowseForFolder` - we own the tree and populate it ourselves for full control. Sorted with `TVI_SORT`. Root node labels the directory root.
  * **File associations**: Tree shows .txt, .md, .chi, .chs files. Open dialog also shows these. Other files hidden.
  * **Encoding**: UTF-8 with BOM handling. Edit control uses `EM_SETLIMITTEXT(0, 0)` for unlimited text. No Unicode conversion layer.
  * **Newline normalization**: Unix LF files expanded to CR+LF on load (via `expand_lf`) for Windows edit control compatibility. CR stripped on save (via `strip_cr`) so files are stored with LF only. Works for both plain and encrypted files.
  * **Safe save**: When `safe_save=1` (default on), writes to timestamped temp file (e.g. `file.txt.tmp.20260704_153012`), deletes original, renames temp. Preserves original until save succeeds. Temp file cleaned up on failure.
  * **Paranoid mode**: When `paranoid_save=1` (default off), reads temp file back and compares byte-for-byte before delete/rename. Protects against silent disk write failures. Only detects truncation/content changes, not longer files.
  * **Window geometry**: Position and size clamped to screen bounds before window creation. Minimum size 400x300 enforced via WM_GETMINMAXINFO. Tree width persisted across sessions.
  * **Word wrap toggle**: Destroys and recreates the entire EDIT control (Win32 EDIT control cannot change scroll style dynamically). Text is saved/restored across the recreation.
  * **Editor font**: Consolas 14pt fixed-width. Created with `CreateFont` and applied via WM_SETFONT.
  * **Keyboard shortcuts**: Global keys (Ctrl+N/O/S/F/Z, F3, Shift+F3, Tab, +/-, Enter) intercepted in message loop pre-dispatch, before TranslateMessage. This avoids accelerator table overhead.
  * **Password confirm**: Confirmation field shown only for encrypt. Disabled (grayed out) for decrypt. Prevents typo on encrypt without burdening decrypt.
  * **Show/hide password**: Checkbox in password dialog toggles `EM_SETPASSWORDCHAR` between 0 (visible) and `'*'` (masked). `InvalidateRect` forces repaint after style change. Default hidden.
  * **Password caching**: Avoids re-prompting during active sessions. `PasswordCache_Set` on successful encrypt/decrypt, `PasswordCache_Get` at `AskPassword` entry, `PasswordCache_Clear` on timeout/manual/exit. Inactivity timer resets on each crypto operation. `SetTimer`/`KillTimer` in WM_CREATE/WM_CLOSE. 1-second WM_TIMER tick checks `GetTickCount()` expiry.
  * **Modal password dialog**: Custom window class, not DialogBox. Disables parent window. Custom message loop until dialog destroyed.
  * **Dirty state**: Tracked via `EN_CHANGE` notification (sets `g_dirty` on first change). Title shows `*` prefix. `EM_GETMODIFY` also tracked. `PromptSave()` called on destructive actions.
  * **Menu state**: Save grayed when no file is loaded (`g_curFile[0] == 0`). SaveAs always enabled. Updated on open/new/save.
  * **RefreshTree after save**: Tree is repopulated after save to reflect any filename changes.
  * **Conditional config save**: Do not save config, if config has not changed. On exit (`WM_CLOSE`), config is only written to disk if it differs from the originally loaded values. Saves a copy of loaded config before applying live window geometry, then compares with `config_equal` before calling `config_save`. Avoids unnecessary disk writes when nothing changed.
  * **Unicode encoding support**: EDIT control created with `CreateWindowExW` (native Unicode). File content converted between file encoding and UTF-16 via `encoding_to_wide`/`wide_to_encoding`. Encoding list configured via `encoding_list` INI setting. On load: BOM detection first, then try each encoding in order with UTF-8 strict validation (`MB_ERR_INVALID_CHARS`). On save: use first encoding in list (or file's original encoding). Tree view stays ANSI (system codepage filenames). Title/status bar use ANSI path strings converted via `MultiByteToWideChar` only at API boundaries.
  * **Command line arguments**: Single optional positional argument, parsed in `WinMain` via `ParseCommandLineArgs()` (`GetCommandLineW()` + `CommandLineToArgvW()` for Unicode paths, converted to the app's internal ANSI paths). A directory becomes the tree root for the session (overrides `last_dir` without writing config); a note file (.txt/.md/.chi/.chs) is opened via the existing `BF01OpenFile` flow after `RefreshTree`, with the tree rooted at its parent directory and "Root" selected. A nonexistent path or unsupported extension shows a warning message box and startup falls back to normal behavior (`last_dir`). Extra arguments are ignored.

## Build & Test

  1. `cd src && make` (requires mingw gcc on Windows)
  2. Run `tonbo.exe`
  3. Test plain text: File > New, type text, File > Save as .txt, close, reopen
  4. Test encrypted: File > Save as .chi, enter password+confirm, close, reopen .chi, enter password
  5. Test .md and .chs: same as above with different extensions
  6. Test tree: Navigate directories, open files from tree (double-click or Enter)
  7. Test splitter: Drag between tree and editor to resize
  8. Test search: Edit > Find (Ctrl+F), search for text, F3 for next
  9. Test word wrap: View > Word Wrap toggle
  10. Test keyboard: Tab/Shift+Tab between tree and editor, +/- in tree, Enter in tree
  11. Test safe save: Save a file, verify temp file is cleaned up
  12. Test prompt to save: Modify text, try to open another file, verify prompt appears
  13. Test password caching: Set `password_timeout=10` in tonbo.ini. Open .chi file, enter password. Save it without re-prompting. Wait >10s, next operation prompts again.
  14. Test Tools > Forget Password: Cache password, then Tools > Forget Password, next operation prompts immediately.
  15. Test UTF-8: Create file with accented characters (e.g. cafe.txt), open, verify display
  16. Test CP1252: Create file with CP1252 encoding, set `encoding_list=utf8,cp1252`, open, verify display
  17. Test BOM: Create UTF-8 file with BOM, verify BOM stripped on load, not re-added on save


## BF01 Encryption Format

  * Algorithm: Blowfish in CBC mode
  * Key derivation: MD5 hash of password
  * Salt: 8 random bytes (via `bf01_rand_bytes`)
  * API: `bf01_encrypt_mem(plaintext, len, password, &out_len, hard_exit)` -> malloc'd ciphertext buffer
  * API: `bf01_decrypt_mem(cipherdata, len, password, &out_len, hard_exit)` -> malloc'd plaintext buffer
  * The `hard_exit` parameter: 0 = return NULL on error (current usage), 1 = call exit()
  * Password: plain C string, null-terminated

## TODO Items

  * Undo still shows file as modified, even though it is not changed
  * Delete Folder support
  * Delete File support
  * working find/search support
  * Icon support (tree has placeholder image indices 0/1 but no actual image list)
  * add a view-only mode, to prevent accidental editing and/or deleting
  * support external editor support, under config
  * support templates/snippets, e.g. today's date, time, timestamp, and static text in config file

## Right-Click Context Menu

Right-click on tree items opens a context menu with type-appropriate actions:

  * **Directories**: "Open Directory" (opens in File Explorer via `ShellExecute("explore",...)`), "New Folder" (creates auto-named directory + in-place label editing)
  * **Text files (.txt, .md)**: "Open" (opens with default file association), "Encrypt" (reads file, encrypts to .chi, deletes original via safe_save)
  * **Encrypted files (.chi, .chs)**: "Open" (opens with default file association), "Decrypt" (reads file, decrypts to .txt, deletes original via safe_save)
  * **Empty tree area**: "New Folder" (creates at current root directory level)

Encrypt/Decrypt from context menu:
  * Operates on the on-disk file, independent of the editor
  * Uses `safe_save` (temp file + rename) when enabled
  * If the file is currently open in the editor and dirty, prompts to save before encrypting/decrypting
  * Clears editor state if the operated-on file was the active editor file

### Implementation notes

  * `NM_RCLICK` notification triggers the popup; `TVM_HITTEST` selects the item under cursor. `GetMessagePos()` returns *screen* coordinates: a client-space copy is used for the hit-test, and the original point is passed directly to `TrackPopupMenu` (converting it again via `ClientToScreen` offsets the menu by the tree's screen origin).
  * Command IDs `IDM_OPEN_DIR`, `IDM_OPEN_ASSOC`, `IDM_NEW_FOLDER`, `IDM_ENCRYPT_FILE`, `IDM_DECRYPT_FILE` in WM_COMMAND
  * `g_rightClickPath` + `g_rightClickItem` globals carry context from NM_RCLICK to WM_COMMAND
  * Tree uses `TVS_EDITLABELS` for in-place label editing (F2); `TVN_BEGINLABELEDITW` blocks root edit; `TVN_ENDLABELEDITW` renames on disk via `MoveFile` and updates `lParam`
  * Directories now store their full path in `lParam` (via `_strdup(childPath)`), matching the existing file lParam pattern
  * `TVN_DELETEITEM` handler frees all lParam allocations (fixes the pre-existing file-path memory leak)
  * Item type is now determined by `is_note_ext()` on lParam path rather than `lParam != 0`
