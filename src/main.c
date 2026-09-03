#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <richedit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "config.h"
#include "version.h"
#include "encoding.h"
#include "bf01_file.h"
#include "crypto.h"
#include "fts_fuzzy_match.h"

#define FILTER_DEBOUNCE_MS 250

/* Timer IDs */
enum {
  IDT_PASSWORD = 1,
  IDT_FILTER   = 2
};

/* Child control IDs (never reuse values from other ID ranges) */
enum {
  ID_TREE = 1000,
  ID_EDITOR,
  ID_STATUS,
  ID_SPLITTER,
  ID_SEARCH
};

/* Password dialog control IDs */
enum {
  IDC_PASS_EDIT = 10000,
  IDC_PASS_CONFIRM,
  IDC_PASS_OK,
  IDC_PASS_CANCEL,
  IDC_PASS_EDIT2,
  IDC_PASS_SHOW
};

/* Find dialog control IDs */
enum {
  IDC_FIND_EDIT = 11000,
  IDC_FIND_NEXT,
  IDC_FIND_PREV,
  IDC_FIND_CLOSE
};

/* Menu command IDs (arrive via WM_COMMAND with lParam == 0).
 * Ranges are disjoint by design: controls 1000-2000, password dialog
 * 10000+, find dialog 11000+, menu commands 20000+. The lParam == 0
 * guard in WM_COMMAND handling is kept as defense in depth. */
enum {
  IDM_NEW = 20000,
  IDM_OPEN,
  IDM_SAVE,
  IDM_SAVEAS,
  IDM_RENAMEFILE,
  IDM_EXIT,
  IDM_UNDO,
  IDM_CUT,
  IDM_COPY,
  IDM_PASTE,
  IDM_FIND,
  IDM_FINDNEXT,
  IDM_FINDPREV,
  IDM_WORDWRAP,
  IDM_FUZZYSEARCH,
  IDM_ABOUT,
  IDM_EXPANDALL,
  IDM_COLLAPSEALL,
  IDM_FORGETPASSWORD,
  IDM_OPEN_DIR,
  IDM_OPEN_ASSOC,
  IDM_NEW_FOLDER,
  IDM_ENCRYPT_FILE,
  IDM_DECRYPT_FILE,
  IDM_RENAME,
  IDM_COPY_PATH,
  /* external editors, index 0-8 -> IDM_EXT_1..IDM_EXT_9 */
  IDM_EXT_1,
  IDM_EXT_2,
  IDM_EXT_3,
  IDM_EXT_4,
  IDM_EXT_5,
  IDM_EXT_6,
  IDM_EXT_7,
  IDM_EXT_8,
  IDM_EXT_9
};

#define WM_START_LABEL_EDIT (WM_APP + 1)
#define WM_CLI_OPEN (WM_APP + 2)

static HINSTANCE g_hInst;
static HWND g_hWnd;
static HWND g_hTree, g_hEditor, g_hStatus, g_hSplitter;
static HWND g_hFindDlg;
static int g_passOk;
static int g_passEncrypt;
static int g_treeW = 200;
static int g_splitDrag;
static AppConfig g_cfg;
static char g_curFile[MAX_PATH];
static char g_curDir[MAX_PATH];
static BOOL g_dirty;
static HFONT g_hFont;
static wchar_t g_findText[256];

static char g_cliDir[MAX_PATH];
static char g_cliFile[MAX_PATH];

static char g_cached_pass[256];
static int g_pass_cached;
static DWORD g_pass_expire_tick;

static wchar_t *g_editor_wtext;
static int g_editor_wlen;
static UINT g_file_cp;
static char g_rightClickPath[MAX_PATH];
static HTREEITEM g_rightClickItem;

/* --- fuzzy tree filter state --- */
static HWND g_hSearch;
static HFONT g_hSearchFont;
static char **g_filterItems;   /* cached full paths of all tree items */
static int g_nFilterItems;
static char **g_filterSnap;    /* expansion snapshot taken at filter start */
static int g_nFilterSnap;
static int g_filterActive;

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK PassWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK FindWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK SplitterWndProc(HWND, UINT, WPARAM, LPARAM);
static void PopulateTree(HWND hTree, const char *dir, HTREEITEM hParent);
static void RefreshTree(void);
static void ExpandAllItems(HWND hTree, HTREEITEM hItem, int expand);
static void BatchTreeOp(HWND hTree, HTREEITEM hRoot, int expand);
static void BF01OpenFile(const char *path);
static void SaveCurrentFile(void);
static void SaveFileAs(void);
static int PromptSave(void);
static void UpdateTitle(void);
static void UpdateStatus(void);
static void SetEditorFont(HWND hEd);
static int AskPassword(char *passBuf, int bufsize, int encrypt);
static void SetEditorTextW(const wchar_t *wtext);
static int GetEditorTextW(wchar_t **out_w, int *out_wlen);
static void NewFolderAt(HWND hTree, HTREEITEM hParent, const char *parentPath);
static void EncryptFileToDisk(const char *path);
static void DecryptFileToDisk(const char *path);
static int is_chi_file(const char *path);
static void SearchSnapshot(void);
static HTREEITEM FindTreeItemByPath(HWND hTree, HTREEITEM hParent, const char *path);
static void SelectTreeItemByPath(const char *path);
static void SearchSnapshot(void);static void SearchApply(HWND hWnd);
static void SearchClear(int focusTree);
static void FreeFilterCache(void);
static int SubstrPos(const char *hay, const char *needle);

static void PasswordCache_Set(const char *pass) {
  strncpy(g_cached_pass, pass, sizeof(g_cached_pass) - 1);
  g_cached_pass[sizeof(g_cached_pass) - 1] = '\0';
  g_pass_cached = 1;
  if (g_cfg.password_timeout > 0)
    g_pass_expire_tick = GetTickCount() + (DWORD)g_cfg.password_timeout * 1000;
}

static int PasswordCache_Get(char *passBuf, int bufsize) {
  if (g_pass_cached && g_cfg.password_timeout > 0 && GetTickCount() < g_pass_expire_tick) {
    strncpy(passBuf, g_cached_pass, bufsize - 1);
    passBuf[bufsize - 1] = '\0';
    return 1;
  }
  return 0;
}

static void PasswordCache_Clear(void) {
  SecureZeroMemory(g_cached_pass, sizeof(g_cached_pass));
  g_pass_cached = 0;
}

static void PasswordCache_ResetTimer(void) {
  if (g_pass_cached && g_cfg.password_timeout > 0)
    g_pass_expire_tick = GetTickCount() + (DWORD)g_cfg.password_timeout * 1000;
}

static int is_note_ext(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot) return 0;
  return    !_stricmp(dot, ".txt") ||
            !_stricmp(dot, ".md")  ||
            !_stricmp(dot, ".chi") ||
            !_stricmp(dot, ".chs") ||
            crypto_is_tool_ext(&g_cfg, name)
  ;
}

/* Build a double-NUL terminated file dialog filter: built-in types plus a
 * combined group for any configured external crypto extensions. */
static void BuildFileDialogFilter(char *buf, size_t bufsz) {
  size_t o = 0;
  const char *parts[] = {
    "Text Files (*.txt;*.md)", "*.txt;*.md",
    "Encrypted (*.chi;*.chs)", "*.chi;*.chs",
  };
  size_t i;
  for (i = 0; i < sizeof(parts)/sizeof(parts[0]); i++) {
    size_t len = strlen(parts[i]) + 1;
    if (o + len + 1 >= bufsz) return;
    memcpy(buf + o, parts[i], len);
    o += len;
  }
  if (g_cfg.crypto_count > 0) {
    size_t j;
    char exts[256];
    exts[0] = '\0';
    for (j = 0; j < g_cfg.crypto_count; j++) {
      if (exts[0]) strncat(exts, " ", sizeof(exts) - strlen(exts) - 1);
      strncat(exts, g_cfg.crypto[j].extensions, sizeof(exts) - strlen(exts) - 1);
    }
    if (exts[0]) {
      const char *label = "Encrypted (external)";
      size_t len = strlen(label) + 1 + strlen(exts) + 1;
      if (o + len + 1 < bufsz) {
        memcpy(buf + o, label, strlen(label) + 1);
        o += strlen(label) + 1;
        memcpy(buf + o, exts, strlen(exts) + 1);
        o += strlen(exts) + 1;
      }
    }
  }
  memcpy(buf + o, "All Files (*.*)", 16); o += 16;
  memcpy(buf + o, "*.*", 4); o += 4;
  buf[o] = '\0'; buf[o + 1] = '\0'; /* double NUL terminator */
}

/* Copy an ANSI string to the clipboard as CF_TEXT. Returns 0 on success,
 * -1 if the clipboard could not be opened, -2 on alloc failure, -3 if
 * SetClipboardData failed. */
static int CopyTextToClipboard(HWND hWnd, const char *text) {
  HGLOBAL hMem;
  size_t len = strlen(text) + 1;
  if (!OpenClipboard(hWnd)) return -1;
  hMem = GlobalAlloc(GMEM_MOVEABLE, len);
  if (!hMem) { CloseClipboard(); return -2; }
  memcpy(GlobalLock(hMem), text, len);
  GlobalUnlock(hMem);
  EmptyClipboard();
  if (!SetClipboardData(CF_TEXT, hMem)) {
    GlobalFree(hMem);
    CloseClipboard();
    return -3;
  }
  CloseClipboard();
  return 0;
}

/* Parse command line: supports --config <path>, plus single optional
   positional argument which is either a directory (tree root) or a file
   (open in editor, tree rooted at its parent directory).  Uses wide
   command line for Unicode path support; paths converted to the app's
   internal ANSI representation. */
