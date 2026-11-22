#include "app.h"

HWND g_hwnd = nullptr;
HWND g_captureHwnd = nullptr;
volatile bool g_enabled = true;
volatile bool g_session = false;
volatile bool g_capturing = false;
DWORD g_hotkeyMods = MOD_WIN;
DWORD g_hotkeyVk = 'H';

// All hook callbacks run on the main (UI) thread, so this state needs no locking.
static HHOOK s_kb = nullptr, s_ms = nullptr;
static bool s_lwin = false, s_rwin = false;
static bool s_lctrl = false, s_rctrl = false;
static bool s_lalt = false, s_ralt = false;
static bool s_lshift = false, s_rshift = false;
static bool s_swallowed = false;  // trigger key down was eaten; eat its key-up too
static bool s_pending = false;    // trigger eaten, waiting for InjectWinH()
static WORD s_heldWinVk = 0;      // Win key whose physical key-up we held back

static bool Tagged(const KBDLLHOOKSTRUCT* k) { return k->dwExtraInfo == kTag; }

static DWORD CurrentMods() {
    DWORD m = 0;
    if (s_lwin || s_rwin) m |= MOD_WIN;
    if (s_lctrl || s_rctrl) m |= MOD_CONTROL;
    if (s_lalt || s_ralt) m |= MOD_ALT;
    if (s_lshift || s_rshift) m |= MOD_SHIFT;
    return m;
}

static bool* ModFlag(DWORD vk) {
    switch (vk) {
    case VK_LWIN: return &s_lwin;
    case VK_RWIN: return &s_rwin;
    case VK_LCONTROL: case VK_CONTROL: return &s_lctrl;
    case VK_RCONTROL: return &s_rctrl;
    case VK_LMENU: case VK_MENU: return &s_lalt;
    case VK_RMENU: return &s_ralt;
    case VK_LSHIFT: case VK_SHIFT: return &s_lshift;
    case VK_RSHIFT: return &s_rshift;
    default: return nullptr;
    }
}

