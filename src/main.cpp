#include "app.h"
#include <shellapi.h>
#include <wtsapi32.h>

enum : UINT_PTR { TIMER_INJECT = 1, TIMER_POLL = 2, TIMER_RESTORE = 3, TIMER_CLOSE_BAR = 4 };
enum : UINT {
    ID_ENABLE = 1, ID_AUTOSTART, ID_EXIT, ID_RESTORE_PREV,
    ID_HOTKEY_WINH, ID_HOTKEY_WINC, ID_HOTKEY_CUSTOM, ID_HOTKEY_ASSIGN,
    ID_VOICE_BASE = 100, ID_RESTORE_BASE = 200,
    ID_CAP_PREVIEW = 301, ID_CAP_HINT = 302,
};
constexpr DWORD kInjectTickMs = 25;    // poll CurrentHkl() before injecting Win+H
constexpr int kInjectMinTicks = 2;     // >= 50ms settle even if layout already matches
constexpr int kInjectMaxTicks = 10;    // 250ms cap
constexpr DWORD kPollMs = 350;
constexpr DWORD kCloseDelayMs = 40;    // let Enter / mouse click settle before sending Win+H
constexpr DWORD kRestoreDelayMs = 80;  // let Voice Typing finish closing before restoring language
constexpr int kCloseMisses = 3;        // require 3 consecutive misses (~1s) to ignore UIA flickers

static Config s_cfg;
static NOTIFYICONDATAW s_nid{};
static UINT s_taskbarCreated;
static bool s_active = false;  // voice bar was seen during this session
static bool s_injectedOpen = false;
static bool s_dumpedWins = false;
static int s_missCount = 0;
static int s_pollTicks = 0;
static int s_injectTicks = 0;
static RECT s_barRect{};
static HKL s_prevHkl = nullptr;
static HKL s_restoreTarget = nullptr;
static HKL s_lastLoggedHkl = nullptr;
static std::vector<Layout> s_menuLayouts;

// ---------------- session state machine ----------------
static void EndSession(const wchar_t* reason, bool closeBar, bool delayRestore = false) {
    if (!g_session) return;
    g_session = false;
    KillTimer(g_hwnd, TIMER_INJECT);
    KillTimer(g_hwnd, TIMER_POLL);
    KillTimer(g_hwnd, TIMER_CLOSE_BAR);
    KillTimer(g_hwnd, TIMER_RESTORE);
    RemoveMouseHook();

    s_restoreTarget = s_cfg.restoreMode == RestorePrevious ? s_prevHkl : FindHkl((LANGID)s_cfg.restoreLang);
    if (closeBar && s_injectedOpen) {
        LogSession(L"EndSession(%ls): scheduling close Win+H; curHKL=%p -> restoreHKL=%p",
                   reason, (void*)CurrentHkl(), (void*)s_restoreTarget);
        SetTimer(g_hwnd, TIMER_CLOSE_BAR, kCloseDelayMs, nullptr);
    } else if (delayRestore) {
        LogSession(L"EndSession(%ls): delayed restore; curHKL=%p -> restoreHKL=%p",
                   reason, (void*)CurrentHkl(), (void*)s_restoreTarget);
        SetTimer(g_hwnd, TIMER_RESTORE, kRestoreDelayMs, nullptr);
    } else {
        LogSession(L"EndSession(%ls): curHKL=%p -> restoreHKL=%p", reason, (void*)CurrentHkl(), (void*)s_restoreTarget);
        SwitchTo(s_restoreTarget);
    }
}

static void StartSession() {
    ResetSessionLog();
    KillTimer(g_hwnd, TIMER_CLOSE_BAR);
    KillTimer(g_hwnd, TIMER_RESTORE);
    HKL cur = CurrentHkl();
    if (!g_session) s_prevHkl = cur;  // keep original layout if hotkey is pressed again mid-session
    HKL voiceHkl = FindHkl((LANGID)s_cfg.voiceLang);
    LogSession(L"StartSession(%ls): prevHKL=%p curHKL=%p -> voiceHKL=%p (fg=%p)",
               FormatHotkey(s_cfg.hotkeyMods, s_cfg.hotkeyVk).c_str(),
               (void*)s_prevHkl, (void*)cur, (void*)voiceHkl, (void*)GetForegroundWindow());
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
    if ((switched && s_injectTicks >= kInjectMinTicks) || s_injectTicks >= kInjectMaxTicks) {
        KillTimer(g_hwnd, TIMER_INJECT);
        LogSession(L"InjectWinH after %dms: curHKL=%p switched=%d",
                   s_injectTicks * (int)kInjectTickMs, (void*)cur, switched);
        s_lastLoggedHkl = cur;
        s_injectedOpen = true;
        InjectWinH();
    }
}