static void ParseCommandLineArgs(char *cliDir, int cliDirSize, char *cliFile,
                                 int cliFileSize, char *cfgPath, int cfgSize) {
  int argc = 0;
  wchar_t **argvW;

  cliDir[0] = '\0';
  cliFile[0] = '\0';
  cfgPath[0] = '\0';

  argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argvW) return;
  {
    int i;
    for (i = 1; i < argc; i++) {
      char arg[MAX_PATH];
      WideCharToMultiByte(CP_ACP, 0, argvW[i], -1, arg, sizeof(arg), NULL, NULL);

      /* --config / -c switch (accept --config= or --config space) */
      if (_stricmp(arg, "--config") == 0 && i + 1 < argc) {
        i++;
        WideCharToMultiByte(CP_ACP, 0, argvW[i], -1, arg, sizeof(arg), NULL, NULL);
        strncpy(cfgPath, arg, cfgSize - 1);
        cfgPath[cfgSize - 1] = '\0';
      } else if ((arg[0] == '-' || arg[0] == '/') && arg[1] == 'c' && arg[2] == '=' && arg[3]) {
        /* --config=path shorthand */
        strncpy(cfgPath, arg + 3, cfgSize - 1);
        cfgPath[cfgSize - 1] = '\0';
      } else if (strncmp(arg, "-config=", 8) == 0) {
        strncpy(cfgPath, arg + 8, cfgSize - 1);
        cfgPath[cfgSize - 1] = '\0';
      } else {
        /* positional: directory or file */
        DWORD attrs = GetFileAttributes(arg);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
          MessageBox(NULL, arg, "Path not found", MB_OK | MB_ICONWARNING);
        } else if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
          strncpy(cliDir, arg, cliDirSize - 1);
          cliDir[cliDirSize - 1] = '\0';
        } else if (is_note_ext(arg)) {
          char *slash;
          strncpy(cliFile, arg, cliFileSize - 1);
          cliFile[cliFileSize - 1] = '\0';
          slash = strrchr(cliFile, '\\');
          if (slash) {
            int dirlen = (int)(slash - cliFile);
            memcpy(cliDir, cliFile, dirlen);
            cliDir[dirlen] = '\0';
          } else {
            GetCurrentDirectoryA(cliDirSize, cliDir);
          }
        } else {
          MessageBox(NULL, arg, "Unsupported file type", MB_OK | MB_ICONWARNING);
        }
      }
    }
  }
  LocalFree(argvW);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow) {
  WNDCLASSEX wc;
  MSG msg;
  INITCOMMONCONTROLSEX icc;

  (void)hPrev; (void)cmdLine;
  g_hInst = hInst;

  /* Resolve config file location (--config flag, search current dir, then
     USERPROFILE). The resolved path is used for both load and save. */
  {
    char cli_cfg[MAX_PATH] = "";
    const char *resolved_path;
    ParseCommandLineArgs(g_cliDir, sizeof(g_cliDir), g_cliFile, sizeof(g_cliFile),
                         cli_cfg, sizeof(cli_cfg));
    resolved_path = config_resolve_path(cli_cfg);
    config_load(&g_cfg, resolved_path);
  }

  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_TREEVIEW_CLASSES;
  InitCommonControlsEx(&icc);

  ZeroMemory(&wc, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  /* TODO: multi-size .ico (16/32/48/256) so small sizes are not scaled */
  wc.hIcon = LoadIconA(hInst, MAKEINTRESOURCEA(1));
  wc.hIconSm = wc.hIcon;
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = "PurenTonboClass";
  RegisterClassEx(&wc);

  wc.lpfnWndProc = PassWndProc;
  wc.lpszClassName = "PassDialog";
  RegisterClassEx(&wc);

  wc.lpfnWndProc = FindWndProc;
  wc.lpszClassName = "FindDialog";
  RegisterClassEx(&wc);

  wc.style = CS_HREDRAW;
  wc.lpfnWndProc = SplitterWndProc;
  wc.lpszClassName = "Splitter";
  RegisterClassEx(&wc);

  {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (g_cfg.win_w < 100) g_cfg.win_w = 800;
    if (g_cfg.win_h < 100) g_cfg.win_h = 600;
    if (g_cfg.win_w > sw) g_cfg.win_w = sw;
    if (g_cfg.win_h > sh) g_cfg.win_h = sh;
    if (g_cfg.win_x < 0) g_cfg.win_x = 0;
    if (g_cfg.win_y < 0) g_cfg.win_y = 0;
    if (g_cfg.win_x > sw - g_cfg.win_w) g_cfg.win_x = sw - g_cfg.win_w;
    if (g_cfg.win_y > sh - g_cfg.win_h) g_cfg.win_y = sh - g_cfg.win_h;
  }

  g_hWnd = CreateWindowEx(0, "PurenTonboClass", "PurenTonbo",
    WS_OVERLAPPEDWINDOW, g_cfg.win_x, g_cfg.win_y,
    g_cfg.win_w, g_cfg.win_h,
    NULL, NULL, hInst, NULL);

  {
    RECT wrc;
    GetWindowRect(g_hWnd, &wrc);
    g_cfg.win_x = wrc.left; g_cfg.win_y = wrc.top;
    g_cfg.win_w = wrc.right - wrc.left; g_cfg.win_h = wrc.bottom - wrc.top;
  }

  ShowWindow(g_hWnd, nShow);
  UpdateWindow(g_hWnd);

  while (GetMessage(&msg, NULL, 0, 0)) {
    if (g_hFindDlg && IsDialogMessage(g_hFindDlg, &msg)) continue;
    if (msg.message == WM_KEYDOWN && (msg.wParam == VK_OEM_PLUS || msg.wParam == VK_OEM_MINUS)
        && GetFocus() == g_hTree) {
      HTREEITEM hSel = TreeView_GetSelection(g_hTree);
      if (hSel) {
        TreeView_Expand(g_hTree, hSel,
          msg.wParam == VK_OEM_PLUS ? TVE_EXPAND : TVE_COLLAPSE);
      }
      continue;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) {
      HWND hFocus = GetFocus();
      int shift = GetKeyState(VK_SHIFT) & 0x8000;
      if (hFocus == g_hTree && !shift) {
        SetFocus(g_hEditor);
        continue;
      }
      if (hFocus == g_hEditor && shift) {
        SetFocus(g_hTree);
        continue;
      }
      if (hFocus == g_hEditor && !shift) {
        SendMessage(g_hEditor, EM_REPLACESEL, TRUE, (LPARAM)"\t");
        continue;
      }
    }
    if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
      int id = 0;
      switch (msg.wParam) {
      case 'N': id = IDM_NEW; break;
      case 'O': id = IDM_OPEN; break;
      case 'S': id = g_curFile[0] ? IDM_SAVE : IDM_SAVEAS; break;
      case 'F': id = IDM_FIND; break;
      case 'Z': id = IDM_UNDO; break;
      case 'L': SetFocus(g_hSearch); break;   /* focus tree filter box */
      case VK_MULTIPLY: id = IDM_EXPANDALL; break;
      case VK_DIVIDE: id = IDM_COLLAPSEALL; break;
      }
      if (id) { SendMessage(g_hWnd, WM_COMMAND, id, 0); continue; }
    }
    /* Alt combos arrive as WM_SYSKEYDOWN (menu interceptor) */
    if (msg.message == WM_SYSKEYDOWN && msg.wParam == 'D'
        && !(GetKeyState(VK_CONTROL) & 0x8000)) {
      SetFocus(g_hSearch);                     /* Alt+D, like a URL bar */
      continue;
    }
    /* Up/Down in the search box hands navigation to the filtered tree */
    if (msg.message == WM_KEYDOWN
        && (msg.wParam == VK_DOWN || msg.wParam == VK_UP)
        && GetFocus() == g_hSearch) {
      SetFocus(g_hTree);
      PostMessage(g_hTree, msg.message, msg.wParam, msg.lParam);
      continue;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_F4
        && GetFocus() != g_hSearch) {
      SetFocus(g_hSearch);
      continue;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE
        && GetFocus() == g_hSearch) {
      /* clear text first so EN_CHANGE restores if a filter was active */
      SetWindowTextA(g_hSearch, "");
      SearchClear(1);
      continue;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_F2 && GetFocus() == g_hTree) {
      HTREEITEM hSel = TreeView_GetSelection(g_hTree);
      if (hSel && hSel != TreeView_GetRoot(g_hTree))
        SendMessage(g_hTree, TVM_EDITLABEL, 0, (LPARAM)hSel);
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  return (int)msg.wParam;
}

static void SetEditorFont(HWND hEd) {
  if (g_hFont) DeleteObject(g_hFont);
  g_hFont = CreateFont(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
  SendMessage(hEd, WM_SETFONT, (WPARAM)g_hFont, TRUE);
}

static void InitMenu(HWND hWnd) {
  HMENU hMenu = GetMenu(hWnd);
  EnableMenuItem(hMenu, IDM_SAVE, MF_GRAYED);
  EnableMenuItem(hMenu, IDM_SAVEAS, MF_GRAYED);
}

static void UpdateMenuSaveState(HWND hWnd) {
  HMENU hMenu = GetMenu(hWnd);
  EnableMenuItem(hMenu, IDM_SAVE, g_curFile[0] ? MF_ENABLED : MF_GRAYED);
  EnableMenuItem(hMenu, IDM_SAVEAS, MF_ENABLED);
}

static LRESULT CALLBACK SplitterWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case WM_SETCURSOR:
    SetCursor(LoadCursor(NULL, IDC_SIZEWE));
    return TRUE;
  case WM_LBUTTONDOWN:
    SetCapture(hWnd);
    g_splitDrag = 1;
    return 0;
  case WM_LBUTTONUP:
    if (g_splitDrag) { ReleaseCapture(); g_splitDrag = 0; }
    return 0;
  case WM_MOUSEMOVE:
    if (g_splitDrag) {
      POINT pt;
      RECT crc;
      pt.x = (short)LOWORD(lParam);
      pt.y = (short)HIWORD(lParam);
      ClientToScreen(hWnd, &pt);
      ScreenToClient(g_hWnd, &pt);
      if (pt.x < 50) pt.x = 50;
      g_treeW = pt.x;
      GetClientRect(g_hWnd, &crc);
      SendMessage(g_hWnd, WM_SIZE, 0, MAKELPARAM(crc.right, crc.bottom));
    }
    return 0;
  }
  return DefWindowProc(hWnd, msg, wParam, lParam);
}

static HWND CreateEditor(HWND hParent, int wrap) {
  DWORD style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL;
  if (!wrap) style |= WS_HSCROLL | ES_AUTOHSCROLL;
  return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
    style, 0, 0, 100, 100, hParent, (HMENU)ID_EDITOR, g_hInst, NULL);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case WM_CREATE: {
    HMENU hMenu = CreateMenu();
    HMENU hFile = CreatePopupMenu();
    HMENU hEdit = CreatePopupMenu();
    HMENU hView = CreatePopupMenu();
    HMENU hTools = CreatePopupMenu();
    HMENU hHelp = CreatePopupMenu();

    AppendMenu(hFile, MF_STRING, IDM_NEW, "&New\tCtrl+N");
    AppendMenu(hFile, MF_STRING, IDM_OPEN, "&Open...\tCtrl+O");
    AppendMenu(hFile, MF_STRING, IDM_SAVE, "&Save\tCtrl+S");
    AppendMenu(hFile, MF_STRING, IDM_SAVEAS, "Save &As...");
    AppendMenu(hFile, MF_STRING, IDM_RENAMEFILE, "Re&name\tF2");
    AppendMenu(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenu(hFile, MF_STRING, IDM_EXIT, "E&xit");
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hFile, "&File");

    AppendMenu(hEdit, MF_STRING, IDM_UNDO, "&Undo\tCtrl+Z");
    AppendMenu(hEdit, MF_SEPARATOR, 0, NULL);
    AppendMenu(hEdit, MF_STRING, IDM_CUT, "Cu&t\tCtrl+X");
    AppendMenu(hEdit, MF_STRING, IDM_COPY, "&Copy\tCtrl+C");
    AppendMenu(hEdit, MF_STRING, IDM_PASTE, "&Paste\tCtrl+V");
    AppendMenu(hEdit, MF_SEPARATOR, 0, NULL);
    AppendMenu(hEdit, MF_STRING, IDM_FIND, "&Find...\tCtrl+F");
    AppendMenu(hEdit, MF_STRING, IDM_FINDNEXT, "Find &Next\tF3");
    AppendMenu(hEdit, MF_STRING, IDM_FINDPREV, "Find &Previous\tShift+F3");
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hEdit, "&Edit");

    AppendMenu(hView, MF_STRING, IDM_WORDWRAP, "&Word Wrap");
    AppendMenu(hView, MF_STRING, IDM_FUZZYSEARCH, "&Fuzzy Search");
    AppendMenu(hView, MF_SEPARATOR, 0, NULL);
    AppendMenu(hView, MF_STRING, IDM_EXPANDALL, "Expand All\tCtrl+Num *");
    AppendMenu(hView, MF_STRING, IDM_COLLAPSEALL, "Collapse All\tCtrl+Num /");
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hView, "&View");

    AppendMenu(hTools, MF_STRING, IDM_FORGETPASSWORD, "&Forget Password");
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hTools, "&Tools");

    AppendMenu(hHelp, MF_STRING, IDM_ABOUT, "&About");
    AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hHelp, "&Help");
    SetMenu(hWnd, hMenu);
    InitMenu(hWnd);
    CheckMenuItem(GetMenu(hWnd), IDM_WORDWRAP,
      g_cfg.word_wrap ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(GetMenu(hWnd), IDM_FUZZYSEARCH,
      g_cfg.fuzzy_search ? MF_CHECKED : MF_UNCHECKED);

    g_hTree = CreateWindowEx(WS_EX_CLIENTEDGE, WC_TREEVIEW, "",
      WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_EDITLABELS,
      0, 0, 200, 400, hWnd, (HMENU)ID_TREE, g_hInst, NULL);

    g_hSplitter = CreateWindow("Splitter", "",
      WS_CHILD | WS_VISIBLE, 200, 0, 4, 400, hWnd, (HMENU)ID_SPLITTER, g_hInst, NULL);

    g_hEditor = CreateEditor(hWnd, 0);

    g_hSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
      0, 0, 200, 22, hWnd, (HMENU)ID_SEARCH, g_hInst, NULL);
    g_hSearchFont = CreateFont(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    SendMessage(g_hSearch, WM_SETFONT, (WPARAM)g_hSearchFont, TRUE);

    g_hStatus = CreateWindowEx(0, STATUSCLASSNAME, "",
      WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
      0, 0, 0, 0, hWnd, (HMENU)ID_STATUS, g_hInst, NULL);

    SetEditorFont(g_hEditor);
    SendMessage(g_hEditor, EM_SETLIMITTEXT, 0, 0);

    if (g_cliDir[0]) strncpy(g_curDir, g_cliDir, MAX_PATH - 1);
    else if (g_cfg.last_dir[0]) strncpy(g_curDir, g_cfg.last_dir, MAX_PATH - 1);
    else GetCurrentDirectory(MAX_PATH, g_curDir);
    g_treeW = g_cfg.tree_w;
    RefreshTree();
    SetFocus(g_hTree);
    if (g_cliFile[0]) {
      /* Defer opening: the password dialog is modal against the main
       * window, which is not visible yet during WM_CREATE. */
      PostMessage(hWnd, WM_CLI_OPEN, 0, 0);
    }
    if (g_cfg.password_timeout > 0)
      SetTimer(hWnd, IDT_PASSWORD, 1000, NULL);
    return 0;
  }

  case WM_SIZE: {
    RECT rc;
    int w = LOWORD(lParam), h = HIWORD(lParam);
    int statusH = 0;
    HDWP hdwp;

    SendMessage(g_hStatus, WM_SIZE, 0, 0);
    GetWindowRect(g_hStatus, &rc);
    statusH = rc.bottom - rc.top;

    hdwp = BeginDeferWindowPos(4);
    {
      RECT src;
      int searchH;
      GetWindowRect(g_hSearch, &src);
      searchH = src.bottom - src.top;
      DeferWindowPos(hdwp, g_hSearch, NULL, 0, 0, g_treeW, searchH, SWP_NOZORDER);
      DeferWindowPos(hdwp, g_hTree, NULL, 0, searchH, g_treeW,
        h - statusH - searchH, SWP_NOZORDER);
    }
    DeferWindowPos(hdwp, g_hSplitter, NULL, g_treeW, 0, 4, h - statusH, SWP_NOZORDER);
    DeferWindowPos(hdwp, g_hEditor, NULL, g_treeW + 4, 0, w - g_treeW - 4, h - statusH, SWP_NOZORDER);
    DeferWindowPos(hdwp, g_hStatus, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    EndDeferWindowPos(hdwp);
    RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    return 0;
  }

  case WM_COMMAND:
    /* Paranoid, but fast/cheap sanity/protection check to avoid problems with dupe IDs.
     * Only menu/accelerator commands here (lParam == 0). Control
     * notifications (EN_*, etc.) carry the control HWND in lParam and
     * their notification codes would be misread as command IDs if they
     * reached the switch. */
    if (lParam != 0)
      return 0;
    if (LOWORD(wParam) == ID_SEARCH && HIWORD(wParam) == EN_CHANGE) {
      char buf[256];
      GetWindowTextA(g_hSearch, buf, sizeof(buf));
      if (!buf[0]) {
        KillTimer(hWnd, IDT_FILTER);
        SearchClear(0);
      } else {
        if (!g_filterActive) SearchSnapshot();
        SetTimer(hWnd, IDT_FILTER, FILTER_DEBOUNCE_MS, NULL);
      }
      return 0;
    }
    if (LOWORD(wParam) == ID_EDITOR && HIWORD(wParam) == EN_CHANGE) {
      if (!g_dirty) {
        g_dirty = TRUE;
        UpdateMenuSaveState(hWnd);
        UpdateTitle();
      }
      return 0;
    }
    switch (LOWORD(wParam)) {
    case IDM_NEW:
      if (PromptSave() != IDCANCEL) {
        g_curFile[0] = '\0';
        SetWindowTextW(g_hEditor, L"");
        if (g_editor_wtext) { free(g_editor_wtext); g_editor_wtext = NULL; }
        g_editor_wlen = 0;
        g_file_cp = 0;
        g_dirty = FALSE;
        UpdateMenuSaveState(hWnd);
        UpdateTitle();
        UpdateStatus();
        SetFocus(g_hEditor);
      }
      break;
    case IDM_OPEN: {
      OPENFILENAME ofn;
      char file[MAX_PATH] = "";
      char filter[1024];
      ZeroMemory(&ofn, sizeof(ofn));
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hWnd;
      BuildFileDialogFilter(filter, sizeof(filter));
      ofn.lpstrFilter = filter;
      ofn.lpstrFile = file;
      ofn.nMaxFile = MAX_PATH;
      ofn.lpstrInitialDir = g_curDir[0] ? g_curDir : NULL;
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
      if (GetOpenFileName(&ofn) && PromptSave() != IDCANCEL) BF01OpenFile(file);
      break;
    }
    case IDM_SAVE:
      if (g_curFile[0]) SaveCurrentFile();
      else SaveFileAs();
      break;
    case IDM_SAVEAS:
      SaveFileAs();
      break;
    case IDM_EXIT:
      SendMessage(hWnd, WM_CLOSE, 0, 0);
      break;
    case IDM_UNDO:
      SendMessage(g_hEditor, EM_UNDO, 0, 0);
      break;
    case IDM_CUT:
      SendMessage(g_hEditor, WM_CUT, 0, 0);
      break;
    case IDM_COPY:
      SendMessage(g_hEditor, WM_COPY, 0, 0);
      break;
    case IDM_PASTE:
      SendMessage(g_hEditor, WM_PASTE, 0, 0);
      break;
    case IDM_FIND:
      if (!g_hFindDlg) {
        g_hFindDlg = CreateWindowEx(WS_EX_TOOLWINDOW, "FindDialog", "Find",
          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
          CW_USEDEFAULT, CW_USEDEFAULT, 300, 100,
          hWnd, NULL, g_hInst, NULL);
      }
      ShowWindow(g_hFindDlg, SW_SHOW);
      SetFocus(g_hFindDlg);
      break;
    case IDM_FINDNEXT: {
      FINDTEXTW ft;
      LONG selEnd;
      LONG pos;
      SendMessage(g_hEditor, EM_GETSEL, 0, (WPARAM)&selEnd);
      ft.chrg.cpMin = selEnd;
      ft.chrg.cpMax = -1;
      ft.lpstrText = g_findText;
      if (g_findText[0] && (pos = (LONG)SendMessageW(g_hEditor, EM_FINDTEXTW, 0, (LPARAM)&ft)) >= 0)
        SendMessage(g_hEditor, EM_SETSEL, pos, pos + (int)wcslen(g_findText));
      break;
    }
    case IDM_FINDPREV: {
      FINDTEXTW ft;
      LONG selStart;
      LONG pos;
      SendMessage(g_hEditor, EM_GETSEL, (WPARAM)&selStart, 0);
      ft.chrg.cpMin = selStart;
      ft.chrg.cpMax = 0;
      ft.lpstrText = g_findText;
      if (g_findText[0] && (pos = (LONG)SendMessageW(g_hEditor, EM_FINDTEXTW, FR_DOWN, (LPARAM)&ft)) >= 0)
        SendMessage(g_hEditor, EM_SETSEL, pos, pos + (int)wcslen(g_findText));
      break;
    }
    case IDM_EXPANDALL:
      BatchTreeOp(g_hTree, TreeView_GetRoot(g_hTree), 1);
      break;
    case IDM_COLLAPSEALL: {
      HTREEITEM hRoot = TreeView_GetRoot(g_hTree);
      if (!hRoot) break;
      {
        HTREEITEM child;
        /* anchor: keep the same first-visible row after the batch */
        HTREEITEM hFirst = TreeView_GetFirstVisible(g_hTree);
        SendMessage(g_hTree, WM_SETREDRAW, FALSE, 0);
        for (child = TreeView_GetChild(g_hTree, hRoot); child;
             child = TreeView_GetNextSibling(g_hTree, child))
          ExpandAllItems(g_hTree, child, 0);
        TreeView_Expand(g_hTree, hRoot, TVE_EXPAND);
        SendMessage(g_hTree, WM_SETREDRAW, TRUE, 0);
        if (hFirst) TreeView_SelectSetFirstVisible(g_hTree, hFirst);
      }
      InvalidateRect(g_hTree, NULL, TRUE);
      break;
    }
    case IDM_WORDWRAP: {
      int len = GetWindowTextLengthW(g_hEditor);
      wchar_t *wtext = NULL;
      RECT edRc;
      HWND hOld = g_hEditor;
      if (len > 0) {
        wtext = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
        if (wtext) GetWindowTextW(g_hEditor, wtext, len + 1);
      }
      GetWindowRect(g_hEditor, &edRc);
      MapWindowPoints(HWND_DESKTOP, hWnd, (LPPOINT)&edRc, 2);
      g_cfg.word_wrap = !g_cfg.word_wrap;
      g_hEditor = CreateEditor(hWnd, g_cfg.word_wrap);
      SetWindowPos(g_hEditor, NULL, edRc.left, edRc.top,
        edRc.right - edRc.left, edRc.bottom - edRc.top, SWP_NOZORDER);
      SetEditorFont(g_hEditor);
      SendMessage(g_hEditor, EM_SETLIMITTEXT, 0, 0);
      if (wtext) {
        SetWindowTextW(g_hEditor, wtext);
        free(wtext);
      }
      DestroyWindow(hOld);
      SetFocus(g_hEditor);
      CheckMenuItem(GetMenu(hWnd), IDM_WORDWRAP,
        g_cfg.word_wrap ? MF_CHECKED : MF_UNCHECKED);
      break;
    }
    case IDM_FUZZYSEARCH: {
      g_cfg.fuzzy_search = !g_cfg.fuzzy_search;
      CheckMenuItem(GetMenu(hWnd), IDM_FUZZYSEARCH,
        g_cfg.fuzzy_search ? MF_CHECKED : MF_UNCHECKED);
      /* re-run any active filter under the new match mode */
      if (g_filterActive)
        SetTimer(hWnd, IDT_FILTER, FILTER_DEBOUNCE_MS, NULL);
      break;
    }
    case IDM_FORGETPASSWORD:
      PasswordCache_Clear();
      UpdateStatus();
      break;
    case IDM_ABOUT:
      MessageBox(hWnd,
        "Puren Tonbo " TONBO_VERSION_STRING "\n"
        "Plain text editor with encryption",
        "About Puren Tonbo", MB_OK | MB_ICONINFORMATION);
      break;
    case IDM_OPEN_DIR: {
      char dirPath[MAX_PATH];
      DWORD attrs;
      if (!g_rightClickPath[0]) break;
      strncpy(dirPath, g_rightClickPath, MAX_PATH - 1);
      dirPath[MAX_PATH - 1] = '\0';
      attrs = GetFileAttributesA(dirPath);
      if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        /* file: explore its containing directory instead */
        char *slash = strrchr(dirPath, '\\');
        if (slash) *slash = '\0';
      }
      ShellExecuteA(hWnd, "explore", dirPath, NULL, NULL, SW_SHOW);
      break;
    }
    case IDM_COPY_PATH: {
      if (g_rightClickPath[0]) {
        int rc = CopyTextToClipboard(hWnd, g_rightClickPath);
        if (rc != 0)
          MessageBoxA(hWnd, "Failed to copy path to clipboard", "Copy Full Name", MB_ICONERROR);
      }
      break;
    }
    case IDM_EXT_1: case IDM_EXT_2: case IDM_EXT_3:
    case IDM_EXT_4: case IDM_EXT_5: case IDM_EXT_6:
    case IDM_EXT_7: case IDM_EXT_8: case IDM_EXT_9: {
      int i = LOWORD(wParam) - IDM_EXT_1;
      char dirPath[MAX_PATH];
      char *slash;
      HINSTANCE h;
      assert(g_cfg.ext_exe[i][0]);
      strncpy(dirPath, g_rightClickPath, MAX_PATH - 1);
      dirPath[MAX_PATH - 1] = '\0';
      slash = strrchr(dirPath, '\\');
      if (slash) *slash = '\0';           /* working dir = file's directory */
      h = ShellExecuteA(hWnd, "open", g_cfg.ext_exe[i], g_rightClickPath,
                        dirPath, SW_SHOW);
      if ((INT_PTR)h <= 32)
        MessageBoxA(hWnd, "Failed to launch external editor",
                    g_cfg.ext_name[i], MB_ICONERROR);
      break;
    }
    case IDM_OPEN_ASSOC:
      if (g_rightClickPath[0])
        ShellExecuteA(hWnd, "open", g_rightClickPath, NULL, NULL, SW_SHOW);
      break;
    case IDM_NEW_FOLDER:
      NewFolderAt(g_hTree, g_rightClickItem,
        g_rightClickPath[0] ? g_rightClickPath : g_curDir);
      break;
    case IDM_RENAMEFILE: {
      HTREEITEM hSel = TreeView_GetSelection(g_hTree);
      if (hSel && hSel != TreeView_GetRoot(g_hTree))
        TreeView_EditLabel(g_hTree, hSel);
      break;
    }
    case IDM_RENAME:
      if (g_rightClickItem)
        TreeView_EditLabel(g_hTree, g_rightClickItem);
      break;
    case IDM_ENCRYPT_FILE:
      if (g_rightClickPath[0] && g_rightClickItem) {
        if (_stricmp(g_rightClickPath, g_curFile) == 0 && PromptSave() == IDCANCEL)
          break;
        EncryptFileToDisk(g_rightClickPath);
      }
      break;
    case IDM_DECRYPT_FILE:
      if (g_rightClickPath[0] && g_rightClickItem) {
        if (_stricmp(g_rightClickPath, g_curFile) == 0 && PromptSave() == IDCANCEL)
          break;
        DecryptFileToDisk(g_rightClickPath);
      }
      break;
    }
    return 0;

  case WM_NOTIFY: {
    NMHDR *nm = (NMHDR *)lParam;
    if (nm->idFrom == ID_TREE && nm->code == TVN_DELETEITEM) {
      NMTREEVIEW *tv = (NMTREEVIEW *)lParam;
      if (tv->itemOld.lParam)
        free((void *)tv->itemOld.lParam);
      return 0;
    }
    if (nm->idFrom == ID_TREE && nm->code == NM_RCLICK) {
      TVHITTESTINFO ht;
      DWORD msgPos = GetMessagePos();
      POINT pt;
      pt.x = (short)LOWORD(msgPos);
      pt.y = (short)HIWORD(msgPos);
      ht.pt = pt;
      ScreenToClient(g_hTree, &ht.pt);
      {
        HTREEITEM hItem = (HTREEITEM)SendMessage(g_hTree, TVM_HITTEST, 0, (LPARAM)&ht);
        if (hItem && (ht.flags & TVHT_ONITEM)) {
          TreeView_SelectItem(g_hTree, hItem);
          {
            TVITEM ti;
            ZeroMemory(&ti, sizeof(ti));
            ti.mask = TVIF_PARAM;
            ti.hItem = hItem;
            TreeView_GetItem(g_hTree, &ti);
            if (ti.lParam) {
              strncpy(g_rightClickPath, (const char *)ti.lParam, MAX_PATH - 1);
              g_rightClickPath[MAX_PATH - 1] = '\0';
              g_rightClickItem = hItem;
            } else {
              g_rightClickPath[0] = '\0';
              g_rightClickItem = NULL;
            }
          }
        } else {
          strncpy(g_rightClickPath, g_curDir, MAX_PATH - 1);
          g_rightClickPath[MAX_PATH - 1] = '\0';
          g_rightClickItem = NULL;
        }
      }
      {
        HMENU hPopup = CreatePopupMenu();
        if (g_rightClickItem) {
          BOOL isFile = g_rightClickPath[0] && is_note_ext(g_rightClickPath);
          if (isFile)
            AppendMenuA(hPopup, MF_STRING, IDM_OPEN_ASSOC, "Open");
          else if (g_rightClickPath[0])
            AppendMenuA(hPopup, MF_STRING, IDM_OPEN_DIR, "Open Directory");
          if (isFile && g_rightClickPath[0])
            AppendMenuA(hPopup, MF_STRING, IDM_OPEN_DIR, "Open Directory");
          if (g_rightClickPath[0])
            AppendMenuA(hPopup, MF_STRING, IDM_COPY_PATH, "Copy Full Name to Clipboard");
          /* External editors: files only.
           * 1 editor: flat item; 2: flat items; 3+: "Open With" submenu.
           * Digits 1-9 act as menu mnemonics while the menu is open. */
          if (isFile && g_rightClickPath[0]) {
            int i, n = 0, idx[EXT_EDITORS];
            for (i = 0; i < EXT_EDITORS; i++)
              if (g_cfg.ext_name[i][0] && g_cfg.ext_exe[i][0])
                idx[n++] = i;
            if (n == 1) {
              char label[128];
              sprintf(label, "Open with %s", g_cfg.ext_name[idx[0]]);
              AppendMenuA(hPopup, MF_STRING, IDM_EXT_1 + idx[0], label);
            } else if (n > 1) {
              HMENU hSub = (n > 2) ? CreatePopupMenu() : hPopup;
              for (i = 0; i < n; i++) {
                char label[128];
                sprintf(label, "&%d %s", idx[i] + 1, g_cfg.ext_name[idx[i]]);
                AppendMenuA(hSub, MF_STRING, IDM_EXT_1 + idx[i], label);
              }
              if (n > 2)
                AppendMenuA(hPopup, MF_POPUP, (UINT_PTR)hSub, "Open With");
            }
          }
          AppendMenuA(hPopup, MF_SEPARATOR, 0, NULL);
          AppendMenuA(hPopup, MF_STRING, IDM_RENAME, "Rename");
          AppendMenuA(hPopup, MF_SEPARATOR, 0, NULL);
        }
        {
          BOOL isFile = g_rightClickItem && g_rightClickPath[0] && is_note_ext(g_rightClickPath);
          if (isFile) {
            if (is_chi_file(g_rightClickPath))
              AppendMenuA(hPopup, MF_STRING, IDM_DECRYPT_FILE, "Decrypt");
            else
              AppendMenuA(hPopup, MF_STRING, IDM_ENCRYPT_FILE, "Encrypt");
          } else {
            AppendMenuA(hPopup, MF_STRING, IDM_NEW_FOLDER, "New Folder");
          }
        }
        {
          /* pt is already screen coordinates from GetMessagePos() -
           * converting again via ClientToScreen offsets the menu by
           * the tree's origin, so it appears away from the cursor. */
          /* SetForegroundWindow is required (KB135788): without it the
           * click that dismisses the menu falls through to the window
           * underneath and can trigger an unintended action. */
          SetForegroundWindow(hWnd);
          TrackPopupMenu(hPopup, TPM_LEFTBUTTON | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
          PostMessage(hWnd, WM_NULL, 0, 0);
          SetFocus(g_hTree);
        }
        DestroyMenu(hPopup);
      }
      return 0;
    }
    if (nm->idFrom == ID_TREE && nm->code == NM_DBLCLK) {
      HTREEITEM hSel = TreeView_GetSelection(g_hTree);
      if (hSel) {
        TVITEM ti;
        ZeroMemory(&ti, sizeof(ti));
        ti.mask = TVIF_PARAM;
        ti.hItem = hSel;
        TreeView_GetItem(g_hTree, &ti);
        if (ti.lParam && is_note_ext((const char *)ti.lParam) && PromptSave() != IDCANCEL)
          BF01OpenFile((const char *)ti.lParam);
      }
    }
    if (nm->idFrom == ID_TREE && nm->code == TVN_KEYDOWN) {
      NMTVKEYDOWN *kd = (NMTVKEYDOWN *)lParam;
      if (kd->wVKey == VK_RETURN) {
        HTREEITEM hSel = TreeView_GetSelection(g_hTree);
        if (hSel) {
          TVITEM ti;
          ZeroMemory(&ti, sizeof(ti));
          ti.mask = TVIF_PARAM;
          ti.hItem = hSel;
          TreeView_GetItem(g_hTree, &ti);
          if (ti.lParam && is_note_ext((const char *)ti.lParam)) {
            if (PromptSave() != IDCANCEL) BF01OpenFile((const char *)ti.lParam);
          } else {
            UINT state = TreeView_GetItemState(g_hTree, hSel, TVIS_EXPANDED);
            TreeView_Expand(g_hTree, hSel, (state & TVIS_EXPANDED) ? TVE_COLLAPSE : TVE_EXPAND);
          }
        }
        return 0;
      }
      if (kd->wVKey == VK_ADD || kd->wVKey == VK_SUBTRACT) {
        HTREEITEM hSel = TreeView_GetSelection(g_hTree);
        if (hSel) {
          TreeView_Expand(g_hTree, hSel,
            kd->wVKey == VK_ADD ? TVE_EXPAND : TVE_COLLAPSE);
        }
        return 0;
      }
    }
    if (nm->idFrom == ID_TREE && nm->code == TVN_BEGINLABELEDITA) {
      NMTVDISPINFOA *di = (NMTVDISPINFOA *)lParam;
      HTREEITEM hRoot = TreeView_GetRoot(g_hTree);
      if (di->item.hItem == hRoot)
        return TRUE;
      return FALSE;
    }
    if (nm->idFrom == ID_TREE && nm->code == TVN_ENDLABELEDITA) {
      NMTVDISPINFOA *di = (NMTVDISPINFOA *)lParam;
      if (di->item.pszText) {
        TVITEM ti;
        ZeroMemory(&ti, sizeof(ti));
        ti.mask = TVIF_PARAM;
        ti.hItem = di->item.hItem;
        if (TreeView_GetItem(g_hTree, &ti) && ti.lParam) {
          char oldPath[MAX_PATH], parentDir[MAX_PATH], newPath[MAX_PATH];
          char *lastSep;
          strncpy(oldPath, (const char *)ti.lParam, MAX_PATH - 1);
          oldPath[MAX_PATH - 1] = '\0';
          strncpy(parentDir, oldPath, MAX_PATH - 1);
          lastSep = strrchr(parentDir, '\\');
          if (lastSep) {
            *lastSep = '\0';
            snprintf(newPath, MAX_PATH, "%s\\%s", parentDir, di->item.pszText);
            if (strcmp(oldPath, newPath) != 0) {
              if (MoveFileA(oldPath, newPath)) {
                free((void *)ti.lParam);
                ti.lParam = (LPARAM)_strdup(newPath);
                TreeView_SetItem(g_hTree, &ti);
                if (_stricmp(oldPath, g_curFile) == 0) {
                  strncpy(g_curFile, newPath, MAX_PATH - 1);
                  g_curFile[MAX_PATH - 1] = '\0';
                  UpdateTitle();
                  UpdateStatus();
                }
              } else {
                MessageBox(g_hWnd, "Cannot rename: name may be in use or invalid",
                  "Error", MB_OK | MB_ICONERROR);
                return FALSE;
              }
            }
          }
        }
      }
      return TRUE;
    }
    return 0;
  }

  case WM_START_LABEL_EDIT:
    if (lParam) {
      SendMessage(g_hTree, TVM_EDITLABEL, 0, (LPARAM)lParam);
    }
    return 0;

  case WM_TIMER:
    if (wParam == IDT_PASSWORD) {
      if (g_pass_cached && GetTickCount() >= g_pass_expire_tick)
        PasswordCache_Clear();
    } else if (wParam == IDT_FILTER) {
      KillTimer(hWnd, IDT_FILTER);
      SearchApply(hWnd);
    }
    return 0;

  case WM_GETMINMAXINFO: {
    MINMAXINFO *mmi = (MINMAXINFO *)lParam;
    mmi->ptMinTrackSize.x = 400;
    mmi->ptMinTrackSize.y = 300;
    return 0;
  }

  case WM_CLI_OPEN: /* deferred command line file open */
    if (g_cliFile[0]) {
      BF01OpenFile(g_cliFile);
      SetFocus(g_hTree); /* keep tree focused on startup, as usual */
      g_cliFile[0] = '\0';
    }
    return 0;

  case WM_CLOSE:
    if (PromptSave() == IDCANCEL) return 0;
    PasswordCache_Clear();
    KillTimer(hWnd, IDT_PASSWORD);
    {
      RECT rc;
      AppConfig saved = g_cfg;
      if (g_cfg.persist_window) {
        GetWindowRect(hWnd, &rc);
        g_cfg.win_x = rc.left; g_cfg.win_y = rc.top;
        g_cfg.win_w = rc.right - rc.left; g_cfg.win_h = rc.bottom - rc.top;
      }
      g_cfg.tree_w = g_treeW;
      if (!config_equal(&g_cfg, &saved))
        config_save(&g_cfg, config_get_save_path());
    }
    if (g_editor_wtext) { free(g_editor_wtext); g_editor_wtext = NULL; }
    DestroyWindow(hWnd);
    return 0;

  case WM_DESTROY:
    if (g_hFont) DeleteObject(g_hFont);
    if (g_hSearchFont) DeleteObject(g_hSearchFont);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProc(hWnd, msg, wParam, lParam);
}

