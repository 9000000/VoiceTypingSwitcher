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
    std::printf("Config: enabled=%lu voice=0x%04lX restore=0x%04lX mode=%lu hotkey=%ls ui=%lu autostart=%d\n",
                c.enabled, c.voiceLang, c.restoreLang, c.restoreMode,
                FormatHotkey(c.hotkeyMods, c.hotkeyVk).c_str(), c.uiLang, GetAutostart());
    CHECK(c.restoreMode <= RestorePrevious);
    CHECK(c.uiLang <= UiEn);
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

    int maj = 0, min = 0, pat = 0;
    CHECK(ParseSemVer(L"v1.2.3", maj, min, pat) && maj == 1 && min == 2 && pat == 3);
    CHECK(ParseSemVer(L"2.0.0-beta", maj, min, pat) && maj == 2 && min == 0 && pat == 0);
    CHECK(!ParseSemVer(L"invalid", maj, min, pat));
    CHECK(IsNewerVersion(L"v1.2.0", 1, 1, 0));
    CHECK(IsNewerVersion(L"v1.1.1", 1, 1, 0));
    CHECK(IsNewerVersion(L"v2.0.0", 1, 9, 9));
    CHECK(!IsNewerVersion(L"v1.1.0", 1, 1, 0));
    CHECK(!IsNewerVersion(L"v1.0.9", 1, 1, 0));

    const std::string mockRel = R"({
        "html_url": "https://github.com/9000000/VoiceTypingSwitcher/releases/tag/v1.2.0",
        "tag_name": "v1.2.0",
        "author": {"html_url": "https://github.com/9000000"},
        "assets": [
            {"name": "source.zip", "browser_download_url": "https://github.com/9000000/VoiceTypingSwitcher/releases/download/v1.2.0/source.zip"},
            {"name": "VoiceTypingSwitcher.exe", "browser_download_url": "https://github.com/9000000/VoiceTypingSwitcher/releases/download/v1.2.0/VoiceTypingSwitcher.exe"}
        ]
    })";
    UpdateInfo ui;
    CHECK(ParseReleaseJson(mockRel, ui));
    CHECK(ui.tag == L"v1.2.0");
    CHECK(ui.htmlUrl == L"https://github.com/9000000/VoiceTypingSwitcher/releases/tag/v1.2.0");
    CHECK(ui.exeUrl == L"https://github.com/9000000/VoiceTypingSwitcher/releases/download/v1.2.0/VoiceTypingSwitcher.exe");

    const std::string noExeRel = R"({"tag_name":"v1.3.0","html_url":"https://github.com/9000000/VoiceTypingSwitcher/releases/tag/v1.3.0","assets":[]})";
    UpdateInfo ui2;
    CHECK(ParseReleaseJson(noExeRel, ui2));
    CHECK(ui2.tag == L"v1.3.0" && ui2.exeUrl.empty());

    CHECK(InitVoiceBar());
    VoiceBarInfo bar = QueryVoiceBar();
    std::printf("VoiceBar query OK (visible=%d)\n", bar.visible);
    CleanupVoiceBar();

    std::printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails != 0;
}
