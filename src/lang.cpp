#include "app.h"

std::vector<Layout> ListLayouts() {
    HKL list[64];
    int n = GetKeyboardLayoutList(64, list);
    std::vector<Layout> out;
    for (int i = 0; i < n; ++i) {
        LANGID lang = LOWORD((UINT_PTR)list[i]);
        bool dup = false;
        for (auto& l : out) dup |= l.lang == lang;
        if (dup) continue;
        wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"", name[128] = L"";
        LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0);
        if (!GetLocaleInfoEx(locale, LOCALE_SLOCALIZEDDISPLAYNAME, name, 128))
            wsprintfW(name, L"0x%04X", lang);
        out.push_back({list[i], lang, name});
    }
    return out;
}

HKL FindHkl(LANGID lang) {
    HKL list[64];
    int n = GetKeyboardLayoutList(64, list);
    for (int i = 0; i < n; ++i)
        if (LOWORD((UINT_PTR)list[i]) == lang) return list[i];
    return nullptr;
}

static HWND FocusedWindow(HWND fg) {
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(0, &gti)) {
        if (gti.hwndFocus) return gti.hwndFocus;
        if (gti.hwndActive) return gti.hwndActive;
    }
    return fg;
}

HKL CurrentHkl() {
    HWND target = FocusedWindow(GetForegroundWindow());
    return GetKeyboardLayout(target ? GetWindowThreadProcessId(target, nullptr) : 0);
}

void SwitchTo(HKL hkl) {
    if (!hkl) return;
    HWND fg = GetForegroundWindow();
    HWND target = FocusedWindow(fg);
    if (!target) return;
    PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)hkl);
    if (fg && fg != target &&
        GetWindowThreadProcessId(fg, nullptr) != GetWindowThreadProcessId(target, nullptr)) {
        PostMessageW(fg, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)hkl);
    }
}
