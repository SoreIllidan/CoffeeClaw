@echo off
setlocal
cd /d "%~dp0\.."

set "TOOL=build\find_roots.exe"

if not exist "%TOOL%" (
    echo Falta %TOOL%. Ejecuta tools\build_find_roots.cmd primero.
    pause
    exit /b 1
)

echo Lanzando %TOOL% con elevacion. Acepta el aviso de UAC.
echo Juego en partida.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%CD%\%TOOL%' -WorkingDirectory '%CD%' -Verb RunAs -Wait"

if exist "found_roots.txt" (
    echo.
    echo Reporte guardado en %CD%\found_roots.txt
) else (
    echo No se genero found_roots.txt. Lo ejecutaste como administrador?
)
pause
endlocal
