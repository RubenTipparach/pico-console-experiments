@echo off
rem Builds .uf2s for the Tufty 2350.
rem
rem    build_uf2s_tufty.bat            every game
rem    build_uf2s_tufty.bat catcoin    one game
rem
rem For the PicoSystem, see build_uf2s.bat. Both are wrappers around
rem tools\build_uf2s.bat, which is where the two boards actually differ.
call "%~dp0tools\build_uf2s.bat" tufty %*
