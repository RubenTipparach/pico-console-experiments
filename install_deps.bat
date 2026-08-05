@echo off
setlocal enabledelayedexpansion

rem One time local setup: checks the toolchain the desktop builds need and
rem fetches the two sibling SDKs, pinned to the same tags CI uses
rem (.github/workflows/build.yml: BLIT_SDK_TAG, PICO_SDK_TAG). Safe to rerun.

set "ROOT=%~dp0"
set "BLIT_SDK_TAG=v0.3.3"
set "PICO_SDK_TAG=2.0.0"

echo Checking toolchain...
set "MISSING=0"

where git >nul 2>nul
if errorlevel 1 (echo   [missing] git & set "MISSING=1") else (echo   [ok] git)

where cmake >nul 2>nul
if errorlevel 1 (echo   [missing] cmake & set "MISSING=1") else (echo   [ok] cmake)

where python >nul 2>nul
if errorlevel 1 (echo   [missing] python & set "MISSING=1") else (echo   [ok] python)

where dotnet >nul 2>nul
if errorlevel 1 (echo   [missing] dotnet ^(needed for tools\flasher^) & set "MISSING=1") else (echo   [ok] dotnet)

where ninja >nul 2>nul
if errorlevel 1 (echo   [missing] ninja ^(needed for the desktop game builds^) & set "MISSING=1") else (echo   [ok] ninja)

where g++ >nul 2>nul
if errorlevel 1 (
    echo   [missing] MinGW g++ ^(needed for the desktop game builds^)
    echo              get it from https://www.mingw-w64.org or via MSYS2, and put its bin\ on PATH
    set "MISSING=1"
) else (
    echo   [ok] g++
)

if "%MISSING%"=="1" (
    echo.
    echo Install the missing tools above, then rerun this script.
    exit /b 1
)

echo.
echo Installing the 32blit python tool ^(needed by the desktop and pico builds^)...
python -m pip install --user --upgrade 32blit
if errorlevel 1 exit /b 1

echo.
echo Checking out the 32blit SDK ^(%BLIT_SDK_TAG%^)...
if exist "%ROOT%..\32blit-sdk" (
    echo   already present at %ROOT%..\32blit-sdk, leaving it alone
) else (
    git clone --depth 1 --branch "%BLIT_SDK_TAG%" https://github.com/32blit/32blit-sdk "%ROOT%..\32blit-sdk"
    if errorlevel 1 exit /b 1
)

echo.
echo Checking out the Pico SDK ^(%PICO_SDK_TAG%^)...
if exist "%ROOT%..\pico-sdk" (
    echo   already present at %ROOT%..\pico-sdk, leaving it alone
) else (
    git clone --depth 1 --branch "%PICO_SDK_TAG%" https://github.com/raspberrypi/pico-sdk "%ROOT%..\pico-sdk"
    if errorlevel 1 exit /b 1
    rem Only tinyusb is needed. A full --recursive also pulls btstack,
    rem cyw43-driver, lwip and mbedtls for no reason a desktop or pico build has.
    git -C "%ROOT%..\pico-sdk" submodule update --init lib/tinyusb
    if errorlevel 1 exit /b 1
)

echo.
echo Done. Try run_dustrider.bat, run_chicken.bat, run_kingfisher.bat, or
echo run_pico-santa.bat. build_console.bat puts every game on one cart.
