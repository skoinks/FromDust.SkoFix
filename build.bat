@echo off
setlocal
set "WHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%WHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS echo Visual Studio with the C++ tools was not found.& exit /b 1
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>nul
cd /d "%~dp0"
if not exist build mkdir build
rc /nologo /fo build\version.res src\version.rc || exit /b 1
cl /nologo /std:c++17 /O2 /MT /W3 /EHa /LD src\*.cpp build\version.res /Fobuild\ /Febuild\dinput8.dll /link /DEF:src\dinput8.def /SUBSYSTEM:WINDOWS user32.lib gdi32.lib