/* --- Tree view --- */

static void PopulateTree(HWND hTree, const char *dir, HTREEITEM hParent) {
  WIN32_FIND_DATA fd;
  char pattern[MAX_PATH], childPath[MAX_PATH];
  HANDLE hFind;
  TVINSERTSTRUCT tvi;
  int pass;
  int dirs_first = g_cfg.sort_dirs_first;

  for (pass = 0; pass < (dirs_first ? 2 : 1); pass++) {
    snprintf(pattern, MAX_PATH, "%s\\*", dir);
    hFind = FindFirstFile(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) continue;

    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (fd.cFileName[0] == '.') continue;
        if (dirs_first && pass != 0) continue;
        snprintf(childPath, MAX_PATH, "%s\\%s", dir, fd.cFileName);

        ZeroMemory(&tvi, sizeof(tvi));
        tvi.hParent = hParent;
        tvi.hInsertAfter = dirs_first ? TVI_LAST : TVI_SORT;
        tvi.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        tvi.item.pszText = fd.cFileName;
        tvi.item.iImage = 0;
        tvi.item.iSelectedImage = 0;
        tvi.item.lParam = (LPARAM)_strdup(childPath);

        {
          HTREEITEM hItem = TreeView_InsertItem(hTree, &tvi);
          PopulateTree(hTree, childPath, hItem);
        }
      } else if (is_note_ext(fd.cFileName)) {
        if (dirs_first && pass != 1) continue;
        {
          char *fullPath = (char *)malloc(MAX_PATH);
          if (!fullPath) continue;
          snprintf(fullPath, MAX_PATH, "%s\\%s", dir, fd.cFileName);

          ZeroMemory(&tvi, sizeof(tvi));
          tvi.hParent = hParent;
          tvi.hInsertAfter = dirs_first ? TVI_LAST : TVI_SORT;
          tvi.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
          tvi.item.pszText = fd.cFileName;
          tvi.item.iImage = 1;
          tvi.item.iSelectedImage = 1;
          tvi.item.lParam = (LPARAM)fullPath;
          TreeView_InsertItem(hTree, &tvi);
        }
      }
    } while (FindNextFile(hFind, &fd));
    FindClose(hFind);
  }
}

