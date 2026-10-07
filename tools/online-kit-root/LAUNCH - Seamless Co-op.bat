@echo off
cd /d "%~dp0SeamlessCoop"
if not exist "settings.bat" call "%~dp0SeamlessCoop\1 - SETUP.bat"
if not exist "%~dp0SeamlessCoop\settings.bat" exit /b 1
call "%~dp0SeamlessCoop\2 - PLAY.bat"
exit
