@echo off
setlocal
cd /d "%~dp0"
if not exist bin mkdir bin
g++ -std=c++17 -Wall -Wextra tests\lang_test.cpp src\lang.cpp src\config.cpp src\voicebar.cpp ^
    -o bin\lang_test.exe -luser32 -ladvapi32 -ldwmapi -lole32 -loleaut32 -luuid || exit /b 1
bin\lang_test.exe