static void Poll() {
    ++s_pollTicks;
    HKL cur = CurrentHkl();
    if (cur != s_lastLoggedHkl) {
        LogSession(L"HKL changed during session: %p -> %p (fg=%p)",
                   (void*)s_lastLoggedHkl, (void*)cur, (void*)GetForegroundWindow());
        s_lastLoggedHkl = cur;
    }

    VoiceBarInfo bar = QueryVoiceBar();
    if (bar.visible) {
        if (!s_active) {
            LogSession(L"Voice bar detected: rect=(%ld,%ld,%ld,%ld)",
                       bar.rect.left, bar.rect.top, bar.rect.right, bar.rect.bottom);
        }
        s_active = true;
        s_missCount = 0;
        s_barRect = bar.rect;
    } else if (s_active) {
        if (++s_missCount >= kCloseMisses) EndSession(L"bar-closed", false);
    } else if (!s_dumpedWins && s_pollTicks >= 3) {
        s_dumpedWins = true;
        LogSession(L"Voice bar not detected after %d polls; topmost/host snapshot:", s_pollTicks);
        LogTopWindowsOnce();
    }
}

// ---------------- tray & hotkey capture dialog ----------------
static void UpdateTip() {
    std::wstring tip = L"VoiceTypingSwitcher (" + FormatHotkey(s_cfg.hotkeyMods, s_cfg.hotkeyVk) +
                       (g_enabled ? L") - Đang bật" : L") - Đang tắt");
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

static HFONT s_capFont = nullptr;
static DWORD s_capMods = 0, s_capVk = 0;
static bool s_capReady = false;
static const wchar_t* kDefaultHint = L"Phím thường phải kèm Win / Ctrl / Alt (hoặc F1–F24). Esc để hủy.";

static std::wstring FormatModsPreview(DWORD m) {
    std::wstring s;
    if (m & MOD_CONTROL) s += L"Ctrl + ";
    if (m & MOD_ALT) s += L"Alt + ";
    if (m & MOD_SHIFT) s += L"Shift + ";
    if (m & MOD_WIN) s += L"Win + ";
    return s + L"...";
}

static LRESULT CALLBACK CaptureWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        UINT dpi = GetDpiForWindow(h);
        auto S = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            UINT sysDpi = GetDpiForSystem();
            if (sysDpi && dpi != sysDpi) ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, (int)dpi, (int)sysDpi);
            s_capFont = CreateFontIndirectW(&ncm.lfMessageFont);
        }

        HINSTANCE inst = GetModuleHandleW(nullptr);
        HWND w1 = CreateWindowW(L"STATIC", L"Nhấn tổ hợp phím muốn gán (vd: Win+C, Alt+Q, F9):",
                                WS_CHILD | WS_VISIBLE, S(14), S(12), S(330), S(20), h, nullptr, inst, nullptr);
        std::wstring init = L"Hiện tại: " + FormatHotkey(s_cfg.hotkeyMods, s_cfg.hotkeyVk);
        HWND w2 = CreateWindowW(L"STATIC", init.c_str(),
                                WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | WS_BORDER,
                                S(14), S(38), S(330), S(34), h, (HMENU)(UINT_PTR)ID_CAP_PREVIEW, inst, nullptr);
        HWND w3 = CreateWindowW(L"STATIC", kDefaultHint,
                                WS_CHILD | WS_VISIBLE, S(14), S(80), S(330), S(20), h,
                                (HMENU)(UINT_PTR)ID_CAP_HINT, inst, nullptr);
        HWND wOk = CreateWindowW(L"BUTTON", L"Lưu", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_DEFPUSHBUTTON,
                                 S(180), S(110), S(78), S(28), h, (HMENU)(UINT_PTR)IDOK, inst, nullptr);
        HWND wCancel = CreateWindowW(L"BUTTON", L"Hủy", WS_CHILD | WS_VISIBLE,
                                     S(266), S(110), S(78), S(28), h, (HMENU)(UINT_PTR)IDCANCEL, inst, nullptr);
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
        if (wp != 0) SetDlgItemTextW(h, ID_CAP_PREVIEW, FormatModsPreview((DWORD)wp).c_str());
        else if (s_capReady) SetDlgItemTextW(h, ID_CAP_PREVIEW, FormatHotkey(s_capMods, s_capVk).c_str());
        else SetDlgItemTextW(h, ID_CAP_PREVIEW, L"Đang chờ nhấn phím...");
        return 0;
    case WM_APP_CAPTURE_KEY: {
        DWORD mods = (DWORD)wp, vk = (DWORD)lp;
        if (IsValidHotkey(mods, vk)) {
            s_capMods = mods;
            s_capVk = vk;
            s_capReady = true;
            SetDlgItemTextW(h, ID_CAP_PREVIEW, FormatHotkey(mods, vk).c_str());
            SetDlgItemTextW(h, ID_CAP_HINT, kDefaultHint);
            EnableWindow(GetDlgItem(h, IDOK), TRUE);
        } else {
            SetDlgItemTextW(h, ID_CAP_HINT, L"Không hợp lệ: cần kèm Win / Ctrl / Alt (hoặc F1–F24).");
        }
        return 0;
    }
    case WM_APP_CAPTURE_ACCEPT:
        if (s_capReady) { ApplyHotkey(s_capMods, s_capVk); DestroyWindow(h); }
        return 0;
    case WM_APP_CAPTURE_CANCEL:
        DestroyWindow(h);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK && s_capReady) { ApplyHotkey(s_capMods, s_capVk); DestroyWindow(h); }
        else if (LOWORD(wp) == IDCANCEL) DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_capturing = false;
        g_captureHwnd = nullptr;
        ResetModifierState();
        if (s_capFont) { DeleteObject(s_capFont); s_capFont = nullptr; }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void OpenCaptureDialog() {
    if (g_captureHwnd) { SetForegroundWindow(g_captureHwnd); return; }
    if (g_session) EndSession(L"capture", s_active && QueryVoiceBar().visible);

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
    AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    int x = (wa.left + wa.right - w) / 2, y = (wa.top + wa.bottom - h) / 2;

    g_captureHwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"VoiceTypingSwitcherCapture",
                                    L"Gán phím tắt - VoiceTypingSwitcher",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                    x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_captureHwnd) { g_capturing = false; return; }
    SetForegroundWindow(g_captureHwnd);
}

