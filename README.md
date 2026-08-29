# Puren Tonbo C

GUI plain text note editor with encryption for Microsoft Windows, built with C99 and Win32 API.

Compatible with Tombo by Tomohisa Hirami is/was a C++ Windows desktop and Windows CE plain text note tool, with optional encryption using blowfish. WARNING encryption format NOT considered secure!
Partially compatible with https://github.com/clach04/puren_tonbo

## Features

  * Tree view sidebar for navigating directories
  * Multi-line text editor with word wrap
  * Open/save plain text (.txt, .md) and encrypted (.chi, .chs) files
  * Unicode text support via configurable encoding list (UTF-8, CP1252, etc.)
  * BOM detection (UTF-8, UTF-16 LE/BE)
  * Password dialog for encrypt/decrypt
  * Password caching with inactivity timer (configurable via `password_timeout`)
  * Tools menu with manual "Forget Password"
  * Right-click context menu on tree items (open, rename, encrypt, decrypt, new folder)
  * In-place rename in tree view (F2 or right-click)
  * Basic text search (find next/prev)
  * Safe save by default (writes to temp file, then renames)
  * Optional paranoid mode (read-back verification)

## Build

Has dependencies on other libraries:

    # TODO skip the cd?
    cd src
    git clone https://github.com/clach04/chi_c.git
    # NOTE only need md5 and blowfish files, not the entire repo
    git clone https://github.com/clach04/openssh-portable.git chi_c/src/openssh-portable

Requires mingw gcc on Windows:

```
cd src
gmake
```

Alternatively zig:

```
# Consider: -Wall -Wextra
zig cc -Wno-attributes -std=c99 -Ichi_c/src/ -Ichi_c/src/openssh_shim -Ichi_c/src/openssh-portable -Ichi_c/src/openssh-portable/openbsd-compat -o tonbo.exe main.c config.c encoding.c ini.c fts_fuzzy_match.c chi_c/src/bf01_file.c chi_c/src/openssh-portable/openbsd-compat/blowfish.c chi_c/src/openssh-portable/openbsd-compat/md5.c -lgdi32 -lcomctl32 -lcomdlg32 -lshell32
```


## Usage

Run `tonbo.exe`. The left pane shows a file tree; double-click a file to open it, or use keyboard and enter/return key.

### Command line

`tonbo.exe [path]` - optional single argument, either a directory or a file:

  * Directory: the tree is rooted at that directory (for this session only, `last_dir` in `tonbo.ini` is not changed).
  * File (.txt, .md, .chi, .chs): the file is opened in the editor and the tree is rooted at its containing directory ("Root" selected, as at normal startup). Encrypted files prompt for a password as usual.
  * A nonexistent path or unsupported file type shows a warning and startup continues normally.

### Keyboard Shortcuts

  * Ctrl+N - New file
  * Ctrl+O - Open file
  * Ctrl+S - Save (or Save As if new)
  * Ctrl+Z - Undo
  * Ctrl+F - Find  - FIXME TODO
  * F3 - Find next  - FIXME TODO
  * Shift+F3 - Find previous  - FIXME TODO
  * Shift+Tab/Tab - Switch between tree and editor (or insert tab in editor)

### Configuration

Settings are stored in `tonbo.ini` (created on exit):

```ini
[window]
x=100
y=100
w=800
h=600
tree_w=200

[general]
last_dir=C:\path\to\last\folder
safe_save=1
paranoid_save=0
password_timeout=300
persist_window=1
sort_dirs_first=0
encoding_list=utf8,cp1252

[word]
word_wrap=0
```

  * `safe_save` - Write to temp file first, then rename. Default: 1 (on)
  * `paranoid_save` - Read back temp file and verify contents before rename. Default: 0 (off)
  * `password_timeout` - Cache password in memory for N seconds after successful encrypt/decrypt. Inactivity resets the timer. 0 = disabled (prompt every time). Default: 0
  * `persist_window` - Save and restore window position/size across sessions. 0 = always open at default size/position. Default: 1
  * `sort_dirs_first` - Show directories at the top of the tree view. Default: 1
  * `encoding_list` - Comma-separated list of character encodings to try when opening files. First encoding used for saving. BOM detection tried before the list. Default: `utf8`

## Third-party code and licenses

See individual source files for licensing.

  * `ini.c` / `ini.h` - [rxi/ini](https://github.com/rxi/ini), MIT license.
    INI file parsing.
  * `fts_fuzzy_match.c` / `fts_fuzzy_match.h` - MIT license; originally
    from Forrest Smith's fuzzy string matching in
    [lib_fts](https://github.com/forrestthewoods/lib_fts) (the vendored
    copy's header credits "Philip Jones, 2022"). Vendored from
    [tajmone/fuzzy-search](https://github.com/tajmone/fuzzy-search/tree/master/fts_fuzzy_match/0.2.0),
    maintained by Tristano Ajmone. Includes a local change:
    an ASCII-only `strcasestr` shim for Win32/MinGW, which lacks the
    POSIX function.

## Known Issues

  * Right-click **Encrypt** / **Decrypt** preserves the original file as `<filename>.<ext>.bak`. This is *not secure* (plaintext remains on disk) but prevents accidental data loss during this early proof-of-concept stage.
