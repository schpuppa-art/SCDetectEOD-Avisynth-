@echo off
REM Build SCDetectEOD.dll for AviSynth+.
REM
REM Run from "x64 Native Tools Command Prompt for VS 2022".
REM MSVC is required — MinGW/GCC produces ABI-incompatible binaries.

if exist SCDetectEOD.dll del SCDetectEOD.dll

cl /nologo /LD /EHsc /O2 /MD /DNDEBUG SCDetectEOD.cpp /link /OUT:SCDetectEOD.dll
if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)

REM Clean up linker artifacts
if exist SCDetectEOD.exp del SCDetectEOD.exp
if exist SCDetectEOD.lib del SCDetectEOD.lib
if exist SCDetectEOD.obj del SCDetectEOD.obj

echo.
echo BUILD OK — SCDetectEOD.dll
