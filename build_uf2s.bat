@echo off
setlocal enabledelayedexpansion

rem Builds a .uf2 for every game in one pass (device build, not desktop or
rem web), for either of the two boards this repo targets.
rem
rem    build_uf2s.bat                 every game, PicoSystem
rem    build_uf2s.bat tufty           every game, Tufty 2350
rem    build_uf2s.bat tufty kingfisher   that one game, Tufty 2350
rem    build_uf2s.bat picosystem catcoin
rem
rem With no game named, the top level CMakeLists adds every games/<slug>/
rem directory it finds, same as CI. Output lands under
rem build.<board>\games\<slug>\<slug>.uf2. The PicoSystem build keeps the
rem build.pico directory it always used, which is one of the directories
rem PicoFlasher (tools/flasher) already scans on its own, so nothing needs
rem copying for the flasher to see it. For every game on one cart instead,
rem see build_console.bat.
rem
rem The two boards are not the same shape (240x240 against 320x240) and do not
rem have the same buttons. Neither difference is handled here or in any game:
rem see engine/include/pse/board.hpp for where it actually lives, and TUFTY.md
rem for what it costs.

rem No trailing backslash: bare "%ROOT%" with one right before the closing
rem quote would escape the quote instead of closing it, on -S below.
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"

set "BOARD=%~1"
set "GAME=%~2"
if "%BOARD%"=="" set "BOARD=picosystem"

rem Each board names its own SDK pair, its own toolchain file and its own
rem build directory, and they are deliberately not shared.
rem
rem The Tufty needs SDKs newer than the ones CI pins and install_deps.bat
rem fetches: its board config landed in 32blit master in March 2026 and the
rem newest tagged release, v0.3.3, is from December 2024. Rather than drag the
rem PicoSystem builds onto an SDK they have not been published from, each
rem board gets the pair it was verified against. That is also why the
rem toolchain files differ, and that difference is the one that bites: see
rem the note above the configure step.
if /i "%BOARD%"=="picosystem" (
    set "PICO_BOARD=pimoroni_picosystem"
    set "BLIT_SDK_DIR=%ROOT%\..\32blit-sdk"
    set "PICO_SDK_DIR=%ROOT%\..\pico-sdk"
    set "TOOLCHAIN=pico.toolchain"
    set "BUILD_DIR=%ROOT%\build.pico"
    set "FETCH=0"
) else if /i "%BOARD%"=="tufty" (
    set "PICO_BOARD=pimoroni_tufty2350"
    set "BLIT_SDK_DIR=%ROOT%\..\32blit-sdk-tufty"
    set "PICO_SDK_DIR=%ROOT%\..\pico-sdk-tufty"
    set "TOOLCHAIN=pico2.toolchain"
    set "BUILD_DIR=%ROOT%\build.tufty"
    set "FETCH=1"
) else (
    echo Unknown board "%BOARD%". Use "picosystem" or "tufty".
    exit /b 1
)

rem Pinned commits for the Tufty pair, so this builds the same tree next month
rem as it does today.
set "BLIT_SDK_SHA=db99cb7fad2ee163bf7251193566bfcf6781da7c"
set "TUFTY_PICO_SDK_TAG=2.3.0"

if not "%GAME%"=="" (
    if not exist "%ROOT%\games\%GAME%\CMakeLists.txt" (
        echo No such game: %GAME%
        exit /b 1
    )
)

where git >nul 2>nul
if errorlevel 1 (echo git not found on PATH. & exit /b 1)
where cmake >nul 2>nul
if errorlevel 1 (echo cmake not found on PATH. & exit /b 1)
where ninja >nul 2>nul
if errorlevel 1 (
    echo ninja not found on PATH. Run install_deps.bat, or install it yourself.
    exit /b 1
)
where arm-none-eabi-gcc >nul 2>nul
if errorlevel 1 (
    echo arm-none-eabi-gcc not found on PATH. Install the GNU Arm Embedded
    echo toolchain and put its bin\ on PATH, then rerun.
    exit /b 1
)
where gcc >nul 2>nul
if errorlevel 1 (
    echo gcc not found on PATH. The pico-sdk build needs a native GCC to
    echo compile picotool, a host side tool it builds for itself. Install
    echo MinGW-w64 and put its bin\ on PATH, then rerun.
    exit /b 1
)

