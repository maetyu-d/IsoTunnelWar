@echo off
setlocal EnableDelayedExpansion

cd /d "%~dp0"

where cl.exe >nul 2>nul
if errorlevel 1 (
  if exist "C:\BuildTools\Common7\Tools\VsDevCmd.bat" set "VSINSTALL=C:\BuildTools"
  if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
    if not defined VSINSTALL (
      for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSINSTALL=%%i"
    )
  )
  if defined VSINSTALL if exist "!VSINSTALL!\Common7\Tools\VsDevCmd.bat" (
    call "!VSINSTALL!\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
  )
)

where cl.exe >nul 2>nul
if errorlevel 1 (
  echo Unable to find cl.exe. Install Visual Studio Build Tools with the C++ workload.
  exit /b 1
)

if not exist build mkdir build

cl /nologo /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE ^
  src\main.cpp ^
  /Fe:build\IsoMiddleEarth.exe ^
  /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib shell32.lib

if errorlevel 1 exit /b 1

echo Built build\IsoMiddleEarth.exe
