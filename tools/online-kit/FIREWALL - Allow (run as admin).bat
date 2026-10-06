@echo off
title Allow co-op through Windows Firewall
rem Right-click, "Run as administrator". Everyone runs it once.
rem Adds inbound rules for shadps4.exe from this folder (P2P) and, for the host,
rem shadnet and the backend ports. Remove them later with
rem   netsh advfirewall firewall delete rule name="<rule name below>"
net session >nul 2>&1
if errorlevel 1 (
    echo Right-click this file and choose "Run as administrator".
    pause
    exit /b 1
)
netsh advfirewall firewall delete rule name="BB coop shadps4" >nul 2>&1
netsh advfirewall firewall delete rule name="BB coop host TCP" >nul 2>&1
netsh advfirewall firewall delete rule name="BB coop host UDP" >nul 2>&1
netsh advfirewall firewall add rule name="BB coop shadps4" dir=in action=allow program="%~dp0shadps4.exe" enable=yes profile=any
netsh advfirewall firewall add rule name="BB coop host TCP" dir=in action=allow protocol=TCP localport=31313,31315,20443 enable=yes profile=any
netsh advfirewall firewall add rule name="BB coop host UDP" dir=in action=allow protocol=UDP localport=31314,51610-51611 enable=yes profile=any
echo.
echo Done.
pause