if "%FETCH%"=="1" (
    if not exist "%BLIT_SDK_DIR%\32blit-pico\board\pimoroni_tufty2350" (
        echo Fetching the 32blit SDK at %BLIT_SDK_SHA:~0,7% ...
        rem A sha cannot be cloned with --branch, so init and fetch it.
        if not exist "%BLIT_SDK_DIR%" mkdir "%BLIT_SDK_DIR%"
        git -C "%BLIT_SDK_DIR%" rev-parse --git-dir >nul 2>nul || git init -q "%BLIT_SDK_DIR%"
        git -C "%BLIT_SDK_DIR%" remote get-url origin >nul 2>nul || git -C "%BLIT_SDK_DIR%" remote add origin https://github.com/32blit/32blit-sdk
        git -C "%BLIT_SDK_DIR%" fetch --depth 1 origin %BLIT_SDK_SHA%
        if errorlevel 1 exit /b 1
        git -C "%BLIT_SDK_DIR%" checkout -q FETCH_HEAD
        if errorlevel 1 exit /b 1
    )
    if not exist "%PICO_SDK_DIR%" (
        echo Fetching the Pico SDK ^(%TUFTY_PICO_SDK_TAG%^) ...
        git clone --depth 1 --branch "%TUFTY_PICO_SDK_TAG%" https://github.com/raspberrypi/pico-sdk "%PICO_SDK_DIR%"
        if errorlevel 1 exit /b 1
        rem Only tinyusb is needed, same as install_deps.bat.
        git -C "%PICO_SDK_DIR%" submodule update --init lib/tinyusb
        if errorlevel 1 exit /b 1
    )
)

if not exist "%BLIT_SDK_DIR%" (
    echo 32blit SDK not found at %BLIT_SDK_DIR%
    echo Run install_deps.bat first.
    exit /b 1
)
if not exist "%PICO_SDK_DIR%" (
    echo Pico SDK not found at %PICO_SDK_DIR%
    echo Run install_deps.bat first.
    exit /b 1
)

rem Resolved to a clean absolute path (no ..\) with forward slashes, not just
rem for tidiness: pico-sdk remembers the literal CMAKE_TOOLCHAIN_FILE string it
rem was configured with, CMake normalized (forward slashes), and refuses to
rem reconfigure if a later run passes the same file spelled differently,
rem backslashes included.
for %%I in ("%BLIT_SDK_DIR%") do set "SDK_DIR=%%~fI"
set "SDK_DIR=%SDK_DIR:\=/%"
for %%I in ("%PICO_SDK_DIR%") do set "PICO_DIR=%%~fI"
set "PICO_DIR=%PICO_DIR:\=/%"

rem pico-sdk builds and then runs picotool as part of its own build, a host
rem tool that links against libstdc++-6.dll and libgcc_s_seh-1.dll from
rem whichever gcc built it. If an older MinGW sits earlier on PATH (Git for
rem Windows ships one at Git\mingw64\bin), Windows resolves those DLLs from
rem there instead, and picotool.exe fails to start at all: no error message,
rem just STATUS_ENTRYPOINT_NOT_FOUND. Pin PATH to the gcc actually being used
rem so its own DLLs are found first.
for /f "delims=" %%G in ('where gcc') do (
    set "GCC_EXE=%%G"
    goto :gcc_found
)
:gcc_found
for %%G in ("%GCC_EXE%") do set "PATH=%%~dpG;%PATH%"

set "ONLY_GAME="
if not "%GAME%"=="" set "ONLY_GAME=-DPICO_ONLY_GAME=%GAME%"

rem The toolchain file is per board and getting it wrong does not fail at
rem configure time. pico.toolchain hardcodes -mcpu=cortex-m0plus, so pointing
rem it at the RP2350 dies in the middle of the compile instead:
rem engine\src\parallel_pico.cpp pulls in pico/multicore.h, RP2350 defaults
rem PICO_USE_SW_SPIN_LOCKS on for errata E2, and that implementation is gated
rem on __ARM_ARCH_8M_MAIN__, which an ARMv6-M build does not define. spin_lock.h
rem then reaches its "#error no SW_SPIN_LOCK_LOCK available ... on this
rem platform" fallback, which reads like a broken SDK and is a wrong -mcpu.
echo Configuring for %PICO_BOARD% ...
cmake -S "%ROOT%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE="%SDK_DIR%/%TOOLCHAIN%" ^
    -DPICO_SDK_PATH="%PICO_DIR%" ^
    -DPICO_BOARD=%PICO_BOARD% %ONLY_GAME% ^
    -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

echo Building (only what is out of date)...
cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1

echo.
echo Built .uf2s:
for /f "delims=" %%F in ('dir /b /s "%BUILD_DIR%\*.uf2" 2^>nul') do echo   %%F

if /i "%BOARD%"=="tufty" (
    echo.
    echo To flash: hold BOOT ^(far left on the back^), tap RESET, and a drive
    echo named RP2350 appears. Copy the .uf2 onto it.
    echo.
    echo That replaces badge OS, and it is reversible: to put the badge back,
    echo flash tufty-vX.X.X-micropython-with-filesystem.uf2 from
    echo https://github.com/pimoroni/tufty2350/releases/latest the same way.
)