static void AddTrayIcon() {
    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = g_hwnd;
    s_nid.uID = 1;
    s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    s_nid.uCallbackMessage = WM_APP_TRAY;
    s_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    Shell_NotifyIconW(NIM_ADD, &s_nid);
    UpdateTip();
}

static void ShowMenu() {
    s_menuLayouts = ListLayouts();
    HMENU hotkey = CreatePopupMenu(), voice = CreatePopupMenu(), restore = CreatePopupMenu(), menu = CreatePopupMenu();

    bool isWinH = s_cfg.hotkeyMods == MOD_WIN && s_cfg.hotkeyVk == 'H';
    bool isWinC = s_cfg.hotkeyMods == MOD_WIN && s_cfg.hotkeyVk == 'C';
    AppendMenuW(hotkey, MF_STRING | (isWinH ? MF_CHECKED : 0), ID_HOTKEY_WINH, L"Win + H (Mặc định)");
    AppendMenuW(hotkey, MF_STRING | (isWinC ? MF_CHECKED : 0), ID_HOTKEY_WINC, L"Win + C");
    if (!isWinH && !isWinC) {
        std::wstring custom = FormatHotkey(s_cfg.hotkeyMods, s_cfg.hotkeyVk) + L" (Tùy chỉnh)";
        AppendMenuW(hotkey, MF_STRING | MF_CHECKED, ID_HOTKEY_CUSTOM, custom.c_str());
    }
    AppendMenuW(hotkey, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hotkey, MF_STRING, ID_HOTKEY_ASSIGN, L"Gán phím khác...");

    for (size_t i = 0; i < s_menuLayouts.size() && i < 100; ++i) {
        const Layout& l = s_menuLayouts[i];
        AppendMenuW(voice, MF_STRING | (l.lang == s_cfg.voiceLang ? MF_CHECKED : 0), ID_VOICE_BASE + i, l.name.c_str());
        bool fixedSel = s_cfg.restoreMode == RestoreFixed && l.lang == s_cfg.restoreLang;
        AppendMenuW(restore, MF_STRING | (fixedSel ? MF_CHECKED : 0), ID_RESTORE_BASE + i, l.name.c_str());
    }
    AppendMenuW(restore, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(restore, MF_STRING | (s_cfg.restoreMode == RestorePrevious ? MF_CHECKED : 0), ID_RESTORE_PREV,
                L"Ngôn ngữ trước khi kích hoạt");

    std::wstring hkLabel = L"Phím tắt (" + FormatHotkey(s_cfg.hotkeyMods, s_cfg.hotkeyVk) + L")";
    AppendMenuW(menu, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_ENABLE, L"Bật");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)hotkey, hkLabel.c_str());
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)voice, L"Ngôn ngữ khi Voice Typing");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)restore, L"Ngôn ngữ khi thoát");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (GetAutostart() ? MF_CHECKED : 0), ID_AUTOSTART, L"Khởi động cùng Windows");
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Thoát");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys submenus
}

