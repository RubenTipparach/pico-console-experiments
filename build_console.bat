@echo off
setlocal enabledelayedexpansion

rem Builds the console: the menu and every game console.yaml lists, in one
rem .uf2. Output lands at build.console\console\console.uf2, which is one of
rem the directories PicoFlasher (tools/flasher) scans on its own.
rem
rem This is the only thing that builds the console. CI used to as well, and
rem stopped: the job spent minutes of every push installing a 3 GB
rem arm-none-eabi toolchain to produce a device artifact nothing else in the
rem repo depends on. The static RAM check that job did therefore moved here,
rem to the bottom of this file, because the console is the build where RAM
rem gets tight and a number nobody prints is a number nobody checks (rule 8).

rem No trailing backslash: bare "%ROOT%" with one right before the closing
rem quote would escape the quote instead of closing it, on -S below.
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"

if not exist "%ROOT%\..\32blit-sdk" (
    echo 32blit SDK not found at %ROOT%\..\32blit-sdk
    echo Run install_deps.bat first.
    exit /b 1
)
rem Resolved to a clean absolute path (no ..\) with forward slashes, not
rem just for tidiness: pico-sdk remembers the literal CMAKE_TOOLCHAIN_FILE
rem string it was configured with, CMake normalized (forward slashes), and
rem refuses to reconfigure if a later run passes the same file spelled
rem differently, backslashes included.
for %%I in ("%ROOT%\..\32blit-sdk") do set "SDK_DIR=%%~fI"
set "SDK_DIR=%SDK_DIR:\=/%"

if not exist "%ROOT%\..\pico-sdk" (
    echo Pico SDK not found at %ROOT%\..\pico-sdk
    echo Run install_deps.bat first.
    exit /b 1
)

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

rem pico-sdk builds and then runs picotool as part of its own build, a host
rem tool that links against libstdc++-6.dll and libgcc_s_seh-1.dll from
rem whichever gcc built it. If an older MinGW sits earlier on PATH (Git for
rem Windows ships one at Git\mingw64\bin), Windows resolves those DLLs from
rem there instead, and picotool.exe fails to start at all: no error message,
rem just STATUS_ENTRYPOINT_NOT_FOUND. Pin PATH to the gcc actually being
rem used so its own DLLs are found first.
for /f "delims=" %%G in ('where gcc') do (
    set "GCC_EXE=%%G"
    goto :gcc_found
)
:gcc_found
for %%G in ("%GCC_EXE%") do set "PATH=%%~dpG;%PATH%"

set "BUILD_DIR=%ROOT%\build.console"

echo Configuring the console (device build)...
cmake -S "%ROOT%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE="%SDK_DIR%/pico.toolchain" ^
    -DPICO_BOARD=pimoroni_picosystem -DCMAKE_BUILD_TYPE=Release ^
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
echo   flash:      !TEXT! bytes of code and const data
echo   static RAM: !RAM! bytes (!DATA! data + !BSS! bss, of 270,336)

rem The RP2040 has 270,336 bytes of SRAM. The SDK's framebuffer is 115,200 of
rem it and is counted in bss, so this ceiling is on the whole static footprint,
rem not on what is left over. 230,000 leaves about 40 KB for stack and heap,
rem which is the early warning worth having: the linker catches a real
rem overflow on its own, and by then the message is `region RAM overflowed`
rem with nothing in it about which game grew.
if !RAM! gtr 230000 (
    echo.
    echo   WARNING: the console wants !RAM! bytes of static RAM of the 270,336
    echo   the RP2040 has, leaving too little for the stack. Something on the
    echo   menu grew: check bss.
)
:no_elf

echo.
echo Flash it with tools\flasher, or drop it on the RPI-RP2 drive by hand.
