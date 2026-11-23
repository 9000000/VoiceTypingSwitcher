#include "app.h"
#include <shellapi.h>
#include <wtsapi32.h>

enum : UINT_PTR {
  TIMER_INJECT = 1,
  TIMER_POLL = 2,
  TIMER_RESTORE = 3,
  TIMER_CLOSE_BAR = 4
};
enum : UINT {
  ID_ENABLE = 1,
  ID_AUTOSTART,
  ID_EXIT,
  ID_RESTORE_PREV,
  ID_HOTKEY_WINH,
  ID_HOTKEY_WINC,
  ID_HOTKEY_CUSTOM,
  ID_HOTKEY_ASSIGN,
  ID_UILANG_VI,
  ID_UILANG_EN,
  ID_UPDATE_CHECK,
  ID_ABOUT,
  ID_VOICE_BASE = 100,
  ID_RESTORE_BASE = 200,
  ID_CAP_PREVIEW = 301,
  ID_CAP_HINT = 302,
  ID_ABOUT_LINK = 401,
  ID_ABOUT_UPDATE = 402,
  ID_ABOUT_GITHUB = 403,
};
constexpr DWORD kInjectTickMs = 25; // poll CurrentHkl() before injecting Win+H
constexpr int kInjectMinTicks =
    2; // >= 50ms settle even if layout already matches
constexpr int kInjectMaxTicks = 10; // 250ms cap
constexpr DWORD kPollMs = 350;
constexpr DWORD kCloseDelayMs =
    40; // wait for Enter / mouse button release before sending Esc
constexpr DWORD kCloseRetryMs =
    120; // re-check Voice Typing bar and retry Esc if still visible
constexpr int kCloseMaxWaitTicks = 15; // <= 600ms waiting for button/mod release
constexpr int kCloseMaxTries = 3;      // max Esc attempts if bar stays visible
constexpr DWORD kRestoreDelayMs =
    80; // let Voice Typing finish closing before restoring language
constexpr int kCloseMisses =
    3; // require 3 consecutive misses (~1s) to ignore UIA flickers

static Config s_cfg;
static HANDLE s_mutex = nullptr;
static NOTIFYICONDATAW s_nid{};
static UINT s_taskbarCreated;
static HWND s_aboutHwnd = nullptr;
static bool s_updating = false;
static bool s_active = false; // voice bar was seen during this session
static bool s_injectedOpen = false;
static bool s_dumpedWins = false;
static int s_missCount = 0;
static int s_pollTicks = 0;
static int s_injectTicks = 0;
static int s_closeTries = 0;
static int s_closeWaitTicks = 0;
static RECT s_barRect{};
static HKL s_prevHkl = nullptr;
static HKL s_restoreTarget = nullptr;
static HKL s_lastLoggedHkl = nullptr;
static std::vector<Layout> s_menuLayouts;

static const wchar_t *Tr(const wchar_t *vi, const wchar_t *en) {
  return s_cfg.uiLang == UiEn ? en : vi;
}

static HFONT MakeUiFont(UINT dpi, bool bold, bool underline) {
  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof(ncm);
  if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
    return nullptr;
  UINT sysDpi = GetDpiForSystem();
  if (sysDpi && dpi != sysDpi)
    ncm.lfMessageFont.lfHeight =
        MulDiv(ncm.lfMessageFont.lfHeight, (int)dpi, (int)sysDpi);
  if (bold)
    ncm.lfMessageFont.lfWeight = FW_BOLD;
  if (underline)
    ncm.lfMessageFont.lfUnderline = TRUE;
  return CreateFontIndirectW(&ncm.lfMessageFont);
}

