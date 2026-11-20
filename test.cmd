@echo off
setlocal
cd /d "%~dp0"
where g++ >nul 2>&1
if errorlevel 1 for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.*") do if exist "%%D\mingw64\bin\g++.exe" set "PATH=%%D\mingw64\bin;%PATH%"
if not exist bin mkdir bin
g++ -std=c++17 -Wall -Wextra tests\lang_test.cpp src\lang.cpp src\config.cpp src\voicebar.cpp src\updater.cpp ^
    -o bin\lang_test.exe -luser32 -ladvapi32 -ldwmapi -lole32 -loleaut32 -luuid -lwinhttp || exit /b 1
bin\lang_test.exe
