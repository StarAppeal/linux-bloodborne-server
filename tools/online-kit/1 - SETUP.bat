@echo off
title Bloodborne co-op setup
rem Everyone runs this once: asks for the host IP, your account and the party,
rem then prepares this folder's own "user" dir from your Bloodborne.PC.Online install.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1"
echo.
pause
