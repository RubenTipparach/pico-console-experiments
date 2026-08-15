@echo off
rem Builds .uf2s for the PicoSystem, which is the board this repo publishes
rem from.
rem
rem    build_uf2s.bat            every game
rem    build_uf2s.bat catcoin    one game
rem
rem For the Tufty 2350, see build_uf2s_tufty.bat. Both are wrappers around
rem tools\build_uf2s.bat, which is where the two boards actually differ.
call "%~dp0tools\build_uf2s.bat" picosystem %*
