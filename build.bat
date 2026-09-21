@echo off
setlocal
cd /d "%~dp0"

set "SRC=main.c hotkey.c settings.c theme.c capture.c save.c log.c overlay.c toolbar.c poly.c hdr.c settings_ui.c record.c audio.c recordbar.c encoder.c hdrvideo.c mp4tag.c wavsink.c remux.c lang.c tabs.c"
set "LIBS=user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib uxtheme.lib shlwapi.lib comctl32.lib d3d11.lib dxgi.lib dxguid.lib windowscodecs.lib uuid.lib dwmapi.lib mfplat.lib mfreadwrite.lib mfuuid.lib"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto :mingw

rem vcvars64 prints a harmless "vswhere.exe not found" on this box but still
rem sets the environment; ignore its output.
call "%VCVARS%" >nul 2>&1
rc /nologo /fo nitshot.res nitshot.rc || goto :fail
cl /nologo /TC /std:c17 /O2 /MT /W4 /WX /permissive- /guard:cf /Qspectre /sdl /utf-8 ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS %SRC% ^
   /link nitshot.res /SUBSYSTEM:WINDOWS /OUT:Nitshot.exe ^
   %LIBS% /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /CETCOMPAT || goto :fail
del /q *.obj nitshot.res >nul 2>&1
echo Built Nitshot.exe with MSVC.
goto :eof

:mingw
where gcc >nul 2>&1 || goto :fail
rem --codepage=65001: the .rc is UTF-8 and carries a copyright sign
windres --codepage=65001 nitshot.rc -O coff -o nitshot.res.o || goto :fail
rem mingw-w64 always links its own default-manifest.o, which collides with
rem ours ("multiple non-default manifests"); shadow it with an empty object
rem found first via -B.
break > empty.c
gcc -c empty.c -o default-manifest.o || goto :fail
gcc -std=c17 -O2 -municode -mwindows -B. -Wall -Wextra ^
    %SRC% nitshot.res.o -o Nitshot.exe ^
    -luser32 -lgdi32 -lshell32 -ladvapi32 -lole32 -loleaut32 -luxtheme -lshlwapi ^
    -lcomctl32 -ld3d11 -ldxgi -ldxguid -lwindowscodecs -luuid -ldwmapi -lmfplat -lmfreadwrite -lmfuuid || goto :fail
del /q nitshot.res.o empty.c default-manifest.o >nul 2>&1
echo Built Nitshot.exe with MinGW.
goto :eof

:fail
echo BUILD FAILED
exit /b 1
