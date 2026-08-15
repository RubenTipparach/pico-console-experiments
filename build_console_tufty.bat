@echo off
rem Builds the console for the Tufty 2350: the menu and every game
rem console.yaml lists, in one .uf2.
rem
rem The Tufty has 520 KB of SRAM against the RP2040's 264 KB, so a console that
rem is tight on a PicoSystem has room here. It is checked against the right
rem ceiling either way: see tools\build_console.bat.
call "%~dp0tools\build_console.bat" tufty %*
