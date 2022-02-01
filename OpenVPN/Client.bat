@echo off
cd /d "%~dp0"

:: Variables
set CONFIG=Client.ovpn
set INTERFACE=VPN
set REMOTE=x.x.x.x
set PORT=1194

:: Expand variables in config
copy /y "%CONFIG%.var" "%CONFIG%"
wscript expand.jse "%CONFIG%" "$interface$" "%INTERFACE%" "$remote$" "%REMOTE%" "$port$" "%PORT%" "$path$" "%CD:\=\\%"

:: Modify variables in scripts
copy /y "scripts\on.bat.var" "scripts\on.bat"
wscript expand.js "scripts\on.bat" "$interface$" "%INTERFACE%"

copy /y "scripts\off.bat.var" "scripts\off.bat"
wscript expand.js "scripts\off.bat" "$interface$" "%INTERFACE%"

:: Start VPN with the specified config
"%ProgramFiles%\OpenVPN\bin\openvpn.exe" --config "%CONFIG%"

:: Remove junk and logs
rmdir /s /q scripts\%%SystemDrive%%
rmdir /s /q "%USERPROFILE%\OpenVPN\log"