static LRESULT CALLBACK KbProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
    auto* k = (KBDLLHOOKSTRUCT*)lp;
    if (Tagged(k)) return CallNextHookEx(nullptr, code, wp, lp);

    bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    bool up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
    DWORD vk = k->vkCode;
    bool* mod = ModFlag(vk);
    if (mod) {
        if (down) *mod = true;
        else if (up) *mod = false;
    }

    // While capturing a custom hotkey, swallow keys so OS shortcuts don't fire.
    if (g_capturing) {
        if (mod) {
            if (g_captureHwnd) PostMessageW(g_captureHwnd, WM_APP_CAPTURE_PREVIEW, (WPARAM)CurrentMods(), 0);
            return 1;
        }
        if (down && g_captureHwnd) {
            DWORD m = CurrentMods();
            if (vk == VK_ESCAPE && m == 0)
                PostMessageW(g_captureHwnd, WM_APP_CAPTURE_CANCEL, 0, 0);
            else if (vk == VK_RETURN && m == 0)
                PostMessageW(g_captureHwnd, WM_APP_CAPTURE_ACCEPT, 0, 0);
            else
                PostMessageW(g_captureHwnd, WM_APP_CAPTURE_KEY, (WPARAM)m, (LPARAM)vk);
        }
        return 1;
    }

    if (up && (vk == VK_LWIN || vk == VK_RWIN) && s_pending) {
        // Win released before we injected Win+H: hold key-up back so Start menu does not open.
        s_heldWinVk = (WORD)vk;
        return 1;
    }

    if (g_enabled && vk == g_hotkeyVk) {
        if (down && CurrentMods() == g_hotkeyMods) {
            if (!s_swallowed) {
                s_swallowed = s_pending = true;
                PostMessageW(g_hwnd, WM_APP_START, 0, 0);
            }
            return 1;  // eat trigger key and its auto-repeat
        }
        if (up && s_swallowed) { s_swallowed = false; return 1; }
    } else if (down && g_session && !(k->flags & LLKHF_INJECTED) &&
               (vk == VK_RETURN || vk == VK_ESCAPE)) {
        PostMessageW(g_hwnd, WM_APP_END, (WPARAM)vk, 0);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK MsProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
        auto* m = (MSLLHOOKSTRUCT*)lp;
        if (!(m->flags & LLMHF_INJECTED))
            PostMessageW(g_hwnd, WM_APP_CLICK, 0, MAKELPARAM((short)m->pt.x, (short)m->pt.y));
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void ResetModifierState() {
    s_lwin = s_rwin = s_lctrl = s_rctrl = s_lalt = s_ralt = s_lshift = s_rshift = false;
    s_swallowed = s_pending = false;
    s_heldWinVk = 0;
}

static INPUT Key(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    bool ext = vk == VK_LWIN || vk == VK_RWIN || vk == VK_RCONTROL || vk == VK_RMENU;
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = kTag;
    return in;
}

static void SendWinH() {
    INPUT in[16];
    UINT n = 0;

    // If the user triggered via Alt/Ctrl/Shift, mask lone-Alt menu activation and lift them first.
    if (s_lalt || s_ralt) {
        in[n++] = Key(0xE8, false);
        in[n++] = Key(0xE8, true);
    }
    if (s_lctrl) in[n++] = Key(VK_LCONTROL, true);
    if (s_rctrl) in[n++] = Key(VK_RCONTROL, true);
    if (s_lalt) in[n++] = Key(VK_LMENU, true);
    if (s_ralt) in[n++] = Key(VK_RMENU, true);
    if (s_lshift) in[n++] = Key(VK_LSHIFT, true);
    if (s_rshift) in[n++] = Key(VK_RSHIFT, true);

    bool winDown = s_lwin || s_rwin || s_heldWinVk != 0;
    if (!winDown) in[n++] = Key(VK_LWIN, false);
    in[n++] = Key('H', false);
    in[n++] = Key('H', true);
    if (s_heldWinVk) in[n++] = Key(s_heldWinVk, true);
    else if (!s_lwin && !s_rwin) in[n++] = Key(VK_LWIN, true);

    s_heldWinVk = 0;
    SendInput(n, in, sizeof(INPUT));
}

void SendEsc() {
    INPUT in[16];
    UINT n = 0;
    if (s_lalt || s_ralt) {
        in[n++] = Key(0xE8, false);
        in[n++] = Key(0xE8, true);
    }
    if (s_lctrl) in[n++] = Key(VK_LCONTROL, true);
    if (s_rctrl) in[n++] = Key(VK_RCONTROL, true);
    if (s_lalt) in[n++] = Key(VK_LMENU, true);
    if (s_ralt) in[n++] = Key(VK_RMENU, true);
    if (s_lshift) in[n++] = Key(VK_LSHIFT, true);
    if (s_rshift) in[n++] = Key(VK_RSHIFT, true);
    if (s_lwin) in[n++] = Key(VK_LWIN, true);
    if (s_rwin) in[n++] = Key(VK_RWIN, true);
    in[n++] = Key(VK_ESCAPE, false);
    in[n++] = Key(VK_ESCAPE, true);
    SendInput(n, in, sizeof(INPUT));
}

void InjectWinH() {
    if (!s_pending) return;
    s_pending = false;
    SendWinH();
}

bool InstallKeyboardHook() {
    s_kb = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, GetModuleHandleW(nullptr), 0);
    return s_kb != nullptr;
}
void RemoveKeyboardHook() {
    if (s_kb) UnhookWindowsHookEx(s_kb);
    s_kb = nullptr;
}
void InstallMouseHook() {
    if (!s_ms) s_ms = SetWindowsHookExW(WH_MOUSE_LL, MsProc, GetModuleHandleW(nullptr), 0);
}
void RemoveMouseHook() {
    if (s_ms) UnhookWindowsHookEx(s_ms);
    s_ms = nullptr;
}
