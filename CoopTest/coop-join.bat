@echo off
rem Co-op test: start GameGuru MAX (dev build) as CLIENT, joining a host.
rem Usage: coop-join.bat ["Project Name"] [host[:port]]
rem Project: first argument, else the first line of project.txt next to this file,
rem else the "Snowy Mountain Stroll" sample. Host defaults to 127.0.0.1 (same PC), port 27015.
rem NOTE: net flags must come before project= (the engine treats the rest of the line as the project name).
set "PROJECT=%~1"
if "%PROJECT%"=="" if exist "%~dp0project.txt" set /p PROJECT=<"%~dp0project.txt"
if "%PROJECT%"=="" set "PROJECT=Snowy Mountain Stroll"
set "HOSTADDR=%~2"
if "%HOSTADDR%"=="" set "HOSTADDR=127.0.0.1"
if "%GG_MAX_BUILD_PATH%"=="" (echo GG_MAX_BUILD_PATH is not set & pause & exit /b 1)
cd /d "%GG_MAX_BUILD_PATH%\Max"
start "" GameGuruMAX.exe -netjoin=%HOSTADDR% project=0%PROJECT%
rem after COOP_MOVEDELAY seconds (default 30, time to click START here), move the client
rem window to the second monitor (if there is one); set COOP_NOMOVE=1 to skip
if "%COOP_MOVEDELAY%"=="" set "COOP_MOVEDELAY=30"
if not "%COOP_NOMOVE%"=="1" start "" /min powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0move-window.ps1" -DelaySeconds %COOP_MOVEDELAY%
