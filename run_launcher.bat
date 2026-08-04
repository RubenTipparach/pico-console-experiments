@echo off
rem Runs the PicoFlasher WinForms utility (tools/flasher) from source.

set "ROOT=%~dp0"

dotnet run --project "%ROOT%tools\flasher\PicoFlasher.csproj" -c Release