static void OpenUrl(const wchar_t *url) {
  ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------- session state machine ----------------
static void EndSession(const wchar_t *reason, bool closeBar,
                       bool delayRestore = false) {
  if (!g_session)
    return;
  g_session = false;
  KillTimer(g_hwnd, TIMER_INJECT);
  KillTimer(g_hwnd, TIMER_POLL);
  KillTimer(g_hwnd, TIMER_CLOSE_BAR);
  KillTimer(g_hwnd, TIMER_RESTORE);
  RemoveMouseHook();

  s_restoreTarget = s_cfg.restoreMode == RestorePrevious
                        ? s_prevHkl
                        : FindHkl((LANGID)s_cfg.restoreLang);
  if (closeBar && s_injectedOpen) {
    s_closeTries = 0;
    s_closeWaitTicks = 0;
    LogSession(
        L"EndSession(%ls): scheduling close Esc; curHKL=%p -> restoreHKL=%p",
        reason, (void *)CurrentHkl(), (void *)s_restoreTarget);
    SetTimer(g_hwnd, TIMER_CLOSE_BAR, kCloseDelayMs, nullptr);
  } else if (delayRestore) {
    LogSession(L"EndSession(%ls): delayed restore; curHKL=%p -> restoreHKL=%p",
               reason, (void *)CurrentHkl(), (void *)s_restoreTarget);
    SetTimer(g_hwnd, TIMER_RESTORE, kRestoreDelayMs, nullptr);
  } else {
    LogSession(L"EndSession(%ls): curHKL=%p -> restoreHKL=%p", reason,
               (void *)CurrentHkl(), (void *)s_restoreTarget);
    SwitchTo(s_restoreTarget);
  }
}

static void StartSession() {
  ResetSessionLog();
  KillTimer(g_hwnd, TIMER_CLOSE_BAR);
  KillTimer(g_hwnd, TIMER_RESTORE);
  HKL cur = CurrentHkl();
  if (!g_session)
    s_prevHkl =
        cur; // keep original layout if hotkey is pressed again mid-session
  HKL voiceHkl = FindHkl((LANGID)s_cfg.voiceLang);
  LogSession(L"StartSession(%ls): prevHKL=%p curHKL=%p -> voiceHKL=%p (fg=%p)",
             FormatHotkey(s_cfg.hotkeyMods, s_cfg.hotkeyVk).c_str(),
             (void *)s_prevHkl, (void *)cur, (void *)voiceHkl,
             (void *)GetForegroundWindow());
  SwitchTo(voiceHkl);

  g_session = true;
  s_active = false;
  s_injectedOpen = false;
  s_dumpedWins = false;
  s_missCount = 0;
  s_pollTicks = 0;
  s_injectTicks = 0;
  s_barRect = {};
  s_lastLoggedHkl = cur;

  InstallMouseHook();
  SetTimer(g_hwnd, TIMER_INJECT, kInjectTickMs, nullptr);
  SetTimer(g_hwnd, TIMER_POLL, kPollMs, nullptr);
}

static void OnInjectTick() {
  ++s_injectTicks;
  HKL cur = CurrentHkl();
  bool switched = LOWORD((UINT_PTR)cur) == (LANGID)s_cfg.voiceLang;
  if ((switched && s_injectTicks >= kInjectMinTicks) ||
      s_injectTicks >= kInjectMaxTicks) {
    KillTimer(g_hwnd, TIMER_INJECT);
    LogSession(L"InjectWinH after %dms: curHKL=%p switched=%d",
               s_injectTicks * (int)kInjectTickMs, (void *)cur, switched);
    s_lastLoggedHkl = cur;
    s_injectedOpen = true;
    InjectWinH();
  }
}

static bool InputBusy() {
  return ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
           GetAsyncKeyState(VK_MBUTTON) | GetAsyncKeyState(VK_CONTROL) |
           GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_SHIFT) |
           GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) &
          0x8000) != 0;
}

static void OnCloseBarTick() {
  if (s_closeTries == 0) {
    if (InputBusy() && ++s_closeWaitTicks < kCloseMaxWaitTicks)
      return;
  } else if (!QueryVoiceBar().visible) {
    KillTimer(g_hwnd, TIMER_CLOSE_BAR);
    LogSession(L"Voice Typing closed after %d Esc", s_closeTries);
    return;
  } else if (s_closeTries >= kCloseMaxTries) {
    KillTimer(g_hwnd, TIMER_CLOSE_BAR);
    LogSession(L"Voice bar still visible after %d Esc; giving up", s_closeTries);
    return;
  }

  ++s_closeTries;
  LogSession(L"Sending Esc to close Voice Typing (%d/%d)", s_closeTries,
             kCloseMaxTries);
  SendEsc();
  if (s_closeTries == 1)
    SetTimer(g_hwnd, TIMER_RESTORE, kRestoreDelayMs, nullptr);
  SetTimer(g_hwnd, TIMER_CLOSE_BAR, kCloseRetryMs, nullptr);
}

static void Poll() {
  ++s_pollTicks;
  HKL cur = CurrentHkl();
  if (cur != s_lastLoggedHkl) {
    LogSession(L"HKL changed during session: %p -> %p (fg=%p)",
               (void *)s_lastLoggedHkl, (void *)cur,
               (void *)GetForegroundWindow());
    bool leftVoice = s_active && s_injectedOpen && cur &&
                     LOWORD((UINT_PTR)s_lastLoggedHkl) == (LANGID)s_cfg.voiceLang &&
                     LOWORD((UINT_PTR)cur) != (LANGID)s_cfg.voiceLang;
    s_lastLoggedHkl = cur;
    if (leftVoice) {
      EndSession(L"hkl-changed", true);
      return;
    }
  }

  VoiceBarInfo bar = QueryVoiceBar();
  if (bar.visible) {
    if (!s_active) {
      LogSession(L"Voice bar detected: rect=(%ld,%ld,%ld,%ld)", bar.rect.left,
                 bar.rect.top, bar.rect.right, bar.rect.bottom);
    }
    s_active = true;
    s_missCount = 0;
    s_barRect = bar.rect;
  } else if (s_active) {
    if (++s_missCount >= kCloseMisses)
      EndSession(L"bar-closed", true);
  } else if (!s_dumpedWins && s_pollTicks >= 3) {
    s_dumpedWins = true;
    LogSession(L"Voice bar not detected after %d polls; topmost/host snapshot:",
               s_pollTicks);
    LogTopWindowsOnce();
  }
}

