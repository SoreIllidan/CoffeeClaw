@echo off
setlocal
cd /d "%~dp0"
rem Compila el extractor de offsets en build\find_roots.exe

where g++ >nul 2>nul
if errorlevel 1 (
    echo No encuentro g++ en PATH.
    pause
    exit /b 1
)

if not exist "..\build" mkdir "..\build"

g++ -std=c++17 -O2 -o "..\build\find_roots.exe" find_roots.cpp -lpsapi
if errorlevel 1 (
    echo Error de compilacion.
    pause
    exit /b 1
)

echo OK -^> ..\build\find_roots.exe
pause
endlocal
