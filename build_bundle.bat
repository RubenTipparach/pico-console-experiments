@echo off
setlocal enabledelayedexpansion

rem Builds what the flasher's Library tab needs to compose a multi-game
rem bundle: the launcher on its own (-DPICO_LAUNCHER_ONLY=ON), and every game
rem a second time, each linked at its own slot (-DPICO_SLOT=n). See
rem LAUNCHER.md for why a game needs a second, differently linked build to go
rem in a bundle at all: the standalone .uf2 (what build_uf2s.bat makes) links
rem at the base of flash, the same address as the launcher, and cannot be
rem bundled.
rem
rem Every result is copied into build.launcher\library\, the flasher's one
rem canonical library folder, and registered in its manifest.json right here,
rem while this script still knows for a fact which slot each one is for.
rem That is the whole point: the flasher used to guess a file's role from its
rem own bytes (where it linked, whether it had a metadata block), which is
rem fundamentally ambiguous for anything not built by this project and was
rem also fooled by ordinary local builds never meant for a bundle. Writing
rem the answer down here, once, at the only point that actually knows it,
rem is what tools/library_manifest.py is for.
rem
rem Games are assigned slots 1, 2, 3... in the order games\ lists them.
rem That assignment only has to be consistent within one run of this script;
rem nothing on the flashing side cares what slot a game had last time.

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"

if not exist "%ROOT%\..\32blit-sdk" (
    echo 32blit SDK not found at %ROOT%\..\32blit-sdk
    echo Run install_deps.bat first.
    exit /b 1
)
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
where python >nul 2>nul
if errorlevel 1 (
    echo python not found on PATH. Needed to write the library manifest
    echo ^(tools\library_manifest.py^), and by the pico-sdk build itself.
    exit /b 1
)

rem Same PATH ordering fix as build_uf2s.bat: pico-sdk builds and runs
rem picotool for itself, and an older MinGW earlier on PATH (Git for Windows
rem ships one) makes it resolve the wrong libstdc++-6.dll / libgcc_s_seh-1.dll
rem and fail to start with no error message at all.
for /f "delims=" %%G in ('where gcc') do (
    set "GCC_EXE=%%G"
    goto :gcc_found
)
:gcc_found
for %%G in ("%GCC_EXE%") do set "PATH=%%~dpG;%PATH%"

set "TOOLCHAIN_ARGS=-DCMAKE_TOOLCHAIN_FILE=%SDK_DIR%/pico.toolchain -DPICO_BOARD=pimoroni_picosystem -DCMAKE_BUILD_TYPE=Release"
set "LIBRARY_DIR=%ROOT%\build.launcher\library"
set "MANIFEST=%LIBRARY_DIR%\manifest.json"
set "MANIFEST_TOOL=%ROOT%\tools\library_manifest.py"
if not exist "%LIBRARY_DIR%" mkdir "%LIBRARY_DIR%"

echo Building the launcher...
cmake -S "%ROOT%" -B "%ROOT%\build.launcher" -G Ninja %TOOLCHAIN_ARGS% -DPICO_LAUNCHER_ONLY=ON
if errorlevel 1 exit /b 1
cmake --build "%ROOT%\build.launcher"
if errorlevel 1 exit /b 1

copy /y "%ROOT%\build.launcher\launcher\launcher.uf2" "%LIBRARY_DIR%\launcher.uf2" >nul
if errorlevel 1 exit /b 1
python "%MANIFEST_TOOL%" add --manifest "%MANIFEST%" --file launcher.uf2 --role launcher
if errorlevel 1 exit /b 1

set /a SLOT=0
for /d %%G in ("%ROOT%\games\*") do (
    if exist "%%G\CMakeLists.txt" (
        set /a SLOT+=1
        set "SLUG=%%~nxG"
        echo.
        echo Building !SLUG! for slot !SLOT!...
        cmake -S "%ROOT%" -B "%ROOT%\build.slot\!SLUG!" -G Ninja %TOOLCHAIN_ARGS% ^
            -DPICO_ONLY_GAME=!SLUG! -DPICO_SLOT=!SLOT!
        if errorlevel 1 exit /b 1
        cmake --build "%ROOT%\build.slot\!SLUG!"
        if errorlevel 1 exit /b 1

        copy /y "%ROOT%\build.slot\!SLUG!\games\!SLUG!\!SLUG!.uf2" "%LIBRARY_DIR%\!SLUG!.uf2" >nul
        if errorlevel 1 exit /b 1
        python "%MANIFEST_TOOL%" add --manifest "%MANIFEST%" --file !SLUG!.uf2 --role slot --slot !SLOT!
        if errorlevel 1 exit /b 1
    )
)

echo.
echo Library ready at %LIBRARY_DIR%:
for /f "delims=" %%F in ('dir /b "%LIBRARY_DIR%\*.uf2" 2^>nul') do echo   %%F
echo.
echo Open run_launcher.bat, Library tab: these should now list with a real
echo slot each and be addable to a bundle.