// ---------------- tray & dialogs ----------------
static void UpdateTip() {
  std::wstring tip = L"VoiceTypingSwitcher (" +
                     FormatHotkey(s_cfg.hotkeyMods, s_cfg.hotkeyVk) +
                     (g_enabled ? Tr(L") - Đang bật", L") - Enabled")
                                : Tr(L") - Đang tắt", L") - Disabled"));
  lstrcpynW(s_nid.szTip, tip.c_str(), ARRAYSIZE(s_nid.szTip));
  s_nid.uFlags = NIF_TIP;
  Shell_NotifyIconW(NIM_MODIFY, &s_nid);
}

static void ApplyHotkey(DWORD mods, DWORD vk) {
  s_cfg.hotkeyMods = g_hotkeyMods = mods;
  s_cfg.hotkeyVk = g_hotkeyVk = vk;
  SaveConfig(s_cfg);
  UpdateTip();
}

static void BeginUpdateCheck() {
  if (s_updating)
    return;
  s_updating = true;
  if (s_aboutHwnd)
    EnableWindow(GetDlgItem(s_aboutHwnd, ID_ABOUT_UPDATE), FALSE);
  StartUpdateCheckAsync(g_hwnd);
}

// ---- Hotkey capture window ----
static HFONT s_capFont = nullptr;
static DWORD s_capMods = 0, s_capVk = 0;
static bool s_capReady = false;

static const wchar_t *DefaultCaptureHint() {
  return Tr(L"Phím thường phải kèm Win / Ctrl / Alt (hoặc F1–F24). Esc để hủy.",
            L"Regular keys need Win / Ctrl / Alt (or F1–F24). Esc to cancel.");
}

static std::wstring FormatModsPreview(DWORD m) {
  std::wstring s;
  if (m & MOD_CONTROL)
    s += L"Ctrl + ";
  if (m & MOD_ALT)
    s += L"Alt + ";
  if (m & MOD_SHIFT)
    s += L"Shift + ";
  if (m & MOD_WIN)
    s += L"Win + ";
  return s + L"...";
}

static LRESULT CALLBACK CaptureWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_CREATE: {
    UINT dpi = GetDpiForWindow(h);
    auto S = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
    s_capFont = MakeUiFont(dpi, false, false);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    HWND w1 = CreateWindowW(
        L"STATIC",
        Tr(L"Nhấn tổ hợp phím muốn gán (vd: Win+C, Alt+Q, F9):",
           L"Press the shortcut to bind (e.g. Win+C, Alt+Q, F9):"),
        WS_CHILD | WS_VISIBLE, S(14), S(12), S(332), S(20), h, nullptr, inst,
        nullptr);
    std::wstring init = Tr(L"Hiện tại: ", L"Current: ") +
                        FormatHotkey(s_cfg.hotkeyMods, s_cfg.hotkeyVk);
    HWND w2 = CreateWindowW(L"STATIC", init.c_str(),
                            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE |
                                WS_BORDER,
                            S(14), S(38), S(332), S(34), h,
                            (HMENU)(UINT_PTR)ID_CAP_PREVIEW, inst, nullptr);
    HWND w3 = CreateWindowW(L"STATIC", DefaultCaptureHint(),
                            WS_CHILD | WS_VISIBLE, S(14), S(80), S(332), S(20),
                            h, (HMENU)(UINT_PTR)ID_CAP_HINT, inst, nullptr);
    HWND wOk = CreateWindowW(
        L"BUTTON", Tr(L"Lưu", L"Save"),
        WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_DEFPUSHBUTTON, S(182), S(110),
        S(78), S(28), h, (HMENU)(UINT_PTR)IDOK, inst, nullptr);
    HWND wCancel = CreateWindowW(
        L"BUTTON", Tr(L"Hủy", L"Cancel"), WS_CHILD | WS_VISIBLE, S(268), S(110),
        S(78), S(28), h, (HMENU)(UINT_PTR)IDCANCEL, inst, nullptr);
    if (s_capFont) {
      for (HWND c : {w1, w2, w3, wOk, wCancel})
        SendMessageW(c, WM_SETFONT, (WPARAM)s_capFont, TRUE);
    }
    return 0;
  }
  case WM_CTLCOLORSTATIC:
    if ((HWND)lp == GetDlgItem(h, ID_CAP_PREVIEW)) {
      SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
      return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
    return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
  case WM_APP_CAPTURE_PREVIEW:
    if (wp != 0)
      SetDlgItemTextW(h, ID_CAP_PREVIEW, FormatModsPreview((DWORD)wp).c_str());
    else if (s_capReady)
      SetDlgItemTextW(h, ID_CAP_PREVIEW,
                      FormatHotkey(s_capMods, s_capVk).c_str());
    else
      SetDlgItemTextW(h, ID_CAP_PREVIEW,
                      Tr(L"Đang chờ nhấn phím...", L"Waiting for key..."));
    return 0;
  case WM_APP_CAPTURE_KEY: {
    DWORD mods = (DWORD)wp, vk = (DWORD)lp;
    if (IsValidHotkey(mods, vk)) {
      s_capMods = mods;
      s_capVk = vk;
      s_capReady = true;
      SetDlgItemTextW(h, ID_CAP_PREVIEW, FormatHotkey(mods, vk).c_str());
      SetDlgItemTextW(h, ID_CAP_HINT, DefaultCaptureHint());
      EnableWindow(GetDlgItem(h, IDOK), TRUE);
    } else {
      SetDlgItemTextW(
          h, ID_CAP_HINT,
          Tr(L"Không hợp lệ: cần kèm Win / Ctrl / Alt (hoặc F1–F24).",
             L"Invalid: add Win / Ctrl / Alt (or use F1–F24)."));
    }
    return 0;
  }
  case WM_APP_CAPTURE_ACCEPT:
    if (s_capReady) {
      ApplyHotkey(s_capMods, s_capVk);
      DestroyWindow(h);
    }
    return 0;
  case WM_APP_CAPTURE_CANCEL:
    DestroyWindow(h);
    return 0;
  case WM_COMMAND:
    if (LOWORD(wp) == IDOK && s_capReady) {
      ApplyHotkey(s_capMods, s_capVk);
      DestroyWindow(h);
    } else if (LOWORD(wp) == IDCANCEL)
      DestroyWindow(h);
    return 0;
  case WM_DESTROY:
    g_capturing = false;
    g_captureHwnd = nullptr;
    ResetModifierState();
    if (s_capFont) {
      DeleteObject(s_capFont);
      s_capFont = nullptr;
    }
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

static void OpenCaptureDialog() {
  if (g_captureHwnd) {
    SetForegroundWindow(g_captureHwnd);
    return;
  }
  if (g_session)
    EndSession(L"capture", s_active && QueryVoiceBar().visible);

  static bool reg = false;
  HINSTANCE inst = GetModuleHandleW(nullptr);
  if (!reg) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = CaptureWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"VoiceTypingSwitcherCapture";
    RegisterClassW(&wc);
    reg = true;
  }

  s_capMods = s_cfg.hotkeyMods;
  s_capVk = s_cfg.hotkeyVk;
  s_capReady = false;
  ResetModifierState();
  g_capturing = true;

  RECT wa{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  UINT sysDpi = GetDpiForSystem();
  int cw = MulDiv(360, (int)sysDpi, 96), ch = MulDiv(150, (int)sysDpi, 96);
  RECT wr{0, 0, cw, ch};
  AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                     WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
  int w = wr.right - wr.left, h = wr.bottom - wr.top;
  int x = (wa.left + wa.right - w) / 2, y = (wa.top + wa.bottom - h) / 2;

  g_captureHwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"VoiceTypingSwitcherCapture",
      Tr(L"Gán phím tắt - VoiceTypingSwitcher",
         L"Assign Hotkey - VoiceTypingSwitcher"),
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, nullptr,
      nullptr, inst, nullptr);
  if (!g_captureHwnd) {
    g_capturing = false;
    return;
  }
  SetForegroundWindow(g_captureHwnd);
}