/* --- Expansion state preservation --- */

static char **g_expandedPaths;
static int g_nExpandedPaths;

static void FreeExpandedState(void) {
  int i;
  for (i = 0; i < g_nExpandedPaths; i++) free(g_expandedPaths[i]);
  free(g_expandedPaths);
  g_expandedPaths = NULL;
  g_nExpandedPaths = 0;
}

static void AddExpandedPath(const char *path) {
  char **p = (char **)realloc(g_expandedPaths,
    (g_nExpandedPaths + 1) * sizeof(char *));
  if (!p) return;
  g_expandedPaths = p;
  g_expandedPaths[g_nExpandedPaths++] = _strdup(path);
}

static int IsExpandedPath(const char *path) {
  int i;
  for (i = 0; i < g_nExpandedPaths; i++)
    if (strcmp(g_expandedPaths[i], path) == 0) return 1;
  return 0;
}

static void CaptureExpandedState(HWND hTree, HTREEITEM hItem) {
  for (; hItem; hItem = TreeView_GetNextSibling(hTree, hItem)) {
    TVITEM tvi;
    HTREEITEM child = TreeView_GetChild(hTree, hItem);
    ZeroMemory(&tvi, sizeof(tvi));
    tvi.hItem = hItem;
    tvi.mask = TVIF_STATE | TVIF_PARAM;
    tvi.stateMask = TVIS_EXPANDED;
    TreeView_GetItem(hTree, &tvi);
    if (child && (tvi.state & TVIS_EXPANDED) && tvi.lParam)
      AddExpandedPath((const char *)tvi.lParam);
    CaptureExpandedState(hTree, child);
  }
}

