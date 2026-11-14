@echo off
rem Builds dd.exe and dd-gui.exe with the MSVC command line tools, without CMake.
rem Run from a "Developer Command Prompt", or let this script find vcvars64.
rem Each step prints a line with the time it started.
setlocal
cd /d "%~dp0"

echo [%time:~0,8%] build 1/6: the Visual Studio C++ build tools
where cl >nul 2>nul
if not errorlevel 1 goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
rem vcvars64 looks for vswhere on PATH first and says so on stderr when it is not there
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul

:build
set "CFLAGS=/nologo /W4 /O2 /MT /std:c11 /Iinclude /Ithird_party\xz\src\liblzma\api /DLZMA_API_STATIC /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0601"
if not exist build\obj mkdir build\obj

rem liblzma (third_party\xz) is built once; delete build\lzma.lib to rebuild it.
if exist build\lzma.lib (
    echo [%time:~0,8%] build 2/6: liblzma: already built
    goto lzma_done
)
echo [%time:~0,8%] build 2/6: liblzma (first build only)
if not exist build\obj\lzma mkdir build\obj\lzma
(for /r third_party\xz\src %%f in (*.c) do @echo "%%f") > build\obj\lzma\sources.rsp
cl @third_party\xz\liblzma.rsp /c /MP /Fobuild\obj\lzma\ @build\obj\lzma\sources.rsp >build\obj\lzma\build.log || (type build\obj\lzma\build.log & exit /b 1)
lib /nologo /out:build\lzma.lib build\obj\lzma\*.obj || exit /b 1
:lzma_done

echo [%time:~0,8%] build 3/6: compiling dd (16 files)
cl %CFLAGS% /c /MP /Fobuild\obj\ lib\copy.c lib\device.c lib\endpoint.c lib\format.c lib\fs_exfat.c lib\fs_ext.c lib\fs_fat.c lib\fs_ntfs.c lib\fs_swap.c lib\fsmap.c lib\hash.c lib\layout.c lib\options.c lib\xz.c cli\main.c gui\gui.c || exit /b 1
echo [%time:~0,8%] build 4/6: resources
rc /nologo /i include /fo build\obj\gui.res gui\gui.rc || exit /b 1
rc /nologo /i include /fo build\obj\dd.res cli\dd.rc || exit /b 1

set "CORE=build\obj\copy.obj build\obj\device.obj build\obj\endpoint.obj build\obj\format.obj build\obj\fs_exfat.obj build\obj\fs_ext.obj build\obj\fs_fat.obj build\obj\fs_ntfs.obj build\obj\fs_swap.obj build\obj\fsmap.obj build\obj\hash.obj build\obj\layout.obj build\obj\options.obj build\obj\xz.obj build\lzma.lib setupapi.lib bcrypt.lib"
echo [%time:~0,8%] build 5/6: linking build\dd.exe
link /nologo /out:build\dd.exe build\obj\main.obj build\obj\dd.res %CORE% || exit /b 1
echo [%time:~0,8%] build 6/6: linking build\dd-gui.exe
link /nologo /subsystem:windows /manifest:no /out:build\dd-gui.exe build\obj\gui.obj build\obj\gui.res %CORE% ^
     user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib uxtheme.lib || exit /b 1
echo [%time:~0,8%] build done
exit /b 0

:novs
echo Visual Studio C++ build tools not found.
exit /b 1
