@echo off
setlocal enabledelayedexpansion

:: ── Find VS 2022 ──────────────────────────────────────────────────────────────
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Is Visual Studio 2022 installed?
    pause & exit /b 1
)

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -version "[17,18)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)

if not defined VS_PATH (
    echo [ERROR] Visual Studio 2022 not found.
    pause & exit /b 1
)

set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [ERROR] vcvars64.bat not found at: %VCVARS%
    pause & exit /b 1
)

:: ── Menu ──────────────────────────────────────────────────────────────────────
echo.
echo  ================================================
echo   ScreamAPI Build Tool
echo  ================================================
echo.
echo   [1]  ScreamAPI  x64
echo   [2]  ScreamAPI  x86
echo   [3]  EpicGUI    x64
echo   [4]  All
echo.
set /p CHOICE= Select: 

if "%CHOICE%"=="1" goto BUILD_X64
if "%CHOICE%"=="2" goto BUILD_X86
if "%CHOICE%"=="3" goto BUILD_GUI
if "%CHOICE%"=="4" goto BUILD_ALL
echo [ERROR] Invalid choice.
pause & exit /b 1

:: ── Helpers ───────────────────────────────────────────────────────────────────
:CHECK_REBUILD_SLN
:: Sets MSBUILD_TARGET=Rebuild if output folder exists, else Build
:: %1 = platform (x64 or x86), sets TARGET_FLAG
set "OUT_DIR=ScreamAPI\build\x64\Release"
if "%~1"=="x86" set "OUT_DIR=ScreamAPI\build\Win32\Release"
set "TARGET_FLAG=/t:Build"
if exist "%OUT_DIR%\" (
    echo  [INFO] Output folder "%OUT_DIR%" exists - using Rebuild
    set "TARGET_FLAG=/t:Rebuild"
) else (
    echo  [INFO] Output folder "%OUT_DIR%" not found - using Build
)
goto :eof

:CHECK_REBUILD_GUI
set "TARGET_FLAG=/t:Build"
if exist "bin\Release\" (
    echo  [INFO] Output folder "EpicGUI\x64\Release" exists - using Rebuild
    set "TARGET_FLAG=/t:Rebuild"
) else (
    echo  [INFO] Output folder "EpicGUI\x64\Release" not found - using Build
)
goto :eof

:: ── Targets ───────────────────────────────────────────────────────────────────
:BUILD_X64
echo.
echo  Building ScreamAPI x64...
call :CHECK_REBUILD_SLN x64
call "%VCVARS%"
msbuild ScreamAPI.sln %TARGET_FLAG% /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m
goto DONE

:BUILD_X86
echo.
echo  Building ScreamAPI x86...
call :CHECK_REBUILD_SLN x86
call "%VCVARS%"
msbuild ScreamAPI.sln %TARGET_FLAG% /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v143 /m
goto DONE

:BUILD_GUI
echo.
echo  Building EpicGUI x64...
call :CHECK_REBUILD_GUI
call "%VCVARS%"
msbuild EpicGUI\EpicGUI.vcxproj %TARGET_FLAG% /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m
goto DONE

:BUILD_ALL
echo.
echo  Building all targets...
call "%VCVARS%"

call :CHECK_REBUILD_SLN x64
msbuild ScreamAPI.sln %TARGET_FLAG% /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m
if errorlevel 1 ( echo [ERROR] x64 build failed. & goto DONE )

call :CHECK_REBUILD_SLN x86
msbuild ScreamAPI.sln %TARGET_FLAG% /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v143 /m
if errorlevel 1 ( echo [ERROR] x86 build failed. & goto DONE )

call :CHECK_REBUILD_GUI
msbuild EpicGUI\EpicGUI.vcxproj %TARGET_FLAG% /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m
goto DONE

:DONE
echo.
if errorlevel 1 (
    echo  [FAILED]
) else (
    echo  [SUCCESS]
)
pause