static void OnCommand(UINT id) {
    if (id == ID_ENABLE) {
        g_enabled = !g_enabled;
        s_cfg.enabled = g_enabled;
        if (!g_enabled) EndSession(L"disabled", s_active && QueryVoiceBar().visible);
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
    } else if (id >= ID_RESTORE_BASE && id < ID_RESTORE_BASE + s_menuLayouts.size()) {
        s_cfg.restoreLang = s_menuLayouts[id - ID_RESTORE_BASE].lang;
        s_cfg.restoreMode = RestoreFixed;
    } else {
        return;
    }
    SaveConfig(s_cfg);
}

// ---------------- window ----------------
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_START: StartSession(); return 0;
    case WM_APP_END:
        EndSession(wp == VK_ESCAPE ? L"key-esc" : L"key-enter", wp != VK_ESCAPE, wp == VK_ESCAPE);
        return 0;
    case WM_APP_CLICK: {
        if (!g_session) return 0;
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
        if (wp == TIMER_INJECT) OnInjectTick();
        else if (wp == TIMER_POLL) Poll();
        else if (wp == TIMER_CLOSE_BAR) {
            KillTimer(h, TIMER_CLOSE_BAR);
            LogSession(L"Sending Win+H to close Voice Typing");
            SendWinH();
            SetTimer(h, TIMER_RESTORE, kRestoreDelayMs, nullptr);
        } else if (wp == TIMER_RESTORE) {
            KillTimer(h, TIMER_RESTORE);
            LogSession(L"Restoring HKL=%p (cur=%p)", (void*)s_restoreTarget, (void*)CurrentHkl());
            SwitchTo(s_restoreTarget);
        }
        return 0;
    case WM_APP_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) ShowMenu();
        return 0;
    case WM_COMMAND: OnCommand(LOWORD(wp)); return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_UNLOCK || wp == WTS_SESSION_LOCK) ResetModifierState();
        return 0;
    case WM_DESTROY:
        if (g_captureHwnd) DestroyWindow(g_captureHwnd);
        WTSUnRegisterSessionNotification(h);
        RemoveMouseHook();
        RemoveKeyboardHook();
        CleanupVoiceBar();
        Shell_NotifyIconW(NIM_DELETE, &s_nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == s_taskbarCreated) { AddTrayIcon(); return 0; }  // Explorer restarted
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    if (wcsstr(cmd, L"--probe")) return RunProbe();

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\VoiceTypingSwitcher");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // crisp tray icon/menu

    s_cfg = LoadConfig();
    g_enabled = s_cfg.enabled != 0;
    g_hotkeyMods = s_cfg.hotkeyMods;
    g_hotkeyVk = s_cfg.hotkeyVk;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"VoiceTypingSwitcherWnd";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"VoiceTypingSwitcher", WS_POPUP,
                             0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WTSRegisterSessionNotification(g_hwnd, NOTIFY_FOR_THIS_SESSION);
    InitVoiceBar();

    if (!InstallKeyboardHook()) {
        MessageBoxW(nullptr, L"Không cài được keyboard hook.", L"VoiceTypingSwitcher", MB_ICONERROR);
        CleanupVoiceBar();
        return 1;
    }
    AddTrayIcon();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CloseHandle(mutex);
    return 0;
}
