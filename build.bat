@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul
if not exist "%~dp0build" mkdir "%~dp0build"
cd /d "%~dp0build"
cl /nologo /utf-8 /O2 /MT /W3 /EHsc /LD "%~dp0src\dllmain.cpp" "%~dp0src\cursor.cpp" "%~dp0src\d3d.cpp" "%~dp0src\patches.cpp" /Fe:dinput8.dll /link /DEF:"%~dp0src\dinput8.def" user32.lib kernel32.lib
if errorlevel 1 exit /b 1
copy /y "%~dp0bhd_mod.ini" . >nul
echo Build OK: %~dp0build\dinput8.dll