// ---- About & GitHub window ----
static HFONT s_abFont = nullptr, s_abBold = nullptr, s_abLink = nullptr;
static HICON s_abIcon = nullptr;

static LRESULT CALLBACK AboutWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_CREATE: {
    UINT dpi = GetDpiForWindow(h);
    auto S = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
    s_abFont = MakeUiFont(dpi, false, false);
    s_abBold = MakeUiFont(dpi, true, false);
    s_abLink = MakeUiFont(dpi, false, true);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    s_abIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(48),
                                 S(48), 0);
    HWND wIco =
        CreateWindowW(L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ICON,
                      S(16), S(16), S(48), S(48), h, nullptr, inst, nullptr);
    SendMessageW(wIco, STM_SETICON, (WPARAM)s_abIcon, 0);

    HWND wTitle =
        CreateWindowW(L"STATIC", L"VoiceTypingSwitcher  v" APP_VER_WSTR,
                      WS_CHILD | WS_VISIBLE, S(76), S(16), S(300), S(22), h,
                      nullptr, inst, nullptr);
    HWND wDesc =
        CreateWindowW(L"STATIC",
                      Tr(L"Tự đổi bàn phím khi mở Voice Typing trên Windows 11 "
                         L"và tắt Voice Typing khi thoát.",
                         L"Automatically switches keyboard layout for Windows "
                         L"11 Voice Typing and closes it on exit."),
                      WS_CHILD | WS_VISIBLE, S(76), S(42), S(306), S(36), h,
                      nullptr, inst, nullptr);
    HWND wLink = CreateWindowW(
        L"STATIC", APP_GITHUB_URL, WS_CHILD | WS_VISIBLE | SS_NOTIFY, S(16),
        S(90), S(366), S(20), h, (HMENU)(UINT_PTR)ID_ABOUT_LINK, inst, nullptr);
    HWND wUpd = CreateWindowW(
        L"BUTTON", Tr(L"Kiểm tra cập nhật", L"Check for updates"),
        WS_CHILD | WS_VISIBLE | (s_updating ? WS_DISABLED : 0), S(16), S(124),
        S(146), S(28), h, (HMENU)(UINT_PTR)ID_ABOUT_UPDATE, inst, nullptr);
    HWND wGh =
        CreateWindowW(L"BUTTON", Tr(L"Mở GitHub", L"Open GitHub"),
                      WS_CHILD | WS_VISIBLE, S(170), S(124), S(112), S(28), h,
                      (HMENU)(UINT_PTR)ID_ABOUT_GITHUB, inst, nullptr);
    HWND wClose = CreateWindowW(L"BUTTON", Tr(L"Đóng", L"Close"),
                                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                S(290), S(124), S(92), S(28), h,
                                (HMENU)(UINT_PTR)IDCANCEL, inst, nullptr);

    if (s_abFont) {
      for (HWND c : {wDesc, wUpd, wGh, wClose})
        SendMessageW(c, WM_SETFONT, (WPARAM)s_abFont, TRUE);
    }
    if (s_abBold)
      SendMessageW(wTitle, WM_SETFONT, (WPARAM)s_abBold, TRUE);
    if (s_abLink)
      SendMessageW(wLink, WM_SETFONT, (WPARAM)s_abLink, TRUE);
    return 0;
  }
  case WM_SETCURSOR:
    if ((HWND)wp == GetDlgItem(h, ID_ABOUT_LINK)) {
      SetCursor(LoadCursorW(nullptr, IDC_HAND));
      return TRUE;
    }
    break;
  case WM_CTLCOLORSTATIC:
    SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
    if ((HWND)lp == GetDlgItem(h, ID_ABOUT_LINK))
      SetTextColor((HDC)wp, RGB(0, 102, 204));
    return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
  case WM_COMMAND:
    if (LOWORD(wp) == ID_ABOUT_LINK || LOWORD(wp) == ID_ABOUT_GITHUB) {
      OpenUrl(APP_GITHUB_URL);
    } else if (LOWORD(wp) == ID_ABOUT_UPDATE) {
      BeginUpdateCheck();
    } else if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK) {
      DestroyWindow(h);
    }
    return 0;
  case WM_KEYDOWN:
    if (wp == VK_ESCAPE) {
      DestroyWindow(h);
      return 0;
    }
    break;
  case WM_DESTROY:
    s_aboutHwnd = nullptr;
    if (s_abFont) {
      DeleteObject(s_abFont);
      s_abFont = nullptr;
    }
    if (s_abBold) {
      DeleteObject(s_abBold);
      s_abBold = nullptr;
    }
    if (s_abLink) {
      DeleteObject(s_abLink);
      s_abLink = nullptr;
    }
    if (s_abIcon) {
      DestroyIcon(s_abIcon);
      s_abIcon = nullptr;
    }
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

