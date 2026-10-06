@echo off
cd /d "%~dp0"
if not exist "settings.bat" (
    echo Run "1 - SETUP" first.
    pause
    exit /b 1
)
call settings.bat
set SHADPS4_BLOODBORNE_SEAMLESS_COOP=1
set SHADPS4_P2P_PORT=%P2PPORT%
set SHADPS4_BLOODBORNE_PARTY=%PARTY%
set SHADPS4_BLOODBORNE_PARTY_ROLE=%ROLE%
set SHADPS4_BLOODBORNE_RE_TRACE=0
start "" "%~dp0shadps4.exe" -g "%INSTALL%\DATA\Bloodborne\CUSA03173\eboot.bin"
