@echo off
set "KIT=%~dp0"
cd /d "%KIT%"
if not exist "settings.bat" (
    echo Run "1 - SETUP" first.
    pause
    exit /b 1
)
call "%KIT%settings.bat"
if "%GAMEDIR%"=="" set "GAMEDIR=%KIT%game"
if not exist "%GAMEDIR%\CUSA03173\eboot.bin" (
    echo Run "1 - SETUP" again: the game folder is missing.
    pause
    exit /b 1
)
set SHADPS4_BLOODBORNE_SEAMLESS_COOP=1
rem auto (and the old kit defaults 51610/51611) lets Windows pick a free UDP port.
set "SHADPS4_P2P_PORT="
if /i not "%P2PPORT%"=="auto" if not "%P2PPORT%"=="51610" if not "%P2PPORT%"=="51611" set "SHADPS4_P2P_PORT=%P2PPORT%"
set SHADPS4_BLOODBORNE_PARTY=%PARTY%
set SHADPS4_BLOODBORNE_PARTY_ROLE=%ROLE%
set SHADPS4_BLOODBORNE_RE_TRACE=0
if exist user\log\shad_log.txt copy /y user\log\shad_log.txt user\log\shad_log.prev.txt >nul
start "" "%KIT%shadps4.exe" -g "%GAMEDIR%\CUSA03173\eboot.bin"
