@echo off
cd /d "%~dp0SeamlessCoop"
if not exist "settings.bat" call "1 - SETUP.bat"
if not exist "settings.bat" exit /b 1
call "2 - PLAY.bat"
exit
