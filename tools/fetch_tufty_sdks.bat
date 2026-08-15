@echo off
rem Fetches the SDK pair the Tufty 2350 builds against, if it is not already
rem there. Shared by tools\build_uf2s.bat and tools\build_console.bat so the
rem pinned versions are written down once.
rem
rem Usage: tools\fetch_tufty_sdks.bat <blit-sdk-dir> <pico-sdk-dir>
rem
rem Deliberately NOT the SDKs install_deps.bat fetches. Tufty support landed in
rem 32blit master in March 2026 and the newest tag, v0.3.3, is from December
rem 2024 with no pimoroni_tufty2350 directory at all; pico-sdk 2.0.0 is too old
rem as well, since the board header wants PICO_RP2350_A2_SUPPORTED and the
rem board config pulls in hardware_powman. Rather than drag the PicoSystem
rem builds onto an SDK they have never published from, each board gets the pair
rem it was verified against. These are the same pins .github/workflows/build.yml
rem uses, under BLIT_SDK_TUFTY_SHA and PICO_SDK_TUFTY_TAG.

set "BLIT_SDK_DIR=%~1"
set "PICO_SDK_DIR=%~2"

if "%BLIT_SDK_DIR%"=="" (
    echo Usage: tools\fetch_tufty_sdks.bat ^<blit-sdk-dir^> ^<pico-sdk-dir^>
    exit /b 1
)

rem A commit rather than a branch, so this builds the same tree next month as
rem it does today.
set "BLIT_SDK_SHA=db99cb7fad2ee163bf7251193566bfcf6781da7c"
set "PICO_SDK_TAG=2.3.0"

if not exist "%BLIT_SDK_DIR%\32blit-pico\board\pimoroni_tufty2350" (
    echo Fetching the 32blit SDK at %BLIT_SDK_SHA:~0,7% ...
    rem A sha cannot be cloned with --branch, so init and fetch it directly.
    if not exist "%BLIT_SDK_DIR%" mkdir "%BLIT_SDK_DIR%"
    git -C "%BLIT_SDK_DIR%" rev-parse --git-dir >nul 2>nul || git init -q "%BLIT_SDK_DIR%"
    git -C "%BLIT_SDK_DIR%" remote get-url origin >nul 2>nul || git -C "%BLIT_SDK_DIR%" remote add origin https://github.com/32blit/32blit-sdk
    git -C "%BLIT_SDK_DIR%" fetch --depth 1 origin %BLIT_SDK_SHA%
    if errorlevel 1 exit /b 1
    git -C "%BLIT_SDK_DIR%" checkout -q FETCH_HEAD
    if errorlevel 1 exit /b 1
)

if not exist "%PICO_SDK_DIR%" (
    echo Fetching the Pico SDK ^(%PICO_SDK_TAG%^) ...
    git clone --depth 1 --branch "%PICO_SDK_TAG%" https://github.com/raspberrypi/pico-sdk "%PICO_SDK_DIR%"
    if errorlevel 1 exit /b 1
    rem Only tinyusb is needed, same as install_deps.bat.
    git -C "%PICO_SDK_DIR%" submodule update --init lib/tinyusb
    if errorlevel 1 exit /b 1
)

exit /b 0