static void OpenAboutDialog() {
  if (s_aboutHwnd) {
    SetForegroundWindow(s_aboutHwnd);
    return;
  }

  static bool reg = false;
  HINSTANCE inst = GetModuleHandleW(nullptr);
  if (!reg) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = AboutWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"VoiceTypingSwitcherAbout";
    RegisterClassW(&wc);
    reg = true;
  }

  RECT wa{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  UINT sysDpi = GetDpiForSystem();
  int cw = MulDiv(398, (int)sysDpi, 96), ch = MulDiv(166, (int)sysDpi, 96);
  RECT wr{0, 0, cw, ch};
  AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                     WS_EX_TOOLWINDOW);
  int w = wr.right - wr.left, h = wr.bottom - wr.top;
  int x = (wa.left + wa.right - w) / 2, y = (wa.top + wa.bottom - h) / 2;

  s_aboutHwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW, L"VoiceTypingSwitcherAbout",
      Tr(L"Thông tin - VoiceTypingSwitcher", L"About - VoiceTypingSwitcher"),
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, nullptr,
      nullptr, inst, nullptr);
  if (s_aboutHwnd)
    SetForegroundWindow(s_aboutHwnd);
}

// ---------------- tray menu ----------------
static void AddTrayIcon() {
  s_nid.cbSize = sizeof(s_nid);
  s_nid.hWnd = g_hwnd;
  s_nid.uID = 1;
  s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  s_nid.uCallbackMessage = WM_APP_TRAY;
  s_nid.hIcon = (HICON)LoadImageW(
      GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
  Shell_NotifyIconW(NIM_ADD, &s_nid);
  UpdateTip();
}

static void ShowMenu() {
  s_menuLayouts = ListLayouts();
  HMENU hotkey = CreatePopupMenu(), voice = CreatePopupMenu(),
        restore = CreatePopupMenu();
  HMENU uiLang = CreatePopupMenu(), menu = CreatePopupMenu();

  bool isWinH = s_cfg.hotkeyMods == MOD_WIN && s_cfg.hotkeyVk == 'H';
  bool isWinC = s_cfg.hotkeyMods == MOD_WIN && s_cfg.hotkeyVk == 'C';
  AppendMenuW(hotkey, MF_STRING | (isWinH ? MF_CHECKED : 0), ID_HOTKEY_WINH,
              Tr(L"Win + H (Mặc định)", L"Win + H (Default)"));
  AppendMenuW(hotkey, MF_STRING | (isWinC ? MF_CHECKED : 0), ID_HOTKEY_WINC,
              L"Win + C");
  if (!isWinH && !isWinC) {
    std::wstring custom = FormatHotkey(s_cfg.hotkeyMods, s_cfg.hotkeyVk) +
                          Tr(L" (Tùy chỉnh)", L" (Custom)");
    AppendMenuW(hotkey, MF_STRING | MF_CHECKED, ID_HOTKEY_CUSTOM,
                custom.c_str());
  }
  AppendMenuW(hotkey, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(hotkey, MF_STRING, ID_HOTKEY_ASSIGN,
              Tr(L"Gán phím khác...", L"Assign custom key..."));

  for (size_t i = 0; i < s_menuLayouts.size() && i < 100; ++i) {
    const Layout &l = s_menuLayouts[i];
    AppendMenuW(voice, MF_STRING | (l.lang == s_cfg.voiceLang ? MF_CHECKED : 0),
                ID_VOICE_BASE + i, l.name.c_str());
    bool fixedSel =
        s_cfg.restoreMode == RestoreFixed && l.lang == s_cfg.restoreLang;
    AppendMenuW(restore, MF_STRING | (fixedSel ? MF_CHECKED : 0),
                ID_RESTORE_BASE + i, l.name.c_str());
  }
  AppendMenuW(restore, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(restore,
              MF_STRING |
                  (s_cfg.restoreMode == RestorePrevious ? MF_CHECKED : 0),
              ID_RESTORE_PREV,
              Tr(L"Ngôn ngữ trước khi kích hoạt", L"Previous language"));

  AppendMenuW(uiLang, MF_STRING | (s_cfg.uiLang == UiVi ? MF_CHECKED : 0),
              ID_UILANG_VI, L"Tiếng Việt");
  AppendMenuW(uiLang, MF_STRING | (s_cfg.uiLang == UiEn ? MF_CHECKED : 0),
              ID_UILANG_EN, L"English");

  std::wstring hkLabel = std::wstring(Tr(L"Phím tắt (", L"Hotkey (")) +
                         FormatHotkey(s_cfg.hotkeyMods, s_cfg.hotkeyVk) + L")";
  std::wstring updLabel =
      s_updating ? Tr(L"Đang kiểm tra cập nhật...", L"Checking for updates...")
                 : Tr(L"Kiểm tra cập nhật (v" APP_VER_WSTR L")...",
                      L"Check for updates (v" APP_VER_WSTR L")...");

  AppendMenuW(menu, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_ENABLE,
              Tr(L"Bật", L"Enabled"));
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)hotkey, hkLabel.c_str());
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)voice,
              Tr(L"Ngôn ngữ Voice Typing", L"Voice typing language"));
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)restore,
              Tr(L"Ngôn ngữ khi thoát", L"Restore language"));
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)uiLang,
              Tr(L"Ngôn ngữ hiển thị", L"Display language"));
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (GetAutostart() ? MF_CHECKED : 0), ID_AUTOSTART,
              Tr(L"Khởi động cùng Windows", L"Run at startup"));
  AppendMenuW(menu, MF_STRING | (s_updating ? MF_GRAYED : 0), ID_UPDATE_CHECK,
              updLabel.c_str());
  AppendMenuW(menu, MF_STRING, ID_ABOUT,
              Tr(L"Thông tin && GitHub...", L"About && GitHub..."));
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, ID_EXIT, Tr(L"Thoát", L"Exit"));

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(g_hwnd);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
  PostMessageW(g_hwnd, WM_NULL, 0, 0);
  DestroyMenu(menu); // also destroys submenus
}

