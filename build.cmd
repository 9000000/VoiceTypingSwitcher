@echo off
rem Build VoiceTypingSwitcher.exe with MinGW-w64 (winget install BrechtSanders.WinLibs.POSIX.UCRT)
setlocal
cd /d "%~dp0"
where windres >nul 2>&1
if errorlevel 1 for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.*") do if exist "%%D\mingw64\bin\windres.exe" set "PATH=%%D\mingw64\bin;%PATH%"
if not exist bin mkdir bin
if not exist assets\app.ico python tools\make_icon.py || exit /b 1

pushd src
windres app.rc -O coff -o ..\bin\app.res || (popd & exit /b 1)
popd

g++ -std=c++17 -Os -s -static -municode -mwindows -Wall -Wextra ^
    -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections ^
    src\main.cpp src\hooks.cpp src\lang.cpp src\voicebar.cpp src\config.cpp src\updater.cpp bin\app.res ^
    -o bin\VoiceTypingSwitcher.exe -luser32 -lshell32 -ldwmapi -ladvapi32 -lole32 -loleaut32 -luuid -lwtsapi32 -lwinhttp || exit /b 1

for %%F in (bin\VoiceTypingSwitcher.exe) do echo Built %%~fF (%%~zF bytes)