static void ApplyExpandedState(HWND hTree, HTREEITEM hItem) {
  for (; hItem; hItem = TreeView_GetNextSibling(hTree, hItem)) {
    TVITEM tvi;
    ZeroMemory(&tvi, sizeof(tvi));
    tvi.hItem = hItem;
    tvi.mask = TVIF_PARAM;
    TreeView_GetItem(hTree, &tvi);
    if (tvi.lParam && IsExpandedPath((const char *)tvi.lParam))
      TreeView_Expand(hTree, hItem, TVE_EXPAND);
    ApplyExpandedState(hTree, TreeView_GetChild(hTree, hItem));
  }
}

/* TreeView_Expand implicitly scrolls the affected item into view; run a
   batch of expands/collapses with redraw off, then re-anchor the same
   first-visible item. Item anchoring beats raw scroll offsets because the
   layout (item count/positions) changes during the batch. */
static void BatchTreeOp(HWND hTree, HTREEITEM hRoot, int expand) {
  HTREEITEM hFirst = TreeView_GetFirstVisible(hTree);
  SendMessage(hTree, WM_SETREDRAW, FALSE, 0);
  ExpandAllItems(hTree, hRoot, expand);
  SendMessage(hTree, WM_SETREDRAW, TRUE, 0);
  if (hFirst) TreeView_SelectSetFirstVisible(hTree, hFirst);
  InvalidateRect(hTree, NULL, TRUE);
}

static void ExpandAllItems(HWND hTree, HTREEITEM hItem, int expand) {
  for (; hItem; hItem = TreeView_GetNextSibling(hTree, hItem)) {
    HTREEITEM child = TreeView_GetChild(hTree, hItem);
    /* NOTE: no TVE_COLLAPSERESET - it deletes child items and this
       tree is eagerly populated, so folders would stay empty */
    if (child) TreeView_Expand(hTree, hItem,
      expand ? TVE_EXPAND : TVE_COLLAPSE);
    ExpandAllItems(hTree, child, expand);
  }
}

/* Rebuild the full unfiltered tree (root + PopulateTree + expand root).
   Caller handles expansion-state capture/apply. */
static void RebuildFullTree(void) {
  TVINSERTSTRUCT tvi;
  HTREEITEM hRoot;
  TreeView_DeleteAllItems(g_hTree);
  if (!g_curDir[0]) return;
  ZeroMemory(&tvi, sizeof(tvi));
  tvi.hParent = TVI_ROOT;
  tvi.hInsertAfter = TVI_LAST;
  tvi.item.mask = TVIF_TEXT;
  tvi.item.pszText = "Root";
  hRoot = TreeView_InsertItem(g_hTree, &tvi);
  PopulateTree(g_hTree, g_curDir, hRoot);
  TreeView_Expand(g_hTree, hRoot, TVE_EXPAND);
}

static void RefreshTree(void) {
  /* an external rebuild invalidates any active filter session */
  if (g_filterActive) {
    FreeExpandedState();
    FreeFilterCache();
    g_filterSnap = NULL;
    g_nFilterSnap = 0;
    g_filterActive = 0;
    SetWindowTextA(g_hSearch, "");
  }
  FreeExpandedState();
  if (g_hTree && g_curDir[0])
    CaptureExpandedState(g_hTree, TreeView_GetRoot(g_hTree));
  RebuildFullTree();
  ApplyExpandedState(g_hTree, TreeView_GetRoot(g_hTree));
  FreeExpandedState();
}

static void NewFolderAt(HWND hTree, HTREEITEM hParent, const char *parentPath) {
  char newPath[MAX_PATH];
  int n = 0;
  do {
    if (n == 0)
      snprintf(newPath, MAX_PATH, "%s\\New Folder", parentPath);
    else
      snprintf(newPath, MAX_PATH, "%s\\New Folder (%d)", parentPath, n);
    n++;
  } while (GetFileAttributesA(newPath) != INVALID_FILE_ATTRIBUTES);

  if (!CreateDirectoryA(newPath, NULL)) {
    MessageBox(g_hWnd, "Cannot create directory", "Error", MB_OK | MB_ICONERROR);
    return;
  }

  if (!hParent) {
    hParent = TreeView_GetRoot(hTree);
  }

  {
    TVINSERTSTRUCT tvi;
    const char *name;
    HTREEITEM hNew;
    ZeroMemory(&tvi, sizeof(tvi));
    tvi.hParent = hParent ? hParent : TVI_ROOT;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT | TVIF_PARAM;
    name = strrchr(newPath, '\\');
    tvi.item.pszText = (LPSTR)(name ? name + 1 : newPath);
    tvi.item.lParam = (LPARAM)_strdup(newPath);
    hNew = TreeView_InsertItem(hTree, &tvi);

    if (hNew) {
      TreeView_SelectItem(hTree, hNew);
      if (hParent)
        TreeView_Expand(hTree, hParent, TVE_EXPAND);
      TreeView_EnsureVisible(hTree, hNew);
      PostMessage(g_hWnd, WM_START_LABEL_EDIT, 0, (LPARAM)hNew);
    }
  }
}

/* --- Fuzzy tree filter --- */

static void FreeFilterCache(void) {
  int i;
  for (i = 0; i < g_nFilterItems; i++) free(g_filterItems[i]);
  free(g_filterItems);
  g_filterItems = NULL;
  g_nFilterItems = 0;
}

/* Recursively cache lParam paths of every non-root item. */
static void CollectTreePaths(HWND hTree, HTREEITEM hItem) {
  for (; hItem; hItem = TreeView_GetNextSibling(hTree, hItem)) {
    TVITEM tvi;
    ZeroMemory(&tvi, sizeof(tvi));
    tvi.hItem = hItem;
    tvi.mask = TVIF_PARAM;
    TreeView_GetItem(hTree, &tvi);
    if (tvi.lParam) {
      char **p = (char **)realloc(g_filterItems,
        (g_nFilterItems + 1) * sizeof(char *));
      if (p) {
        g_filterItems = p;
        g_filterItems[g_nFilterItems++] = _strdup((const char *)tvi.lParam);
      }
    }
    CollectTreePaths(hTree, TreeView_GetChild(hTree, hItem));
  }
}

/* Called at first keystroke of a filtering session: snapshot expansion
   state and cache all item paths (tree gets rebuilt during filtering). */
static void SearchSnapshot(void) {  FreeExpandedState();
  if (g_hTree && g_curDir[0])
    CaptureExpandedState(g_hTree, TreeView_GetRoot(g_hTree));
  g_filterSnap = g_expandedPaths;
  g_nFilterSnap = g_nExpandedPaths;
  g_expandedPaths = NULL;
  g_nExpandedPaths = 0;
  FreeFilterCache();
  CollectTreePaths(g_hTree,
    TreeView_GetChild(g_hTree, TreeView_GetRoot(g_hTree)));
  g_filterActive = 1;
}

/* Restore unfiltered tree from the snapshot taken at filter start. */
static void SearchClear(int focusTree) {
  if (!g_filterActive) {
    if (focusTree && g_hTree) SetFocus(g_hTree);
    return;
  }
  FreeExpandedState();
  g_expandedPaths = g_filterSnap;
  g_nExpandedPaths = g_nFilterSnap;
  g_filterSnap = NULL;
  g_nFilterSnap = 0;
  RebuildFullTree();
  ApplyExpandedState(g_hTree, TreeView_GetRoot(g_hTree));
  FreeExpandedState();
  FreeFilterCache();
  g_filterActive = 0;
  if (focusTree && g_hTree) SetFocus(g_hTree);
}

/* Folder-node map for the filtered rebuild: full dir path -> item handle. */
typedef struct { char path[MAX_PATH]; HTREEITEM h; } FilterNode;

static HTREEITEM EnsureDirNode(FilterNode **map, int *n, int *cap,
    HTREEITEM hRoot, const char *dirFullPath) {
  size_t baseLen = strlen(g_curDir);
  const char *p = dirFullPath + baseLen;
  char acc[MAX_PATH];
  HTREEITEM parent = hRoot;
  int i;

  if (dirFullPath + baseLen != dirFullPath
      && _strnicmp(dirFullPath, g_curDir, baseLen) != 0)
    return hRoot; /* unexpected prefix; bail to root */
  while (*p == '\\') p++;
  strncpy(acc, g_curDir, MAX_PATH - 1);
  acc[MAX_PATH - 1] = '\0';

  while (p && *p) {
    const char *q = strchr(p, '\\');
    size_t segLen = q ? (size_t)(q - p) : strlen(p);
    char seg[MAX_PATH];
    if (segLen >= MAX_PATH) segLen = MAX_PATH - 1;
    memcpy(seg, p, segLen);
    seg[segLen] = '\0';
    if (acc[0] && strcat_s(acc, sizeof(acc), "\\") != 0) return parent;
    if (strlen(acc) + segLen >= MAX_PATH) return parent;
    strcat(acc, seg);

    /* find or create node for acc */
    for (i = 0; i < *n; i++)
      if ((*map)[i].path[0] && strcmp((*map)[i].path, acc) == 0) break;
    if (i < *n) {
      parent = (*map)[i].h;
    } else {
      TVINSERTSTRUCT tvi;
      HTREEITEM hNew;
      ZeroMemory(&tvi, sizeof(tvi));
      tvi.hParent = parent;
      tvi.hInsertAfter = TVI_LAST;
      tvi.item.mask = TVIF_TEXT | TVIF_PARAM;
      tvi.item.pszText = seg;
      tvi.item.lParam = (LPARAM)_strdup(acc);
      hNew = TreeView_InsertItem(g_hTree, &tvi);
      if (!hNew) return parent;
      if (*n == *cap) {
        int newCap = *cap ? *cap * 2 : 64;
        FilterNode *m = (FilterNode *)realloc(*map,
          newCap * sizeof(FilterNode));
        if (!m) return parent;
        *map = m;
        *cap = newCap;
      }
      strncpy((*map)[*n].path, acc, MAX_PATH - 1);
      (*map)[*n].path[MAX_PATH - 1] = '\0';
      (*map)[*n].h = hNew;
      (*n)++;
      parent = hNew;
    }
    p = q ? q + 1 : NULL;
  }
  return parent;
}

