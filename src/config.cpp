#include "app.h"

static const wchar_t* kKey = L"Software\\VoiceTypingSwitcher";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"VoiceTypingSwitcher";

static void ReadDword(const wchar_t* name, DWORD& out) {
    DWORD v, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        out = v;
}

static void WriteDword(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_DWORD, &v, sizeof(v));
}

Config LoadConfig() {
    Config c;
    ReadDword(L"Enabled", c.enabled);
    ReadDword(L"VoiceLang", c.voiceLang);
    ReadDword(L"RestoreLang", c.restoreLang);
    ReadDword(L"RestoreMode", c.restoreMode);
    ReadDword(L"HotkeyMods", c.hotkeyMods);
    ReadDword(L"HotkeyVk", c.hotkeyVk);
    c.hotkeyMods &= (MOD_WIN | MOD_CONTROL | MOD_ALT | MOD_SHIFT);
    if (!IsValidHotkey(c.hotkeyMods, c.hotkeyVk)) {
        c.hotkeyMods = MOD_WIN;
        c.hotkeyVk = 'H';
    }
    return c;
}

void SaveConfig(const Config& c) {
    WriteDword(L"Enabled", c.enabled);
    WriteDword(L"VoiceLang", c.voiceLang);
    WriteDword(L"RestoreLang", c.restoreLang);
    WriteDword(L"RestoreMode", c.restoreMode);
    WriteDword(L"HotkeyMods", c.hotkeyMods);
    WriteDword(L"HotkeyVk", c.hotkeyVk);
}

bool IsValidHotkey(DWORD mods, DWORD vk) {
    if (vk <= 0x07 || vk >= 0xFE) return false;
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_CAPITAL: case VK_NUMLOCK:
        return false;
    }
    if ((mods & (MOD_WIN | MOD_CONTROL | MOD_ALT)) != 0) return true;
    return (vk >= VK_F1 && vk <= VK_F24) || vk == VK_PAUSE || vk == VK_SCROLL ||
           vk == VK_INSERT || vk == VK_SNAPSHOT;
}

std::wstring FormatHotkey(DWORD mods, DWORD vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl + ";
    if (mods & MOD_ALT) s += L"Alt + ";
    if (mods & MOD_SHIFT) s += L"Shift + ";
    if (mods & MOD_WIN) s += L"Win + ";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        s.push_back((wchar_t)vk);
        return s;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        wchar_t buf[8];
        wsprintfW(buf, L"F%lu", vk - VK_F1 + 1);
        return s + buf;
    }
    switch (vk) {
    case VK_SPACE: return s + L"Space";
    case VK_TAB: return s + L"Tab";
    case VK_RETURN: return s + L"Enter";
    case VK_BACK: return s + L"Backspace";
    case VK_PAUSE: return s + L"Pause";
    case VK_SNAPSHOT: return s + L"Print Screen";
    }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    bool ext = vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
               vk == VK_PRIOR || vk == VK_NEXT || (vk >= VK_LEFT && vk <= VK_DOWN) || vk == VK_DIVIDE;
    wchar_t name[64] = L"";
    if (sc && GetKeyNameTextW((LONG)((sc << 16) | (ext ? (1 << 24) : 0)), name, 64))
        return s + name;
    wchar_t hex[16];
    wsprintfW(hex, L"0x%02lX", vk);
    return s + hex;
}

bool GetAutostart() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    if (!on) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName);
        return;
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName, REG_SZ, cmd.c_str(),
                    DWORD((cmd.size() + 1) * sizeof(wchar_t)));
}
