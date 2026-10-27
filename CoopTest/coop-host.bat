@echo off
rem Co-op test: start GameGuru MAX (dev build) as HOST, running a project directly.
rem Usage: coop-host.bat ["Project Name"] [port]
rem Project: first argument, else the first line of project.txt next to this file,
rem else the "Snowy Mountain Stroll" sample. Default port 27015.
rem NOTE: net flags must come before project= (the engine treats the rest of the line as the project name).
set "PROJECT=%~1"
if "%PROJECT%"=="" if exist "%~dp0project.txt" set /p PROJECT=<"%~dp0project.txt"
if "%PROJECT%"=="" set "PROJECT=Snowy Mountain Stroll"
set "NETARG=-nethost"
if not "%~2"=="" set "NETARG=-nethost=%~2"
if "%GG_MAX_BUILD_PATH%"=="" (echo GG_MAX_BUILD_PATH is not set & pause & exit /b 1)
cd /d "%GG_MAX_BUILD_PATH%\Max"
start "" GameGuruMAX.exe %NETARG% project=0%PROJECT%
