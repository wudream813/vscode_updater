@echo off
setlocal
cd /d "%~dp0"

where g++ >nul 2>nul
if not errorlevel 1 (
    echo Building with MinGW-w64...
    g++ -std=c++17 -O2 -Wall -Wextra -static update.cpp -lwininet -lbcrypt -lshell32 -o vscode_updater.exe
    if errorlevel 1 goto :fail
    goto :success
)

where cl >nul 2>nul
if not errorlevel 1 (
    echo Building with MSVC...
    cl /nologo /EHsc /std:c++17 /O2 /utf-8 /W4 /MT update.cpp /Fe:vscode_updater.exe /link wininet.lib bcrypt.lib shell32.lib
    if errorlevel 1 goto :fail
    goto :success
)

echo ERROR: Install MinGW-w64, or run from an x64 Visual Studio Developer Command Prompt.
exit /b 1

:fail
echo ERROR: Build failed.
exit /b 1

:success
echo Built vscode_updater.exe
exit /b 0