/* ASCII case-insensitive find; returns position or -1. */
static int SubstrPos(const char *hay, const char *needle) {
  size_t nlen = strlen(needle);
  const char *p;
  if (!nlen) return 0;
  for (p = hay; *p; p++) {
    size_t i;
    for (i = 0; i < nlen; i++) {
      if (tolower((unsigned char)p[i]) != tolower((unsigned char)needle[i]))
        break;
      if (!p[i]) return -1;
    }
    if (i == nlen) return (int)(p - hay);
  }
  return -1;
}

typedef struct { const char *path; int32_t score; } MatchEntry;

static int MatchCmp(const void *a, const void *b) {
  return strcmp(((const MatchEntry *)a)->path, ((const MatchEntry *)b)->path);
}

/* Debounced filter application: rebuild tree with matching items plus
   their ancestor folders, all expanded; select best-scoring match. */
static void SearchApply(HWND hWnd) {
  char pat[256];
  MatchEntry *matches = NULL;
  int nMatches = 0, capMatches = 0, i;
  FilterNode *map = NULL;
  int nMap = 0, capMap = 0;
  HTREEITEM hRoot, hBest = NULL;
  int32_t bestScore = INT32_MAX;
  TVINSERTSTRUCT tvi;
  size_t baseLen;

  if (!g_filterActive || !g_curDir[0]) return;
  GetWindowTextA(g_hSearch, pat, sizeof(pat));
  if (!pat[0]) { SearchClear(0); return; }
  baseLen = strlen(g_curDir);

  /* score every cached item against its path relative to root */
  for (i = 0; i < g_nFilterItems; i++) {
    const char *rel;
    int32_t score;
    if (_strnicmp(g_filterItems[i], g_curDir, baseLen) != 0) continue;
    rel = g_filterItems[i] + baseLen;
    while (*rel == '\\') rel++;
    if (g_cfg.fuzzy_search) {
      score = fts_fuzzy_match(pat, rel);
    } else {
      /* simple partial string match, case-insensitive;
         earlier occurrence ranks better
         TODO: option B - no ranking, first item in tree order wins */
      int pos = SubstrPos(rel, pat);
      if (pos < 0) continue;
      score = -pos;
    }
    if (g_cfg.fuzzy_search && score == INT32_MIN) continue;
    if (nMatches == capMatches) {
      int newCap = capMatches ? capMatches * 2 : 64;
      MatchEntry *m = (MatchEntry *)realloc(matches,
        newCap * sizeof(MatchEntry));
      if (!m) break;
      matches = m;
      capMatches = newCap;
    }
    matches[nMatches].path = g_filterItems[i];
    matches[nMatches].score = score;
    nMatches++;
  }

  SendMessage(g_hTree, WM_SETREDRAW, FALSE, 0);
  TreeView_DeleteAllItems(g_hTree);
  ZeroMemory(&tvi, sizeof(tvi));
  tvi.hParent = TVI_ROOT;
  tvi.hInsertAfter = TVI_LAST;
  tvi.item.mask = TVIF_TEXT;
  tvi.item.pszText = "Root";
  hRoot = TreeView_InsertItem(g_hTree, &tvi);

  if (matches) qsort(matches, nMatches, sizeof(MatchEntry), MatchCmp);

  for (i = 0; matches && i < nMatches; i++) {
    const char *fullPath = matches[i].path;
    const char *leaf = strrchr(fullPath, '\\');
    HTREEITEM hParent = EnsureDirNode(&map, &nMap, &capMap,
      hRoot, fullPath);
    HTREEITEM hNew;
    leaf = leaf ? leaf + 1 : fullPath;
    ZeroMemory(&tvi, sizeof(tvi));
    tvi.hParent = hParent;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT | TVIF_PARAM;
    tvi.item.pszText = (LPSTR)leaf;
    tvi.item.lParam = (LPARAM)_strdup(fullPath);
    hNew = TreeView_InsertItem(g_hTree, &tvi);
    if (hNew && matches[i].score < bestScore) {
      bestScore = matches[i].score;
      hBest = hNew;
    }
  }

  ExpandAllItems(g_hTree, hRoot, 1);
  SendMessage(g_hTree, WM_SETREDRAW, TRUE, 0);
  if (hBest) {
    TreeView_SelectItem(g_hTree, hBest);
    TreeView_EnsureVisible(g_hTree, hBest);
  }
  InvalidateRect(g_hTree, NULL, TRUE);

  free(matches);
  free(map);
  (void)hWnd;
}

/* --- File I/O --- */

static int is_chi_file(const char *path) {
  const char *dot = strrchr(path, '.');
  return dot && (!_stricmp(dot, ".chi") || !_stricmp(dot, ".chs"));
}

static int strip_cr_w(wchar_t *buf, int len) {
  int w = 0, r;
  for (r = 0; r < len; r++) {
    if (buf[r] != L'\r') buf[w++] = buf[r];
  }
  return w;
}

static wchar_t *expand_lf_w(const wchar_t *src, int srclen, int *dstlen) {
  int i, count = 0;
  wchar_t *dst;
  for (i = 0; i < srclen; i++)
    count += (src[i] == L'\n') ? 2 : 1;
  dst = (wchar_t *)malloc((count + 1) * sizeof(wchar_t));
  if (!dst) { *dstlen = 0; return NULL; }
  {
    int w = 0;
    for (i = 0; i < srclen; i++) {
      if (src[i] == L'\n') dst[w++] = L'\r';
      dst[w++] = src[i];
    }
    dst[w] = L'\0';
    *dstlen = w;
  }
  return dst;
}

static void SetEditorTextW(const wchar_t *wtext) {
  SetWindowTextW(g_hEditor, wtext);
}

static int GetEditorTextW(wchar_t **out_w, int *out_wlen) {
  int wlen = GetWindowTextLengthW(g_hEditor);
  *out_w = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
  if (!*out_w) { *out_wlen = 0; return 0; }
  GetWindowTextW(g_hEditor, *out_w, wlen + 1);
  *out_wlen = wlen;
  return 1;
}

static void BF01OpenFileRaw(const unsigned char *raw, long rawlen) {
  wchar_t *wtext = NULL;
  int wlen = 0;
  int bom_len = 0;
  UINT cp;
  int i;

  cp = encoding_detect_bom(raw, rawlen, &bom_len);
  if (cp) {
    g_file_cp = cp;
    if (cp == 1200 || cp == 1201) {
      int wlen2 = MultiByteToWideChar(cp, 0, (const char *)raw + bom_len, (int)(rawlen - bom_len), NULL, 0);
      if (wlen2 > 0) {
        wtext = (wchar_t *)malloc((wlen2 + 1) * sizeof(wchar_t));
        if (wtext) {
          MultiByteToWideChar(cp, 0, (const char *)raw + bom_len, (int)(rawlen - bom_len), wtext, wlen2);
          wtext[wlen2] = L'\0';
          wlen = wlen2;
        }
      }
    } else {
      encoding_to_wide(raw + bom_len, (int)(rawlen - bom_len), cp, &wtext, &wlen);
    }
  }

  if (!wtext) {
    for (i = 0; i < g_cfg.encoding_count; i++) {
      if (encoding_to_wide(raw + bom_len, (int)(rawlen - bom_len), g_cfg.encoding_cps[i], &wtext, &wlen)) {
        g_file_cp = g_cfg.encoding_cps[i];
        break;
      }
    }
  }

  if (!wtext) {
    MessageBox(g_hWnd, "Cannot decode file with any configured encoding", "Error", MB_OK | MB_ICONERROR);
    return;
  }

  {
    int explen;
    wchar_t *expanded = expand_lf_w(wtext, wlen, &explen);
    free(wtext);
    if (!expanded) return;
    SetEditorTextW(expanded);
    if (g_editor_wtext) free(g_editor_wtext);
    g_editor_wtext = expanded;
    g_editor_wlen = explen;
  }
}

/* Recursively find the tree item whose lParam path matches path (case-insensitive). */
static HTREEITEM FindTreeItemByPath(HWND hTree, HTREEITEM hParent, const char *path) {
  HTREEITEM hItem;
  TVITEM ti;
  char itemPath[MAX_PATH];

  if (!hTree) return NULL;
  hItem = TreeView_GetChild(hTree, hParent);
  while (hItem) {
    ZeroMemory(&ti, sizeof(ti));
    ti.mask = TVIF_PARAM | TVIF_HANDLE;
    ti.hItem = hItem;
    if (TreeView_GetItem(hTree, &ti) && ti.lParam) {
      strncpy(itemPath, (const char *)ti.lParam, MAX_PATH - 1);
      itemPath[MAX_PATH - 1] = '\0';
      if (!_stricmp(itemPath, path)) return hItem;
      /* search inside subdirectories (dirs have non-note-extension paths) */
      if (!is_note_ext(itemPath)) {
        HTREEITEM hFound = FindTreeItemByPath(hTree, hItem, path);
        if (hFound) return hFound;
      }
    }
    hItem = TreeView_GetNextSibling(hTree, hItem);
  }
  return NULL;
}

static void SelectTreeItemByPath(const char *path) {
  HTREEITEM hRoot = TreeView_GetRoot(g_hTree);
  HTREEITEM hItem = FindTreeItemByPath(g_hTree, hRoot, path);
  if (hItem) {
    TreeView_SelectItem(g_hTree, hItem);
    TreeView_EnsureVisible(g_hTree, hItem);
  }
}

static void BF01OpenFile(const char *path) {
  SelectTreeItemByPath(path);
  {
    /* External encryption binary (or tool) file: child decrypts to stdout,
     * tonbo reads plain text via pipe. */
    const CryptoTool *ct = crypto_tool_for_path(&g_cfg, path);
    if (ct) {
      char pass[256] = "";
      char err[1024] = "";
      char *plain = NULL;
      size_t plainlen = 0;

      if (!AskPassword(pass, sizeof(pass), 0)) {
        SetFocus(g_hTree);
        return;
      }

      if (crypto_decrypt_file(ct, path, pass, &plain, &plainlen, err, sizeof(err))) {
        MessageBox(g_hWnd, err, "Decryption failed", MB_OK | MB_ICONERROR);
        SetFocus(g_hTree);
        return;
      }
      PasswordCache_Set(pass);
      PasswordCache_ResetTimer();
      BF01OpenFileRaw((unsigned char *)plain, (long)plainlen);
      free(plain);
    }
    else if (is_chi_file(path)) {
    char pass[256] = "";
    FILE *fin;
    unsigned char *filedata;
    long filesize;
    unsigned char *plain;
    size_t plainlen;

    if (!AskPassword(pass, sizeof(pass), 0)) {
      SetFocus(g_hTree);
      return;
    }

    fin = fopen(path, "rb");
    if (!fin) { MessageBox(g_hWnd, "Cannot open file", "Error", MB_OK | MB_ICONERROR); SetFocus(g_hTree); return; }
    fseek(fin, 0, SEEK_END);
    filesize = ftell(fin);
    rewind(fin);
    filedata = (unsigned char *)malloc(filesize);
    if (!filedata) { fclose(fin); MessageBox(g_hWnd, "Out of memory", "Error", MB_OK | MB_ICONERROR); SetFocus(g_hTree); return; }
    fread(filedata, 1, filesize, fin);
    fclose(fin);

    plain = bf01_decrypt_mem(filedata, filesize, pass, &plainlen, 0);
    free(filedata);
    if (!plain) {
      MessageBox(g_hWnd, "Decryption failed (wrong password?)", "Error", MB_OK | MB_ICONERROR);
      SetFocus(g_hTree);
      return;
    }
    PasswordCache_Set(pass);
    PasswordCache_ResetTimer();
    BF01OpenFileRaw(plain, (long)plainlen);
    free(plain);
  } else {
    FILE *f = fopen(path, "rb");
    long sz;
    unsigned char *buf;
    if (!f) { MessageBox(g_hWnd, "Cannot open file", "Error", MB_OK | MB_ICONERROR); return; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);
    buf = (unsigned char *)malloc(sz);
    if (buf) {
      fread(buf, 1, sz, f);
      BF01OpenFileRaw(buf, sz);
      free(buf);
    }
    fclose(f);
  }
  strncpy(g_curFile, path, MAX_PATH - 1);
  g_curFile[MAX_PATH - 1] = '\0';
  {
    const char *lastSlash = strrchr(path, '\\');
    if (!lastSlash) lastSlash = strrchr(path, '/');
    if (lastSlash) {
      int len = (int)(lastSlash - path);
      strncpy(g_curDir, path, len);
      g_curDir[len] = '\0';
    }
  }
  g_dirty = FALSE;
  SendMessage(g_hEditor, EM_SETMODIFY, FALSE, 0);
  UpdateMenuSaveState(g_hWnd);
  UpdateTitle();
  UpdateStatus();
  }
}

