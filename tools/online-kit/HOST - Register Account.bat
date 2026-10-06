@echo off
title Register a shadnet account
rem HOST ONLY, with "HOST - Start Servers" running. Register yourself and your friend,
rem using the account names and passwords each of you typed into "1 - SETUP".
cd /d "%~dp0server\clientsample"

:again
echo.
set "ID="
set "PW="
set /p "ID=Account name (blank to quit): "
if "%ID%"=="" exit /b 0
set /p "PW=Password: "
if "%PW%"=="" goto :again
shadnet-sample.exe 127.0.0.1 31313 register "%ID%" "%PW%" "%ID%.%PW%@p2p.db"
goto :again
