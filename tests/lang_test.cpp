// Console smoke test: layout enumeration and config defaults.
#include "../src/app.h"
#include <cstdio>

HWND g_hwnd = nullptr, g_captureHwnd = nullptr;  // referenced by app.h externs
volatile bool g_enabled = true, g_session = false, g_capturing = false;
DWORD g_hotkeyMods = MOD_WIN, g_hotkeyVk = 'H';

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main() {
    auto layouts = ListLayouts();
    std::printf("Installed layouts: %zu\n", layouts.size());
    for (auto& l : layouts) std::printf("  0x%04X hkl=%p %ls\n", l.lang, (void*)l.hkl, l.name.c_str());
    CHECK(!layouts.empty());
    CHECK(FindHkl(0x0409) != nullptr);
    CHECK(FindHkl(0x042A) != nullptr);
    CHECK(FindHkl(0x7FFF) == nullptr);
    for (auto& l : layouts) CHECK(FindHkl(l.lang) == l.hkl);

    Config c = LoadConfig();
    std::printf("Config: enabled=%lu voice=0x%04lX restore=0x%04lX mode=%lu hotkey=%ls autostart=%d\n",
                c.enabled, c.voiceLang, c.restoreLang, c.restoreMode,
                FormatHotkey(c.hotkeyMods, c.hotkeyVk).c_str(), GetAutostart());
    CHECK(c.restoreMode <= RestorePrevious);
    CHECK(IsValidHotkey(c.hotkeyMods, c.hotkeyVk));

    CHECK(IsValidHotkey(MOD_WIN, 'C'));
    CHECK(IsValidHotkey(MOD_CONTROL | MOD_SHIFT, 'V'));
    CHECK(IsValidHotkey(0, VK_F9));
    CHECK(!IsValidHotkey(0, 'A'));
    CHECK(!IsValidHotkey(MOD_SHIFT, 'A'));
    CHECK(!IsValidHotkey(MOD_WIN, VK_LWIN));
    CHECK(FormatHotkey(MOD_WIN, 'C') == L"Win + C");
    CHECK(FormatHotkey(MOD_CONTROL | MOD_ALT, 'Q') == L"Ctrl + Alt + Q");
    CHECK(FormatHotkey(0, VK_F9) == L"F9");

    CHECK(InitVoiceBar());
    VoiceBarInfo bar = QueryVoiceBar();
    std::printf("VoiceBar query OK (visible=%d)\n", bar.visible);
    CleanupVoiceBar();

    std::printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails != 0;
}