static void SaveCurrentFile(void) {
  wchar_t *wbuf;
  int wlen;
  char *bytes = NULL;
  int blen = 0;
  UINT save_cp;
  if (!g_curFile[0]) { SaveFileAs(); return; }

  if (!GetEditorTextW(&wbuf, &wlen)) return;
  wlen = strip_cr_w(wbuf, wlen);

  save_cp = g_file_cp ? g_file_cp : (g_cfg.encoding_count > 0 ? g_cfg.encoding_cps[0] : CP_UTF8);
  if (!wide_to_encoding(wbuf, wlen, save_cp, &bytes, &blen)) {
    MessageBox(g_hWnd, "Cannot encode text to target encoding", "Error", MB_OK | MB_ICONERROR);
    free(wbuf);
    return;
  }
  free(wbuf);

  {
    /* External encryption binary (or tool): tonbo writes plain text to the
     * child's stdin, child writes the encrypted output file itself. */
    const CryptoTool *ct = crypto_tool_for_path(&g_cfg, g_curFile);
    if (ct) {
      char pass[256] = "";
      char err[1024] = "";
      if (!AskPassword(pass, sizeof(pass), 1)) {
        free(bytes);
        return;
      }
      /* TODO add support for safe save with command line tools that support that */
  /* NOTE: encrypt path not yet verified end-to-end (child did not exit in
   * smoke test); decrypt is the supported path for now. */
      if (crypto_encrypt_file(ct, g_curFile, bytes, (size_t)blen, pass, err, sizeof(err))) {
        free(bytes);
        MessageBox(g_hWnd, err, "Encryption failed", MB_OK | MB_ICONERROR);
        return;
      }
      PasswordCache_Set(pass);
      PasswordCache_ResetTimer();
      free(bytes);
      g_dirty = FALSE;
      SendMessage(g_hEditor, EM_SETMODIFY, FALSE, 0);
      UpdateMenuSaveState(g_hWnd);
      UpdateTitle();
      RefreshTree();
      return;
    }
  }

  if (is_chi_file(g_curFile)) {
    char pass[256] = "";
    unsigned char *cipher;
    size_t cipherlen;
    if (!AskPassword(pass, sizeof(pass), 1)) {
      free(bytes);
      return;
    }
    cipher = bf01_encrypt_mem((unsigned char *)bytes, blen, pass, &cipherlen, 0);
    if (!cipher) {
      MessageBox(g_hWnd, "Encryption failed", "Error", MB_OK | MB_ICONERROR);
      free(bytes);
      return;
    }
    PasswordCache_Set(pass);
    PasswordCache_ResetTimer();

    if (g_cfg.safe_save) {
      SYSTEMTIME st;
      char tmpPath[MAX_PATH];
      FILE *f;
      GetLocalTime(&st);
      snprintf(tmpPath, MAX_PATH, "%s.tmp.%04d%02d%02d_%02d%02d%02d",
        g_curFile, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
      f = fopen(tmpPath, "wb");
      if (!f) { free(cipher); free(bytes); MessageBox(g_hWnd, "Cannot write temp file", "Error", MB_OK | MB_ICONERROR); return; }
#ifdef DEBUG_TRUNCATE_SAVE_CORRUPTION_CHECK
      fwrite(cipher, 1, cipherlen-1, f);
#else
      fwrite(cipher, 1, cipherlen, f);
#endif
      fclose(f);
      if (g_cfg.paranoid_save) {
        unsigned char *verify = (unsigned char *)malloc(cipherlen);
        FILE *fv = fopen(tmpPath, "rb");
        int ok = verify && fv && fread(verify, 1, cipherlen, fv) == cipherlen && memcmp(verify, cipher, cipherlen) == 0;
        if (fv) fclose(fv);
        free(verify);
        if (!ok) {
          char msg[MAX_PATH + 64];
          free(cipher); free(bytes);
          snprintf(msg, sizeof(msg), "Verify failed: temp file does not match source\n%s", tmpPath);
          MessageBox(g_hWnd, msg, "Error", MB_OK | MB_ICONERROR);
          return;
        }
      }
      if (!DeleteFile(g_curFile) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        DeleteFile(tmpPath);
        free(cipher); free(bytes);
        MessageBox(g_hWnd, "Cannot delete original file", "Error", MB_OK | MB_ICONERROR);
        return;
      }
      if (!MoveFile(tmpPath, g_curFile)) {
        DeleteFile(tmpPath);
        free(cipher); free(bytes);
        MessageBox(g_hWnd, "Cannot rename temp file", "Error", MB_OK | MB_ICONERROR);
        return;
      }
      free(cipher);
    } else {
      FILE *f = fopen(g_curFile, "wb");
      if (!f) { free(cipher); free(bytes); MessageBox(g_hWnd, "Cannot write file", "Error", MB_OK | MB_ICONERROR); return; }
      fwrite(cipher, 1, cipherlen, f);
      fclose(f);
      free(cipher);
    }
  } else {
    if (g_cfg.safe_save) {
      SYSTEMTIME st;
      char tmpPath[MAX_PATH];
      FILE *f;
      GetLocalTime(&st);
      snprintf(tmpPath, MAX_PATH, "%s.tmp.%04d%02d%02d_%02d%02d%02d",
        g_curFile, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
      f = fopen(tmpPath, "wb");
      if (!f) { free(bytes); MessageBox(g_hWnd, "Cannot write temp file", "Error", MB_OK | MB_ICONERROR); return; }
#ifdef DEBUG_TRUNCATE_SAVE_CORRUPTION_CHECK
      fwrite(bytes, 1, blen-1, f);
#else
      fwrite(bytes, 1, blen, f);
#endif
      fclose(f);
      if (g_cfg.paranoid_save) {
        char *verify = (char *)malloc(blen);
        FILE *fv = fopen(tmpPath, "rb");
        int ok = verify && fv && fread(verify, 1, blen, fv) == (size_t)blen && memcmp(verify, bytes, blen) == 0;
        if (fv) fclose(fv);
        free(verify);
        if (!ok) {
          char msg[MAX_PATH + 64];
          free(bytes);
          snprintf(msg, sizeof(msg), "Verify failed: temp file does not match source\n%s", tmpPath);
          MessageBox(g_hWnd, msg, "Error", MB_OK | MB_ICONERROR);
          return;
        }
      }
      if (!DeleteFile(g_curFile) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        DeleteFile(tmpPath);
        free(bytes);
        MessageBox(g_hWnd, "Cannot delete original file", "Error", MB_OK | MB_ICONERROR);
        return;
      }
      if (!MoveFile(tmpPath, g_curFile)) {
        DeleteFile(tmpPath);
        free(bytes);
        MessageBox(g_hWnd, "Cannot rename temp file", "Error", MB_OK | MB_ICONERROR);
        return;
      }
    } else {
      FILE *f = fopen(g_curFile, "wb");
      if (!f) { free(bytes); MessageBox(g_hWnd, "Cannot write file", "Error", MB_OK | MB_ICONERROR); return; }
      fwrite(bytes, 1, blen, f);
      fclose(f);
    }
  }
  free(bytes);
  g_dirty = FALSE;
  SendMessage(g_hEditor, EM_SETMODIFY, FALSE, 0);
  UpdateMenuSaveState(g_hWnd);
  UpdateTitle();
  RefreshTree();
}

static void SaveFileAs(void) {
  OPENFILENAME ofn;
  char file[MAX_PATH] = "";
  char filter[1024];
  const char *dot;
  if (g_curFile[0]) strncpy(file, g_curFile, MAX_PATH - 1);

  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_hWnd;
  BuildFileDialogFilter(filter, sizeof(filter));
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrInitialDir = g_curDir[0] ? g_curDir : NULL;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

  if (GetSaveFileName(&ofn)) {
    dot = strrchr(file, '.');
    if (!dot || (_stricmp(dot, ".txt") && _stricmp(dot, ".chi") && _stricmp(dot, ".chs") && _stricmp(dot, ".md")
                 && !crypto_is_tool_ext(&g_cfg, file)))
      strncat(file, ".txt", MAX_PATH - strlen(file) - 1);
    strncpy(g_curFile, file, MAX_PATH - 1);
    g_curFile[MAX_PATH - 1] = '\0';
    SaveCurrentFile();
  }
}

static int PromptSave(void) {
  if (!g_dirty) return IDNO;
  {
    int r = MessageBox(g_hWnd, "Save changes?", "Puren Tonbo", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDYES) SaveCurrentFile();
    return r;
  }
}

static void EncryptFileToDisk(const char *path) {
  FILE *f;
  long sz;
  unsigned char *buf, *cipher;
  size_t cipherlen;
  char pass[256];
  char chiPath[MAX_PATH];

  if (is_chi_file(path) || crypto_is_tool_ext(&g_cfg, path)) return;

  f = fopen(path, "rb");
  if (!f) { MessageBox(g_hWnd, "Cannot open file", "Error", MB_OK | MB_ICONERROR); return; }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  rewind(f);
  buf = (unsigned char *)malloc(sz);
  if (!buf) { fclose(f); return; }
  fread(buf, 1, sz, f);
  fclose(f);

  if (!AskPassword(pass, sizeof(pass), 1)) { free(buf); return; }

  cipher = bf01_encrypt_mem(buf, sz, pass, &cipherlen, 0);
  free(buf);
  if (!cipher) { MessageBox(g_hWnd, "Encryption failed", "Error", MB_OK | MB_ICONERROR); return; }

  PasswordCache_Set(pass);
  PasswordCache_ResetTimer();

  strncpy(chiPath, path, MAX_PATH - 1);
  chiPath[MAX_PATH - 1] = '\0';
  {
    char *dot = strrchr(chiPath, '.');
    if (dot) *dot = '\0';
  }
  strncat(chiPath, ".chi", MAX_PATH - strlen(chiPath) - 1);

  if (g_cfg.safe_save) {
    SYSTEMTIME st;
    char tmpPath[MAX_PATH];
    FILE *ft;
    GetLocalTime(&st);
    snprintf(tmpPath, MAX_PATH, "%s.tmp.%04d%02d%02d_%02d%02d%02d",
      chiPath, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    ft = fopen(tmpPath, "wb");
    if (!ft) { free(cipher); MessageBox(g_hWnd, "Cannot write temp file", "Error", MB_OK | MB_ICONERROR); return; }
    fwrite(cipher, 1, cipherlen, ft);
    fclose(ft);
    if (!DeleteFileA(chiPath) && GetLastError() != ERROR_FILE_NOT_FOUND) {
      DeleteFileA(tmpPath);
      free(cipher);
      MessageBox(g_hWnd, "Cannot replace encrypted file", "Error", MB_OK | MB_ICONERROR);
      return;
    }
    if (!MoveFileA(tmpPath, chiPath)) {
      DeleteFileA(tmpPath);
      free(cipher);
      MessageBox(g_hWnd, "Cannot rename temp file", "Error", MB_OK | MB_ICONERROR);
      return;
    }
  } else {
    FILE *fo = fopen(chiPath, "wb");
    if (!fo) { free(cipher); MessageBox(g_hWnd, "Cannot write encrypted file", "Error", MB_OK | MB_ICONERROR); return; }
    fwrite(cipher, 1, cipherlen, fo);
    fclose(fo);
  }
  free(cipher);

#ifndef DEBUG_PRESERVE_OLD_FILES
  {
    char bakPath[MAX_PATH];
    snprintf(bakPath, MAX_PATH, "%s.bak", path);
    DeleteFileA(bakPath);
    MoveFileA(path, bakPath);
  }
#else
  DeleteFileA(path);
#endif // DEBUG_PRESERVE_OLD_FILES

  if (_stricmp(path, g_curFile) == 0) {
    g_curFile[0] = '\0';
    SetWindowTextW(g_hEditor, L"");
    if (g_editor_wtext) { free(g_editor_wtext); g_editor_wtext = NULL; }
    g_editor_wlen = 0;
    g_file_cp = 0;
    g_dirty = FALSE;
    UpdateMenuSaveState(g_hWnd);
    UpdateTitle();
    UpdateStatus();
  }

  RefreshTree();
}

static void DecryptFileToDisk(const char *path) {
  FILE *f;
  long sz;
  unsigned char *filedata, *plain;
  size_t plainlen;
  char pass[256];
  char txtPath[MAX_PATH];

  if (!is_chi_file(path) || crypto_is_tool_ext(&g_cfg, path)) return;

  f = fopen(path, "rb");
  if (!f) { MessageBox(g_hWnd, "Cannot open file", "Error", MB_OK | MB_ICONERROR); return; }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  rewind(f);
  filedata = (unsigned char *)malloc(sz);
  if (!filedata) { fclose(f); return; }
  fread(filedata, 1, sz, f);
  fclose(f);

  if (!AskPassword(pass, sizeof(pass), 0)) { free(filedata); return; }

  plain = bf01_decrypt_mem(filedata, sz, pass, &plainlen, 0);
  free(filedata);
  if (!plain) {
    MessageBox(g_hWnd, "Decryption failed (wrong password?)", "Error", MB_OK | MB_ICONERROR);
    return;
  }

  PasswordCache_Set(pass);
  PasswordCache_ResetTimer();

  strncpy(txtPath, path, MAX_PATH - 1);
  txtPath[MAX_PATH - 1] = '\0';
  {
    char *dot = strrchr(txtPath, '.');
    if (dot) strcpy(dot, ".txt");
    else strncat(txtPath, ".txt", MAX_PATH - strlen(txtPath) - 1);
  }

  if (g_cfg.safe_save) {
    SYSTEMTIME st;
    char tmpPath[MAX_PATH];
    FILE *ft;
    GetLocalTime(&st);
    snprintf(tmpPath, MAX_PATH, "%s.tmp.%04d%02d%02d_%02d%02d%02d",
      txtPath, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    ft = fopen(tmpPath, "wb");
    if (!ft) { free(plain); MessageBox(g_hWnd, "Cannot write temp file", "Error", MB_OK | MB_ICONERROR); return; }
    fwrite(plain, 1, plainlen, ft);
    fclose(ft);
    if (!DeleteFileA(txtPath) && GetLastError() != ERROR_FILE_NOT_FOUND) {
      DeleteFileA(tmpPath);
      free(plain);
      MessageBox(g_hWnd, "Cannot replace text file", "Error", MB_OK | MB_ICONERROR);
      return;
    }
    if (!MoveFileA(tmpPath, txtPath)) {
      DeleteFileA(tmpPath);
      free(plain);
      MessageBox(g_hWnd, "Cannot rename temp file", "Error", MB_OK | MB_ICONERROR);
      return;
    }
  } else {
    FILE *fo = fopen(txtPath, "wb");
    if (!fo) { free(plain); MessageBox(g_hWnd, "Cannot write text file", "Error", MB_OK | MB_ICONERROR); return; }
    fwrite(plain, 1, plainlen, fo);
    fclose(fo);
  }
  free(plain);

#ifndef DEBUG_PRESERVE_OLD_FILES
  {
    char bakPath[MAX_PATH];
    snprintf(bakPath, MAX_PATH, "%s.bak", path);
    DeleteFileA(bakPath);
    MoveFileA(path, bakPath);
  }
#else
  DeleteFileA(path);
#endif // DEBUG_PRESERVE_OLD_FILES

  if (_stricmp(path, g_curFile) == 0) {
    g_curFile[0] = '\0';
    SetWindowTextW(g_hEditor, L"");
    if (g_editor_wtext) { free(g_editor_wtext); g_editor_wtext = NULL; }
    g_editor_wlen = 0;
    g_file_cp = 0;
    g_dirty = FALSE;
    UpdateMenuSaveState(g_hWnd);
    UpdateTitle();
    UpdateStatus();
  }

  RefreshTree();
}

static void UpdateTitle(void) {
  char title[512];
  if (g_curFile[0])
    snprintf(title, sizeof(title), "Puren Tonbo - %s%s", g_dirty ? "*" : "", g_curFile);
  else
    snprintf(title, sizeof(title), "Puren Tonbo - %sUntitled", g_dirty ? "*" : "");
  SetWindowTextA(g_hWnd, title);
}

static void UpdateStatus(void) {
  char status[256];
  if (g_curFile[0])
    snprintf(status, sizeof(status), "%s", g_curFile);
  else
    snprintf(status, sizeof(status), "No file");
  SendMessage(g_hStatus, SB_SETTEXT, 0, (LPARAM)status);
}

/* --- Password Dialog --- */

static int AskPassword(char *passBuf, int bufsize, int encrypt) {
  MSG msg;
  HWND hPass, hEdit, hEdit2, hOk, hCancel;
  HWND hLabel2;
  HFONT hDlgFont;
  RECT rc;

  if (PasswordCache_Get(passBuf, bufsize)) return 1;

  hDlgFont = CreateFont(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    DEFAULT_QUALITY, DEFAULT_PITCH, "MS Shell Dlg");

  GetWindowRect(g_hWnd, &rc);
  hPass = CreateWindowEx(WS_EX_DLGMODALFRAME, "PassDialog", "Password",
    WS_POPUP | WS_CAPTION | WS_SYSMENU,
    (rc.left + rc.right) / 2 - 120, (rc.top + rc.bottom) / 2 - 124,
    240, 220, g_hWnd, NULL, g_hInst, NULL);

  CreateWindow("STATIC", "Enter password:", WS_CHILD | WS_VISIBLE,
    10, 10, 200, 20, hPass, NULL, g_hInst, NULL);
  hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
    10, 35, 210, 24, hPass, (HMENU)IDC_PASS_EDIT, g_hInst, NULL);

  hLabel2 = CreateWindow("STATIC", "Confirm password:", WS_CHILD | WS_VISIBLE,
    10, 65, 200, 20, hPass, NULL, g_hInst, NULL);
  hEdit2 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
    10, 85, 210, 24, hPass, (HMENU)IDC_PASS_EDIT2, g_hInst, NULL);

  CreateWindow("BUTTON", "Show password",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
    10, 118, 210, 20, hPass, (HMENU)IDC_PASS_SHOW, g_hInst, NULL);

  hOk = CreateWindow("BUTTON", "OK",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
    10, 148, 95, 28, hPass, (HMENU)IDC_PASS_OK, g_hInst, NULL);
  hCancel = CreateWindow("BUTTON", "Cancel",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
    115, 148, 95, 28, hPass, (HMENU)IDC_PASS_CANCEL, g_hInst, NULL);

  SendMessage(hEdit, WM_SETFONT, (WPARAM)hDlgFont, TRUE);
  SendMessage(hLabel2, WM_SETFONT, (WPARAM)hDlgFont, TRUE);
  SendMessage(hEdit2, WM_SETFONT, (WPARAM)hDlgFont, TRUE);
  SendMessage(hOk, WM_SETFONT, (WPARAM)hDlgFont, TRUE);
  SendMessage(hCancel, WM_SETFONT, (WPARAM)hDlgFont, TRUE);

  if (!encrypt) {
    EnableWindow(hLabel2, FALSE);
    EnableWindow(hEdit2, FALSE);
  }

  SetWindowLongPtr(hPass, GWLP_USERDATA, (LONG_PTR)passBuf);
  SetWindowLongPtr(hPass, GWLP_HINSTANCE, (LONG_PTR)bufsize);

  g_passOk = 0;
  g_passEncrypt = encrypt;
  ShowWindow(hPass, SW_SHOW);
  SetFocus(hEdit);
  EnableWindow(g_hWnd, FALSE);

  while (GetMessage(&msg, NULL, 0, 0)) {
    if (!IsWindow(hPass)) break;
    if (IsDialogMessage(hPass, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  EnableWindow(g_hWnd, TRUE);
  SetForegroundWindow(g_hWnd);
  SetFocus(g_hTree); /* restore tree focus on success AND cancel/failure */
  DeleteObject(hDlgFont);
  return g_passOk;
}

static LRESULT CALLBACK PassWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case DM_GETDEFID:
    return MAKELONG(IDC_PASS_OK, DC_HASDEFID);
  case WM_COMMAND:
    if (LOWORD(wParam) == IDC_PASS_OK) {
      char *dst = (char *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
      int bufsize = (int)GetWindowLongPtr(hWnd, GWLP_HINSTANCE);
      GetDlgItemText(hWnd, IDC_PASS_EDIT, dst, bufsize);
      if (g_passEncrypt) {
        char confirm[256] = "";
        GetDlgItemText(hWnd, IDC_PASS_EDIT2, confirm, sizeof(confirm));
        if (strcmp(dst, confirm) != 0) {
          MessageBox(hWnd, "Passwords do not match", "Error", MB_OK | MB_ICONERROR);
          SetFocus(GetDlgItem(hWnd, IDC_PASS_EDIT));
          return 0;
        }
      }
      g_passOk = 1;
      DestroyWindow(hWnd);
      return 0;
    }
    if (LOWORD(wParam) == IDC_PASS_CANCEL || LOWORD(wParam) == IDCANCEL) {
      DestroyWindow(hWnd);
      return 0;
    }
    if (LOWORD(wParam) == IDC_PASS_SHOW && HIWORD(wParam) == BN_CLICKED) {
      HWND hEdit1 = GetDlgItem(hWnd, IDC_PASS_EDIT);
      HWND hEdit2 = GetDlgItem(hWnd, IDC_PASS_EDIT2);
      int show = (int)SendMessage(GetDlgItem(hWnd, IDC_PASS_SHOW), BM_GETCHECK, 0, 0);
      WPARAM ch = (show == BST_CHECKED) ? 0 : '*';
      SendMessage(hEdit1, EM_SETPASSWORDCHAR, ch, 0);
      InvalidateRect(hEdit1, NULL, TRUE);
      SendMessage(hEdit2, EM_SETPASSWORDCHAR, ch, 0);
      InvalidateRect(hEdit2, NULL, TRUE);
      return 0;
    }
    break;
  case WM_KEYDOWN:
    if (wParam == VK_RETURN) {
      SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDC_PASS_OK, BN_CLICKED), 0);
      return 0;
    }
    if (wParam == VK_ESCAPE) {
      SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDC_PASS_CANCEL, BN_CLICKED), 0);
      return 0;
    }
    break;
  case WM_CLOSE:
    DestroyWindow(hWnd);
    return 0;
  }
  return DefWindowProc(hWnd, msg, wParam, lParam);
}