static void OnCommand(UINT id) {
  if (id == ID_ENABLE) {
    g_enabled = !g_enabled;
    s_cfg.enabled = g_enabled;
    if (!g_enabled)
      EndSession(L"disabled", s_active && QueryVoiceBar().visible);
    UpdateTip();
  } else if (id == ID_HOTKEY_WINH) {
    ApplyHotkey(MOD_WIN, 'H');
    return;
  } else if (id == ID_HOTKEY_WINC) {
    ApplyHotkey(MOD_WIN, 'C');
    return;
  } else if (id == ID_HOTKEY_ASSIGN) {
    OpenCaptureDialog();
    return;
  } else if (id == ID_UILANG_VI || id == ID_UILANG_EN) {
    s_cfg.uiLang = (id == ID_UILANG_EN) ? UiEn : UiVi;
    SaveConfig(s_cfg);
    UpdateTip();
    if (s_aboutHwnd) {
      DestroyWindow(s_aboutHwnd);
      OpenAboutDialog();
    }
    return;
  } else if (id == ID_UPDATE_CHECK) {
    BeginUpdateCheck();
    return;
  } else if (id == ID_ABOUT) {
    OpenAboutDialog();
    return;
  } else if (id == ID_AUTOSTART) {
    SetAutostart(!GetAutostart());
    return;
  } else if (id == ID_EXIT) {
    DestroyWindow(g_hwnd);
    return;
  } else if (id == ID_RESTORE_PREV) {
    s_cfg.restoreMode = RestorePrevious;
  } else if (id >= ID_VOICE_BASE && id < ID_VOICE_BASE + s_menuLayouts.size()) {
    s_cfg.voiceLang = s_menuLayouts[id - ID_VOICE_BASE].lang;
  } else if (id >= ID_RESTORE_BASE &&
             id < ID_RESTORE_BASE + s_menuLayouts.size()) {
    s_cfg.restoreLang = s_menuLayouts[id - ID_RESTORE_BASE].lang;
    s_cfg.restoreMode = RestoreFixed;
  } else {
    return;
  }
  SaveConfig(s_cfg);
}

