@echo off
rem Builds the console for the PicoSystem: the menu and every game
rem console.yaml lists, in one .uf2.
rem
rem For the Tufty 2350, see build_console_tufty.bat. Both are wrappers around
rem tools\build_console.bat, which is where the two boards actually differ,
rem including the SRAM ceiling the build is checked against.
call "%~dp0tools\build_console.bat" picosystem %*
