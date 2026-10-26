@echo off
setlocal
rem Coffeeclaw - build script. Requires MSVC (cl.exe in PATH) or MinGW g++ in PATH.

set "SRC=%~dp0src\main.cpp"
set "OUT=%~dp0build"

if not exist "%OUT%" mkdir "%OUT%"

where cl >nul 2>nul
if %ERRORLEVEL%==0 goto :msvc

where g++ >nul 2>nul
if %ERRORLEVEL%==0 goto :mingw

echo No compiler found. Install Visual Studio Build Tools or MinGW-w64.
exit /b 1

:msvc
echo [msvc] compiling coffeeclaw...
cl /nologo /std:c++17 /O2 /W4 /EHsc /utf-8 /DUNICODE /D_UNICODE ^
   /Fe:"%OUT%\coffeeclaw.exe" /Fo:"%OUT%\\" "%SRC%" ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib psapi.lib
if errorlevel 1 exit /b 1
goto :copy

:mingw
echo [mingw] compiling coffeeclaw...
g++ -std=c++17 -O2 -mwindows -static -static-libgcc -static-libstdc++ -Wall -Wextra ^
    -o "%OUT%\coffeeclaw.exe" "%SRC%" -lgdi32 -luser32 -lpsapi
if errorlevel 1 exit /b 1
goto :copy

:copy
copy /y "%~dp0offsets.ini" "%OUT%\offsets.ini" >nul
echo OK -> %OUT%\coffeeclaw.exe
endlocal
