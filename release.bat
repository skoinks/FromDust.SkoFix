@echo off
rem Packs build\dinput8.dll, the settings and the license into dist\FromDust.SkoFix.zip
setlocal
cd /d "%~dp0"
if not exist build\dinput8.dll echo Run build.bat first.& exit /b 1
if exist dist rmdir /s /q dist
mkdir dist\pack
copy /y build\dinput8.dll dist\pack >nul
copy /y FromDust.SkoFix.ini dist\pack >nul
copy /y LICENSE dist\pack\FromDust.SkoFix.license.txt >nul
powershell -NoProfile -Command "Compress-Archive -Path dist\pack\* -DestinationPath dist\FromDust.SkoFix.zip -Force; (Get-FileHash dist\FromDust.SkoFix.zip).Hash"
rmdir /s /q dist\pack
