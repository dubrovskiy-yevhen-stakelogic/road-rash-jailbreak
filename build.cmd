@echo off
setlocal
set BUILD_DIR=%1
if "%BUILD_DIR%"=="" set BUILD_DIR=build
set BUILD_TYPE=%2
if "%BUILD_TYPE%"=="" set BUILD_TYPE=Release
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
rem Any other VS 2022 with the C++ tools (Enterprise, the Build Tools that INSTALL.bat installs through WinGet).
if errorlevel 1 for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cmake -S "%~dp0." -B "%~dp0%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% || exit /b 1
cmake --build "%~dp0%BUILD_DIR%" || exit /b 1
