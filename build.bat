@echo off
cd /d "%~dp0"

:: Compile resources
rc broadcast.rc > nul

:: Build the executable
clang -O2 -mconsole -municode %* broadcast.c broadcast.res -o broadcast.exe -lws2_32 -lIphlpapi -lshlwapi -ladvapi32

:: Embed manifest
mt -nologo -manifest broadcast.exe.manifest -outputresource:"broadcast.exe;1"
