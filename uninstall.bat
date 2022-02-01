@echo off
cd /d "%~dp0"

:: Remove BROADcast directory after running this script
:: to completely uninstall it from your system.

set lnkpath=%APPDATA%\Microsoft\Windows\Start Menu\Programs\BROADcast.lnk
del /q /f "%lnkpath%"

:: Remove BROADcast service
broadcast.exe uninstall
