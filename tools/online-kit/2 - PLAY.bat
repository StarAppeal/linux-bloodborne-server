@echo off
cd /d "%~dp0"
if not exist "settings.bat" (
    echo Run "1 - SETUP" first.
    pause
    exit /b 1
)
if not exist "game\CUSA03173\eboot.bin" (
    echo Run "1 - SETUP" again: this kit plays from its own game folder now.
    pause
    exit /b 1
)
call settings.bat
set SHADPS4_BLOODBORNE_SEAMLESS_COOP=1
set SHADPS4_P2P_PORT=%P2PPORT%
set SHADPS4_BLOODBORNE_PARTY=%PARTY%
set SHADPS4_BLOODBORNE_PARTY_ROLE=%ROLE%
set SHADPS4_BLOODBORNE_RE_TRACE=0
if exist user\log\shad_log.txt copy /y user\log\shad_log.txt user\log\shad_log.prev.txt >nul
start "" "%~dp0shadps4.exe" -g "%~dp0game\CUSA03173\eboot.bin"