// ---------------- update callbacks ----------------
static void OnUpdateCheckDone(UpdateInfo *info) {
  UpdateInfo copy = *info;
  delete info;
  HWND owner = s_aboutHwnd;
  if (s_aboutHwnd)
    EnableWindow(GetDlgItem(s_aboutHwnd, ID_ABOUT_UPDATE), TRUE);

  if (copy.status == UpdUpToDate) {
    s_updating = false;
    std::wstring msg =
        std::wstring(Tr(L"Bạn đang dùng phiên bản mới nhất (v",
                        L"You are using the latest version (v")) +
        APP_VER_WSTR L").";
    MessageBoxW(owner, msg.c_str(), L"VoiceTypingSwitcher", MB_ICONINFORMATION);
  } else if (copy.status == UpdNoReleases) {
    s_updating = false;
    int r =
        MessageBoxW(owner,
                    Tr(L"Chưa có bản phát hành (Release) nào trên GitHub.\nBạn "
                       L"có muốn mở trang GitHub Releases không?",
                       L"No GitHub Releases published yet.\nOpen the GitHub "
                       L"Releases page?"),
                    L"VoiceTypingSwitcher", MB_YESNO | MB_ICONINFORMATION);
    if (r == IDYES)
      OpenUrl(copy.htmlUrl.c_str());
  } else if (copy.status == UpdError) {
    s_updating = false;
    std::wstring msg = std::wstring(Tr(L"Không kiểm tra được cập nhật: ",
                                       L"Failed to check for updates: ")) +
                       copy.error;
    MessageBoxW(owner, msg.c_str(), L"VoiceTypingSwitcher", MB_ICONERROR);
  } else if (copy.status == UpdAvailable) {
    if (copy.exeUrl.empty()) {
      s_updating = false;
      std::wstring msg =
          std::wstring(Tr(L"Có phiên bản mới ", L"New version available: ")) +
          copy.tag +
          Tr(L" (hiện tại "
             L"v" APP_VER_WSTR L").\nBản phát hành chưa đính kèm file .exe. Mở "
                               L"trang GitHub?",
             L" (current "
             L"v" APP_VER_WSTR L").\nNo .exe asset attached. Open GitHub "
                               L"Release page?");
      if (MessageBoxW(owner, msg.c_str(), L"VoiceTypingSwitcher",
                      MB_YESNO | MB_ICONINFORMATION) == IDYES)
        OpenUrl(copy.htmlUrl.c_str());
      return;
    }
    std::wstring msg =
        std::wstring(Tr(L"Đã có phiên bản mới ", L"New version available: ")) +
        copy.tag +
        Tr(L" (hiện tại "
           L"v" APP_VER_WSTR L").\nBạn có muốn tải và cập nhật ngay không?",
           L" (current v" APP_VER_WSTR L").\nDownload and update now?");
    if (MessageBoxW(owner, msg.c_str(), L"VoiceTypingSwitcher",
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
      if (s_aboutHwnd)
        EnableWindow(GetDlgItem(s_aboutHwnd, ID_ABOUT_UPDATE), FALSE);
      StartUpdateDownloadAsync(g_hwnd, copy.exeUrl);
    } else {
      s_updating = false;
    }
  }
}

static void OnUpdateDownloadDone(DownloadResult *res) {
  DownloadResult copy = *res;
  delete res;
  s_updating = false;
  HWND owner = s_aboutHwnd;
  if (s_aboutHwnd)
    EnableWindow(GetDlgItem(s_aboutHwnd, ID_ABOUT_UPDATE), TRUE);

  if (!copy.ok) {
    std::wstring msg = std::wstring(Tr(L"Tải bản cập nhật thất bại: ",
                                       L"Update download failed: ")) +
                       copy.error +
                       Tr(L"\nMở trang GitHub Releases để tải thủ công?",
                          L"\nOpen GitHub Releases to download manually?");
    if (MessageBoxW(owner, msg.c_str(), L"VoiceTypingSwitcher",
                    MB_YESNO | MB_ICONERROR) == IDYES)
      OpenUrl(APP_GITHUB_URL L"/releases");
    return;
  }

  std::wstring newExe;
  if (!ReplaceSelfAndPrepareRelaunch(copy.tempExePath, newExe)) {
    DeleteFileW(copy.tempExePath.c_str());
    std::wstring msg =
        Tr(L"Không thể ghi đè file .exe (thử chạy bằng quyền Administrator "
           L"hoặc tải thủ công từ GitHub).\nMở trang GitHub Releases?",
           L"Cannot replace the executable (try running as Administrator or "
           L"download manually).\nOpen GitHub Releases?");
    if (MessageBoxW(owner, msg.c_str(), L"VoiceTypingSwitcher",
                    MB_YESNO | MB_ICONERROR) == IDYES)
      OpenUrl(APP_GITHUB_URL L"/releases");
    return;
  }

  // Release hooks, tray icon, and mutex before starting the updated binary.
  if (g_captureHwnd)
    DestroyWindow(g_captureHwnd);
  if (s_aboutHwnd)
    DestroyWindow(s_aboutHwnd);
  RemoveMouseHook();
  RemoveKeyboardHook();
  Shell_NotifyIconW(NIM_DELETE, &s_nid);
  if (s_mutex) {
    ReleaseMutex(s_mutex);
    CloseHandle(s_mutex);
    s_mutex = nullptr;
  }

  wchar_t cmd[MAX_PATH + 48];
  wsprintfW(cmd, L"\"%ls\" --updated %lu", newExe.c_str(),
            GetCurrentProcessId());
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(newExe.c_str(), cmd, nullptr, nullptr, FALSE, 0, nullptr,
                     nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  DestroyWindow(g_hwnd);
}

// ---------------- window ----------------
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_APP_START:
    StartSession();
    return 0;
  case WM_APP_END:
    EndSession(wp == VK_ESCAPE ? L"key-esc" : L"key-enter", wp != VK_ESCAPE,
               wp == VK_ESCAPE);
    return 0;
  case WM_APP_CLICK: {
    if (!g_session)
      return 0;
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    if (IsPointOnVoiceBar(pt, s_active ? &s_barRect : nullptr)) {
      LogSession(L"Click inside voice bar at (%ld,%ld) - kept", pt.x, pt.y);
    } else {
      LogSession(L"Click outside voice bar at (%ld,%ld)", pt.x, pt.y);
      EndSession(L"click-outside", true);
    }
    return 0;
  }
  case WM_TIMER:
    if (wp == TIMER_INJECT)
      OnInjectTick();
    else if (wp == TIMER_POLL)
      Poll();
    else if (wp == TIMER_CLOSE_BAR)
      OnCloseBarTick(); else if (wp == TIMER_RESTORE) {
      KillTimer(h, TIMER_RESTORE);
      LogSession(L"Restoring HKL=%p (cur=%p)", (void *)s_restoreTarget,
                 (void *)CurrentHkl());
      SwitchTo(s_restoreTarget);
    }
    return 0;
  case WM_APP_TRAY:
    if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP)
      ShowMenu();
    return 0;
  case WM_APP_UPDATE_CHECK:
    OnUpdateCheckDone((UpdateInfo *)lp);
    return 0;
  case WM_APP_UPDATE_DL:
    OnUpdateDownloadDone((DownloadResult *)lp);
    return 0;
  case WM_COMMAND:
    OnCommand(LOWORD(wp));
    return 0;
  case WM_WTSSESSION_CHANGE:
    if (wp == WTS_SESSION_UNLOCK || wp == WTS_SESSION_LOCK)
      ResetModifierState();
    return 0;
  case WM_DESTROY:
    if (g_captureHwnd)
      DestroyWindow(g_captureHwnd);
    if (s_aboutHwnd)
      DestroyWindow(s_aboutHwnd);
    WTSUnRegisterSessionNotification(h);
    RemoveMouseHook();
    RemoveKeyboardHook();
    CleanupVoiceBar();
    Shell_NotifyIconW(NIM_DELETE, &s_nid);
    PostQuitMessage(0);
    return 0;
  }
  if (msg == s_taskbarCreated) {
    AddTrayIcon();
    return 0;
  } // Explorer restarted
  return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
  if (wcsstr(cmd, L"--probe"))
    return RunProbe();
  CleanupOldBinary(cmd);

  s_mutex = CreateMutexW(nullptr, TRUE, L"Local\\VoiceTypingSwitcher");
  if (GetLastError() == ERROR_ALREADY_EXISTS)
    return 0;
  SetProcessDpiAwarenessContext(
      DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // crisp tray icon/menu

  s_cfg = LoadConfig();
  g_enabled = s_cfg.enabled != 0;
  g_hotkeyMods = s_cfg.hotkeyMods;
  g_hotkeyVk = s_cfg.hotkeyVk;

  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = L"VoiceTypingSwitcherWnd";
  RegisterClassW(&wc);
  g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName,
                           L"VoiceTypingSwitcher", WS_POPUP, 0, 0, 0, 0,
                           nullptr, nullptr, inst, nullptr);
  s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  WTSRegisterSessionNotification(g_hwnd, NOTIFY_FOR_THIS_SESSION);
  InitVoiceBar();

  if (!InstallKeyboardHook()) {
    MessageBoxW(nullptr,
                Tr(L"Không cài được keyboard hook.",
                   L"Failed to install keyboard hook."),
                L"VoiceTypingSwitcher", MB_ICONERROR);
    CleanupVoiceBar();
    return 1;
  }
  AddTrayIcon();

  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  if (s_mutex)
    CloseHandle(s_mutex);
  return 0;
}
