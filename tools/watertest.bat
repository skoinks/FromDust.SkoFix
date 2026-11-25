@echo off
setlocal
set "WHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%WHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS echo Visual Studio with the C++ tools was not found.& exit /b 1
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>nul
cd /d "%~dp0..\build"
cl /nologo /std:c++17 /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS ..\tools\watertest.cpp /Fowatertest.obj /Fewatertest.exe && .\watertest.exe ..
