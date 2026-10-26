@echo off
setlocal
cd /d "%~dp0\.."

if not exist "build\bp_count.exe" ( echo Falta build\bp_count.exe & pause & exit /b 1 )

echo Contando escrituras del POV de la camara durante 3 segundos.
echo Acepta el UAC. Juego en partida, en tercera persona, y quieto.
echo El juego se congela un instante al adjuntarse.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%CD%\build\bp_count.exe' -WorkingDirectory '%CD%' -Verb RunAs -Wait"

echo.
if exist "bp_count.txt" type "bp_count.txt"
echo.
pause
endlocal
