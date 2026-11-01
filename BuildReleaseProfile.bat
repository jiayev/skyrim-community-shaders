@echo off
rem One-click release profile: LTCG, full PDB, AIO folder and package, with
rem Aftermath, DevBench, NvPerf, SPIR-V debug info and pass timestamps off, and Tracy on demand.
setlocal
call "%~dp0BuildRelease.bat" Release
set "exit_code=%ERRORLEVEL%"
endlocal & exit /b %exit_code%
