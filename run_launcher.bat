@echo off
rem Runs the PicoFlasher utility (tools/flasher) from source: pick a .uf2,
rem copy it to a board in BOOTSEL. Its bundle tab is gone, since the console
rem is built as one .uf2 with every game already in it (see CONSOLE.md), so
rem there is nothing left to assemble on a PC.

set "ROOT=%~dp0"

dotnet run --project "%ROOT%tools\flasher\PicoFlasher.csproj" -c Release
