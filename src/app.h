#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00  // Windows 10+
#include <windows.h>
#include <string>
#include <vector>
#include "version.h"

// ---- Messages posted to the main window ----
enum : UINT {
    WM_APP_START = WM_APP + 1,  // trigger hotkey pressed: switch to voice language, (re)start session
    WM_APP_END,                 // Enter / Esc: end session, restore language
    WM_APP_CLICK,               // mouse click during session (lParam = MAKELPARAM(x, y))
    WM_APP_TRAY,                // tray icon callback
    WM_APP_CAPTURE_PREVIEW,     // wParam = current modifier mask while binding
    WM_APP_CAPTURE_KEY,         // wParam = mods, lParam = vk captured
    WM_APP_CAPTURE_CANCEL,      // Esc pressed in capture mode
    WM_APP_CAPTURE_ACCEPT,      // Enter pressed after a combo was captured
    WM_APP_UPDATE_CHECK,        // lParam = UpdateInfo* (heap-allocated, receiver frees)
    WM_APP_UPDATE_DL,           // lParam = DownloadResult* (heap-allocated, receiver frees)
};

constexpr ULONG_PTR kTag = 0x56545357;  // 'VTSW' marks input injected by this app

// ---- Config (registry: HKCU\Software\VoiceTypingSwitcher) ----
enum RestoreMode : DWORD { RestoreFixed = 0, RestorePrevious = 1 };
enum UiLang : DWORD { UiVi = 0, UiEn = 1 };
struct Config {
    DWORD enabled = 1;
    DWORD voiceLang = 0x042A;    // Vietnamese
    DWORD restoreLang = 0x0409;  // English (US)
    DWORD restoreMode = RestoreFixed;
    DWORD hotkeyMods = MOD_WIN;  // MOD_WIN | MOD_CONTROL | MOD_ALT | MOD_SHIFT
    DWORD hotkeyVk = 'H';        // trigger key (default Win+H)
    DWORD uiLang = UiVi;         // 0 = Vietnamese, 1 = English
};
Config LoadConfig();
void SaveConfig(const Config&);
bool GetAutostart();
void SetAutostart(bool on);
bool IsValidHotkey(DWORD mods, DWORD vk);
std::wstring FormatHotkey(DWORD mods, DWORD vk);

// ---- Keyboard layouts ----
struct Layout { HKL hkl; LANGID lang; std::wstring name; };
std::vector<Layout> ListLayouts();
HKL FindHkl(LANGID lang);
HKL CurrentHkl();
void SwitchTo(HKL hkl);

// ---- Voice Typing bar (hosted by TextInputHost.exe) ----
struct VoiceBarInfo {
    bool visible = false;
    RECT rect{};
};
bool InitVoiceBar();
void CleanupVoiceBar();
VoiceBarInfo QueryVoiceBar();
bool IsPointOnVoiceBar(POINT pt, const RECT* cachedRect = nullptr);
void LogTopWindowsOnce();
int RunProbe();  // --probe: log TextInputHost windows + UIA to probe.log

// ---- Diagnostic session log (%TEMP%\VoiceTypingSwitcher.log) ----
void ResetSessionLog();
void LogSession(const wchar_t* fmt, ...);

// ---- GitHub Release Updater ----
enum UpdateStatus { UpdUpToDate = 0, UpdAvailable = 1, UpdNoReleases = 2, UpdError = 3 };
struct UpdateInfo {
    UpdateStatus status = UpdError;
    DWORD httpStatus = 0;
    std::wstring tag;
    std::wstring htmlUrl;
    std::wstring exeUrl;
    std::wstring error;
};
struct DownloadResult {
    bool ok = false;
    std::wstring tempExePath;
    std::wstring error;
};
bool ParseSemVer(const wchar_t* tag, int& maj, int& min, int& pat);
bool IsNewerVersion(const wchar_t* remoteTag, int curMaj, int curMin, int curPat);
bool ParseReleaseJson(const std::string& json, UpdateInfo& out);
void StartUpdateCheckAsync(HWND notifyWnd);
void StartUpdateDownloadAsync(HWND notifyWnd, const std::wstring& exeUrl);
bool ReplaceSelfAndPrepareRelaunch(const std::wstring& tempExePath, std::wstring& outExePath);
void CleanupOldBinary(const wchar_t* cmdLine);

// ---- Hooks ----
extern HWND g_hwnd;
extern HWND g_captureHwnd;
extern volatile bool g_enabled;
extern volatile bool g_session;    // true while a voice typing session is tracked
extern volatile bool g_capturing;  // true while the custom hotkey dialog is listening
extern DWORD g_hotkeyMods;
extern DWORD g_hotkeyVk;
bool InstallKeyboardHook();
void RemoveKeyboardHook();
void InstallMouseHook();
void RemoveMouseHook();
void ResetModifierState();
void SendEsc();
void InjectWinH();