/* --- Find Dialog --- */

static LRESULT CALLBACK FindWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  (void)lParam;
  switch (msg) {
  case WM_CREATE: {
    HFONT hDlgFont = CreateFont(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      DEFAULT_QUALITY, DEFAULT_PITCH, "MS Shell Dlg");
    CreateWindow("STATIC", "Find:", WS_CHILD | WS_VISIBLE,
      10, 12, 30, 20, hWnd, NULL, g_hInst, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_findText,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
      45, 10, 180, 24, hWnd, (HMENU)IDC_FIND_EDIT, g_hInst, NULL);
    CreateWindow("BUTTON", "Next",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      235, 10, 50, 24, hWnd, (HMENU)IDC_FIND_NEXT, g_hInst, NULL);
    CreateWindow("BUTTON", "Prev",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      235, 40, 50, 24, hWnd, (HMENU)IDC_FIND_PREV, g_hInst, NULL);
    CreateWindow("BUTTON", "Close",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      235, 70, 50, 24, hWnd, (HMENU)IDC_FIND_CLOSE, g_hInst, NULL);
    {
      HWND hChild = GetWindow(hWnd, GW_CHILD);
      while (hChild) {
        SendMessage(hChild, WM_SETFONT, (WPARAM)hDlgFont, TRUE);
        hChild = GetWindow(hChild, GW_HWNDNEXT);
      }
    }
    DeleteObject(hDlgFont);
    return 0;
  }
  case WM_COMMAND:
    if (LOWORD(wParam) == IDC_FIND_NEXT || LOWORD(wParam) == IDC_FIND_PREV) {
      FINDTEXTW ft;
      LONG selStart, selEnd;
      LONG pos;
      GetDlgItemTextW(hWnd, IDC_FIND_EDIT, g_findText, sizeof(g_findText)/sizeof(g_findText[0]));
      if (!g_findText[0]) return 0;
      SendMessage(g_hEditor, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
      if (LOWORD(wParam) == IDC_FIND_NEXT) {
        ft.chrg.cpMin = selEnd;
        ft.chrg.cpMax = -1;
      } else {
        ft.chrg.cpMin = selStart;
        ft.chrg.cpMax = 0;
      }
      ft.lpstrText = g_findText;
      if ((pos = (LONG)SendMessageW(g_hEditor, EM_FINDTEXTW,
            LOWORD(wParam) == IDC_FIND_PREV ? FR_DOWN : 0, (LPARAM)&ft)) >= 0)
        SendMessage(g_hEditor, EM_SETSEL, pos, pos + (int)wcslen(g_findText));
      else
        MessageBoxW(hWnd, L"Not found", L"Find", MB_OK);
      return 0;
    }
    if (LOWORD(wParam) == IDC_FIND_CLOSE || LOWORD(wParam) == IDCANCEL) {
      DestroyWindow(hWnd);
      return 0;
    }
    break;
  case WM_CLOSE:
    DestroyWindow(hWnd);
    return 0;
  case WM_DESTROY:
    g_hFindDlg = NULL;
    break;
  }
  return DefWindowProc(hWnd, msg, wParam, lParam);
}
