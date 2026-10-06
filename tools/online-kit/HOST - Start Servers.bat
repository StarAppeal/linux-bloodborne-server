@echo off
title Start co-op servers
rem HOST ONLY. Starts shadnet (accounts, matchmaking, summons; ports 31313-31315)
rem and the local backend (game login and online check; port 20443).
rem Leave both windows open while you play.
cd /d "%~dp0"
if not exist "settings.bat" (
    echo Run "1 - SETUP" first.
    pause
    exit /b 1
)
call settings.bat

set "PY=py -3"
where py >nul 2>nul
if errorlevel 1 set "PY=python"
%PY% --version >nul 2>nul
if errorlevel 1 (
    echo Python was not found. Install it from https://www.python.org/downloads/
    echo and tick "Add python.exe to PATH" during setup.
    pause
    exit /b 1
)

tasklist /fi "imagename eq shadnet.exe" | find /i "shadnet.exe" >nul
if not errorlevel 1 (
    echo A shadnet.exe is already running. If it is the test rig or the old
    echo "SERVER - Host P2P", close it first so the ports are free.
    pause
    exit /b 1
)

cd /d "%~dp0server"
start "co-op shadnet" shadnet.exe
cd /d "%~dp0backend"
start "co-op backend" %PY% bb_dummy_server.py --address %HOSTIP% --port 20443 --shadnet-webapi http://%HOSTIP%:31315

echo Started shadnet and the backend for %HOSTIP%.
echo Check from any PC: http://%HOSTIP%:20443/status should say "ok": true
echo.
pause
