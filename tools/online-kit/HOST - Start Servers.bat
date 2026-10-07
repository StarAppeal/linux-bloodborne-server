@echo off
title Start co-op servers
rem HOST ONLY. Starts shadnet (accounts, matchmaking, summons; ports 31313-31315)
rem and the local backend (game login and online check; port 20443).
rem Leave both windows open while you play. Running it again starts only what is missing.
set "KIT=%~dp0"
cd /d "%KIT%"
if not exist "%KIT%settings.bat" (
    echo Run "1 - SETUP" first.
    pause
    exit /b 1
)
call "%KIT%settings.bat"

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
if errorlevel 1 (
    cd /d "%KIT%server"
    start "co-op shadnet" "%KIT%server\shadnet.exe"
    echo Started shadnet.
) else (
    echo shadnet is already running. If it is the test rig or the old
    echo "SERVER - Host P2P" rather than this one, close it and run this again.
)

netstat -ano | find ":20443 " | find "LISTENING" >nul
if errorlevel 1 (
    cd /d "%KIT%backend"
    start "co-op backend" %PY% "%KIT%backend\bb_dummy_server.py" --address %HOSTIP% --port 20443 --shadnet-webapi http://%HOSTIP%:31315
    echo Started the backend for %HOSTIP%.
) else (
    echo The backend is already running.
)

echo.
echo Check from any PC: http://%HOSTIP%:20443/status should say "ok": true
echo.
pause
