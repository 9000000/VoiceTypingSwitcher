# VoiceTypingSwitcher

[Tiếng Việt](README.md) | **English**

Lightweight Windows 11 system tray utility written in pure Win32 C++ (~277 KB executable, ~2.5 MB private RAM, 0% idle CPU):

- Press the **activation hotkey** (default **Win+H**, switchable to **Win+C** or any custom key combination): automatically switches the input language to your dictation language (default **Vietnamese**) before opening Windows Voice Typing.
- When ending dictation (**Enter**, **clicking outside** the Voice Typing bar, **Esc**, clicking **X**, or when the bar closes automatically):
  - On **Enter** or **clicking outside** the bar, the app automatically sends `Win+H` to **close the Voice Typing bar** before restoring the input language.
  - Restores either a **fixed language** (default **English US**) or the **previous language** active before triggering Voice Typing.

## Usage
Run `bin\VoiceTypingSwitcher.exe`. Left-click or right-click the blue microphone icon in the system tray:

| Tray Menu Item | Description |
|---|---|
| `Bật` (Enabled) | Toggle the switcher on/off (when off, the hotkey behaves normally) |
| `Phím tắt` (Hotkey) | Quick-select `Win + H`, `Win + C`, or `Gán phím khác...` to bind a custom shortcut (e.g. `Alt+Q`, `Ctrl+Shift+V`, `F9`) |
| `Ngôn ngữ khi Voice Typing` (Voice language) | Target keyboard language to switch to when activated |
| `Ngôn ngữ khi thoát` (Restore language) | Fixed language to restore, or `Ngôn ngữ trước khi kích hoạt` (previous layout before activation) |
| `Khởi động cùng Windows` (Run at startup) | Registers/removes startup entry under `HKCU\...\Run` |
| `Thoát` (Exit) | Close the app |

Settings are stored in the registry at `HKCU\Software\VoiceTypingSwitcher`. Available keyboard layouts are populated dynamically from Windows Settings › Time & language.

## Build
Requires MinGW-w64:

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

Then run:

```bat
build.cmd   :: outputs bin\VoiceTypingSwitcher.exe
test.cmd    :: builds and runs tests (layout detection, config, UIA probe, hotkey helpers)
```

## Diagnostics
- The latest session trace is automatically written to `%TEMP%\VoiceTypingSwitcher.log` (overwritten on each activation).
- `VoiceTypingSwitcher.exe --probe` writes `probe.log` (next to the `.exe`) for 60 seconds, logging `TextInputHost.exe` windows and UI Automation state changes.

## Limitations
- Because the app runs as a standard user (`asInvoker`), Windows UIPI blocks keyboard hooks and layout switching when an elevated (**Administrator**) window has focus.
- Clicking inside the Voice Typing bar itself (e.g. mic toggle or settings) does not end the session. Closing via the **X** button is detected within ~1 second.

## Project Structure
```
src/main.cpp      Tray icon, menu, hotkey capture dialog, session state machine
src/hooks.cpp     Low-level keyboard/mouse hooks and Win+H input injection
src/lang.cpp      Keyboard layout enumeration and WM_INPUTLANGCHANGEREQUEST switching
src/voicebar.cpp  Voice Typing bar detection (FindWindowExW + IUIAutomation) and --probe
src/config.cpp    Registry configuration, hotkey formatting/validation, Windows startup
```
