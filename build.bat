@echo off
setlocal

rem ================================================
rem  VS Code 自动更新程序 - 编译脚本
rem  自动选择可用的编译器:
rem    1. MinGW-w64 g++  (推荐, 无需安装 VS)
rem    2. MSVC cl        (需要 Visual Studio 环境)
rem ================================================

where g++ >nul 2>nul
if not errorlevel 1 (
    echo [1/2] 使用 MinGW g++ 编译...
    g++ -std=c++17 -O2 -Wall -static update.cpp -lwininet -o vscode_updater.exe
    if errorlevel 1 goto :fail
    echo [2/2] 编译成功: vscode_updater.exe
    echo 运行: vscode_updater.exe --help
    goto :end
)

where cl >nul 2>nul
if not errorlevel 1 (
    echo [1/2] 使用 MSVC cl 编译...
    cl /nologo /EHsc /std:c++17 /O2 /utf-8 /W3 update.cpp /Fe:vscode_updater.exe /link wininet.lib
    if errorlevel 1 goto :fail
    echo [2/2] 编译成功: vscode_updater.exe
    goto :end
)

echo [错误] 未找到 g++ 或 cl。
echo        请安装 MinGW-w64 (https://winlibs.com/) 或 Visual Studio Build Tools。
exit /b 1

:fail
echo [错误] 编译失败，请检查上方错误信息。
exit /b 1

:end
endlocal
