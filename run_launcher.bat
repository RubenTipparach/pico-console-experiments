@echo off
rem Runs the PicoFlasher utility (tools/flasher) from source. Three tabs:
rem
rem   Flash    pick a .uf2, copy it to a board in BOOTSEL
rem   Console  pick games, build them into one console .uf2
rem   Play     build a game for this PC and run it, no device needed
rem
rem The Play tab drives tools\run_game.bat, which is what the per game
rem run_<slug>.bat wrappers already call. Same build either way.

set "ROOT=%~dp0"

dotnet run --project "%ROOT%tools\flasher\PicoFlasher.csproj" -c Release
