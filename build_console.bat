@echo off
setlocal enabledelayedexpansion

rem Builds the console: the menu and every game console.yaml lists, in one
rem .uf2. Same configure the `console` job in .github/workflows/build.yml
rem runs, because the workflow is the source of truth and this is convenience
rem (rule 2). Output lands at build.console\console\console.uf2, which is one
rem of the directories PicoFlasher (tools/flasher) scans on its own.

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
echo.
echo Flash it with tools\flasher, or drop it on the RPI-RP2 drive by hand.
