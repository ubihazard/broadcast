@echo off
cd /d "%~dp0"

:: Compile resources
rc broadcast.rc > nul

:: Build the executable
clang -O2 -m32 -mconsole -municode %* broadcast.c broadcast.res -o BROADcast.exe -lws2_32 -lIphlpapi -lshlwapi -ladvapi32

:: Embed manifest
mt -nologo -manifest BROADcast.exe.manifest -outputresource:"BROADcast.exe;1"
