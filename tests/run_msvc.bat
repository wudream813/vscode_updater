@echo off
setlocal
cl /nologo /EHsc /std:c++17 /O2 /utf-8 /W4 /MT update.cpp /Fe:vscode_updater.exe /link wininet.lib bcrypt.lib shell32.lib
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /O2 /utf-8 /W4 /MT tests\core_tests.cpp /Fe:core-tests.exe
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /O2 /utf-8 /W4 /MT tests\windows_tests.cpp /Fe:windows-tests.exe /link wininet.lib bcrypt.lib shell32.lib
if errorlevel 1 exit /b 1
core-tests.exe
if errorlevel 1 exit /b 1
windows-tests.exe tests\fixtures
if errorlevel 1 exit /b 1
python tests\smoke.py .\vscode_updater.exe
if errorlevel 1 exit /b 1
exit /b 0
