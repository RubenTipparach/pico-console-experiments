@echo off
setlocal enabledelayedexpansion

rem Shared build+run logic for the per-game launchers in the repo root
rem (run_<slug>.bat). Not meant to be called directly.
rem
rem Usage: tools\run_game.bat <slug>

set "ROOT=%~dp0.."
set "SLUG=%~1"

if "%SLUG%"=="" (
    echo Usage: tools\run_game.bat ^<slug^>
    exit /b 1
)

if not exist "%ROOT%\games\%SLUG%\game.yml" (
    echo No such game: %SLUG%
    echo Expected %ROOT%\games\%SLUG%\game.yml
    exit /b 1
)

set "SDK_DIR=%ROOT%\..\32blit-sdk"
if not exist "%SDK_DIR%" (
    echo 32blit SDK not found at %SDK_DIR%
    echo Run install_deps.bat first.
    exit /b 1
)

set "BUILD_DIR=%ROOT%\build.desktop\%SLUG%"

where ninja >nul 2>nul
if errorlevel 1 (
    echo ninja not found on PATH. Run install_deps.bat, or install it yourself.
    exit /b 1
)
where g++ >nul 2>nul
if errorlevel 1 (
    echo MinGW g++ not found on PATH. The desktop build needs it: the game
    echo metadata block and the engine's warning flags use GCC attribute
    echo syntax that MSVC's cl.exe rejects, the same reason the device and
    echo web builds already use GCC-family compilers. Install MinGW-w64 and
    echo put its bin\ on PATH, then rerun.
    exit /b 1
)

rem cmake --build only recompiles what changed, so this is a no-op past the
rem first run unless sources, models, or assets actually moved.
rem
rem Forced onto Ninja + MinGW rather than the default Visual Studio/MSVC
rem generator: MSVC cannot compile this codebase (see the g++ check above).
rem
rem CMAKE_POLICY_VERSION_MINIMUM works around 32blit-sdk's vendored SDL2
rem package: its cmake config still declares cmake_minimum_required(<3.5),
rem support for which CMake 4 removed outright.
echo Configuring %SLUG% (desktop build)...
cmake -S "%ROOT%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ ^
    -DPICO_ONLY_GAME=%SLUG% -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 exit /b 1

echo Building %SLUG% (only if out of date)...
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 exit /b 1

set "GAME_EXE="
for /f "delims=" %%F in ('dir /b /s "%BUILD_DIR%\%SLUG%.exe" 2^>nul') do set "GAME_EXE=%%F"

if "%GAME_EXE%"=="" (
    echo Built, but could not find %SLUG%.exe under %BUILD_DIR%
    exit /b 1
)

echo Running %GAME_EXE%...
"%GAME_EXE%"
