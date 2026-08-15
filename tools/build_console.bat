@echo off
setlocal enabledelayedexpansion

rem Shared build logic for the per-board console builders in the repo root
rem (build_console.bat, build_console_tufty.bat). Not meant to be called
rem directly.
rem
rem Usage: tools\build_console.bat <picosystem|tufty>
rem
rem Builds the console: the menu and every game console.yaml lists, in one
rem .uf2. Output lands at build.console\console\console.uf2 for the PicoSystem
rem and build.console.tufty\console\console.uf2 for the Tufty, both of which
rem PicoFlasher (tools/flasher) scans on its own.
rem
rem This is the only thing that builds the console. CI used to as well, and
rem stopped: the job spent minutes of every push installing a 3 GB
rem arm-none-eabi toolchain to produce a device artifact nothing else in the
rem repo depends on. The static RAM check that job did therefore moved here, to
rem the bottom of this file, because the console is the build where RAM gets
rem tight and a number nobody prints is a number nobody checks (rule 8).

rem Resolved rather than left as tools\.., because BUILD_DIR is derived from it
rem and CMake treats a build directory spelled two ways as two directories.
for %%I in ("%~dp0..") do set "ROOT=%%~fI"

set "BOARD=%~1"
if "%BOARD%"=="" (
    echo Usage: tools\build_console.bat ^<picosystem^|tufty^>
    exit /b 1
)

rem The SRAM figures are the whole reason this is per board rather than a
rem PICO_BOARD swap. The ceiling is on the entire static footprint, not on what
rem is left over, because the SDK's framebuffer is counted in bss: 115,200
rem bytes of it on a PicoSystem, and 307,200 on a Tufty, which double buffers
rem hires by default on RP2350. Checking an RP2350 build against the RP2040's
rem 270,336 reports a console that fits comfortably as one that has overrun,
rem which is exactly what it did before this file knew about boards.
if /i "%BOARD%"=="picosystem" (
    set "PICO_BOARD=pimoroni_picosystem"
    set "BLIT_SDK_DIR=%ROOT%\..\32blit-sdk"
    set "PICO_SDK_DIR=%ROOT%\..\pico-sdk"
    set "TOOLCHAIN=pico.toolchain"
    set "BUILD_DIR=%ROOT%\build.console"
    set "FETCH=0"
    set "CHIP=RP2040"
    rem 264 KB of SRAM, as the RP2040 counts it.
    set "SRAM=270336"
    rem Leaves about 40 KB for stack and heap.
    set "SRAM_WARN=230000"
    set "DRIVE=RPI-RP2"
) else if /i "%BOARD%"=="tufty" (
    set "PICO_BOARD=pimoroni_tufty2350"
    set "BLIT_SDK_DIR=%ROOT%\..\32blit-sdk-tufty"
    set "PICO_SDK_DIR=%ROOT%\..\pico-sdk-tufty"
    set "TOOLCHAIN=pico2.toolchain"
    set "BUILD_DIR=%ROOT%\build.console.tufty"
    set "FETCH=1"
    set "CHIP=RP2350"
    rem 520 KB of SRAM.
    set "SRAM=532480"
    rem Leaves about 50 KB, the same kind of margin scaled to the bigger chip.
    set "SRAM_WARN=480000"
    set "DRIVE=RP2350"
) else (
    echo Unknown board "%BOARD%". Use "picosystem" or "tufty".
    exit /b 1
)

where git >nul 2>nul
if errorlevel 1 (echo git not found on PATH. & exit /b 1)
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
    call "%ROOT%\tools\fetch_tufty_sdks.bat" "%BLIT_SDK_DIR%" "%PICO_SDK_DIR%"
    if errorlevel 1 exit /b 1
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

rem pico2.toolchain for the Tufty, NOT pico.toolchain. The latter hardcodes
rem -mcpu=cortex-m0plus, configures without complaint, and then dies in the
rem middle of the compile: engine\src\parallel_pico.cpp pulls in
rem pico/multicore.h, RP2350 defaults PICO_USE_SW_SPIN_LOCKS on for errata E2,
rem and that implementation is gated on __ARM_ARCH_8M_MAIN__, which an ARMv6-M
rem build does not define. spin_lock.h then reaches its "#error no
rem SW_SPIN_LOCK_LOCK available ... on this platform" fallback, which reads
rem like a broken SDK and is a wrong -mcpu.
echo Configuring the console for %PICO_BOARD% ...
cmake -S "%ROOT%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE="%SDK_DIR%/%TOOLCHAIN%" ^
    -DPICO_SDK_PATH="%PICO_DIR%" ^
    -DPICO_BOARD=%PICO_BOARD% -DCMAKE_BUILD_TYPE=Release ^
    -DPSE_CONSOLE=ON
if errorlevel 1 exit /b 1

echo Building (only what is out of date)...
cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1

echo.
echo Built:
for /f "delims=" %%F in ('dir /b /s "%BUILD_DIR%\console.uf2" 2^>nul') do echo   %%F

rem What it costs. arm-none-eabi-size prints `text data bss dec hex filename`
rem with a header row above it, so skip=1 and take the first three columns.
rem Every column gets a name rather than being skipped positionally: the check
rem this replaces once read two columns over, summed text + data + 2*bss, and
rem failed itself at 595,368 bytes on a build that had linked perfectly.
set "ELF="
for /f "delims=" %%E in ('dir /b /s "%BUILD_DIR%\console.elf" 2^>nul') do set "ELF=%%E"
if not defined ELF goto :no_elf

for /f "skip=1 tokens=1,2,3" %%A in ('arm-none-eabi-size "!ELF!"') do (
    set "TEXT=%%A"
    set "DATA=%%B"
    set "BSS=%%C"
    goto :sized
)
:sized
set /a RAM=DATA+BSS
echo.
echo   board:      %PICO_BOARD% ^(%CHIP%^)
echo   flash:      !TEXT! bytes of code and const data
echo   static RAM: !RAM! bytes (!DATA! data + !BSS! bss, of %SRAM%)

rem The ceiling is on the whole static footprint, not on what is left over,
rem because the SDK's framebuffer is counted in bss. The warning is the early
rem one worth having: the linker catches a real overflow on its own, and by
rem then the message is `region RAM overflowed` with nothing in it about which
rem game grew.
if !RAM! gtr %SRAM_WARN% (
    echo.
    echo   WARNING: the console wants !RAM! bytes of static RAM of the %SRAM%
    echo   the %CHIP% has, leaving too little for the stack. Something on the
    echo   menu grew: check bss.
)
:no_elf

echo.
echo Flash it with tools\flasher, or drop it on the %DRIVE% drive by hand.
