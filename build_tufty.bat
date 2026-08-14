@echo off
setlocal enabledelayedexpansion

rem Builds one game as a .uf2 for the Pimoroni Tufty 2350.
rem
rem    build_tufty.bat            builds catcoin
rem    build_tufty.bat kingfisher builds that game instead
rem
rem This is a probe, not a supported target. Nothing in .github/workflows
rem builds for the Tufty, no Tufty binary is published, and per rule 2 a local
rem script is not a build: if the Tufty becomes a real target, the workflow is
rem what has to learn about it. What this script is for is answering "does it
rem boot at all" without disturbing anything that already works.
rem
rem Two things are known wrong in what it produces, and neither is a bug in
rem the build:
rem
rem   - Geometry. The Tufty is 320x240, so lores is 160x120 where the
rem     PicoSystem gives 120x120. PSE_RENDER_WIDTH/HEIGHT are compile time
rem     120x120, and several games measure against a literal 120. Expect a
rem     picture in the wrong part of a wider screen.
rem   - Steering. The Tufty's five front buttons are up, down, A, B and C.
rem     There is no left and no right, and every game in this repo reads
rem     DPAD_LEFT and DPAD_RIGHT. A game that steers will not steer. The
rem     board config also enables the tca9555 driver, so a Qw/ST Pad on the
rem     I2C connector gives a real dpad with no code changes.
rem
rem There is also no audio: the Tufty board config declares no audio driver,
rem so BLIT_AUDIO_DRIVER defaults to none.

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"

set "GAME=%~1"
if "%GAME%"=="" set "GAME=catcoin"

if not exist "%ROOT%\games\%GAME%\CMakeLists.txt" (
    echo No such game: %GAME%
    echo Expected %ROOT%\games\%GAME%\CMakeLists.txt
    exit /b 1
)

rem Deliberately NOT the SDKs install_deps.bat fetches, and deliberately not
rem the versions CI pins.
rem
rem Tufty 2350 support landed in 32blit master in March 2026 (board config,
rem powman, the tca9555 input driver). The newest tagged release is v0.3.3
rem from December 2024 and has no pimoroni_tufty2350 directory at all, so the
rem tag CI pins cannot build this. pico-sdk 2.0.0 is likewise too old: the
rem board header wants PICO_RP2350_A2_SUPPORTED and hardware_powman.
rem
rem Sibling directories of their own so the PicoSystem builds keep the exact
rem SDKs they were verified against. Nothing here touches ..\32blit-sdk or
rem ..\pico-sdk.
set "BLIT_SDK_SHA=db99cb7fad2ee163bf7251193566bfcf6781da7c"
set "PICO_SDK_TAG=2.3.0"
set "BLIT_SDK_DIR=%ROOT%\..\32blit-sdk-tufty"
set "PICO_SDK_DIR=%ROOT%\..\pico-sdk-tufty"

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
where 32blit >nul 2>nul
if errorlevel 1 (
    echo The 32blit python tool is not on PATH. It writes each game's metadata
    echo block. Run install_deps.bat, or: python -m pip install --user 32blit
    exit /b 1
)

rem Same picotool DLL trap build_uf2s.bat documents: pico-sdk builds picotool
rem with the native gcc and then runs it, and an older MinGW earlier on PATH
rem (Git for Windows ships one) supplies the wrong libstdc++-6.dll, so
rem picotool.exe fails to start with no message at all.
for /f "delims=" %%G in ('where gcc') do (
    set "GCC_EXE=%%G"
    goto :gcc_found
)
:gcc_found
for %%G in ("%GCC_EXE%") do set "PATH=%%~dpG;%PATH%"

if not exist "%BLIT_SDK_DIR%\32blit-pico\board\pimoroni_tufty2350" (
    echo Fetching the 32blit SDK at %BLIT_SDK_SHA:~0,7% ...
    rem A sha cannot be cloned with --branch, so init and fetch it directly.
    rem Pinned to a commit rather than tracking master so this script builds
    rem the same tree next month as it does today.
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

rem Absolute, forward slashed: pico-sdk stores the literal
rem CMAKE_TOOLCHAIN_FILE string it was configured with and refuses to
rem reconfigure when a later run spells the same file differently.
for %%I in ("%BLIT_SDK_DIR%") do set "SDK_DIR=%%~fI"
set "SDK_DIR=%SDK_DIR:\=/%"
for %%I in ("%PICO_SDK_DIR%") do set "PICO_DIR=%%~fI"
set "PICO_DIR=%PICO_DIR:\=/%"

set "BUILD_DIR=%ROOT%\build.tufty"

rem pico2.toolchain, NOT the pico.toolchain every other build here uses.
rem pico.toolchain hardcodes -mcpu=cortex-m0plus, which configures and then
rem fails deep inside the compile: engine\src\parallel_pico.cpp pulls in
rem pico/multicore.h, and RP2350 defaults PICO_USE_SW_SPIN_LOCKS on for
rem errata E2, whose implementation is gated on __ARM_ARCH_8M_MAIN__. Built
rem as ARMv6-M that macro is absent and spin_lock.h reaches its
rem "#error no SW_SPIN_LOCK_LOCK available ... on this platform" fallback,
rem which reads like an SDK bug and is a wrong -mcpu.
echo Configuring %GAME% for the Tufty 2350 ...
cmake -S "%ROOT%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE="%SDK_DIR%/pico2.toolchain" ^
    -DPICO_SDK_PATH="%PICO_DIR%" ^
    -DPICO_BOARD=pimoroni_tufty2350 ^
    -DPICO_ONLY_GAME=%GAME% ^
    -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

echo Building ...
cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1

echo.
if exist "%BUILD_DIR%\games\%GAME%\%GAME%.uf2" (
    echo Built %BUILD_DIR%\games\%GAME%\%GAME%.uf2
) else (
    echo Built, but no .uf2 where one was expected. Look under %BUILD_DIR%.
    exit /b 1
)
echo.
echo To flash it: hold BOOT ^(far left on the back^), tap RESET, and a drive
echo named RP2350 appears. Copy the .uf2 onto it.
echo.
echo This replaces badge OS, and that is reversible: to put the badge back,
echo flash tufty-vX.X.X-micropython-with-filesystem.uf2 from
echo https://github.com/pimoroni/tufty2350/releases/latest the same way.
