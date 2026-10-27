@echo off
rem Builds the co-op mod installer ("GameGuruMAX-CoopSetup.exe") with the C# compiler that ships with Windows.
rem It embeds only the co-op files: the built GameGuruMAX.exe, a plain Play.exe launcher (built fresh here,
rem no game art) and How to play.txt.
rem Usage: build-installer.bat [Max folder] [output folder] [icon.ico]
rem   Max folder: the dev build's Max folder (default %GG_MAX_BUILD_PATH%\Max). Needs the built
rem               GameGuruMAX.exe and version.ini.
rem   output folder: default is this folder.
rem   icon: optional .ico for the installer.
setlocal
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (echo .NET Framework 4 C# compiler not found. & pause & exit /b 1)
set "MAXDIR=%~1"
if "%MAXDIR%"=="" if not "%GG_MAX_BUILD_PATH%"=="" set "MAXDIR=%GG_MAX_BUILD_PATH%\Max"
if "%MAXDIR%"=="" (echo Pass the Max folder or set GG_MAX_BUILD_PATH. & pause & exit /b 1)
if not exist "%MAXDIR%\GameGuruMAX.exe" (echo No GameGuruMAX.exe in "%MAXDIR%". & pause & exit /b 1)
if not exist "%MAXDIR%\version.ini" (echo No version.ini in "%MAXDIR%". & pause & exit /b 1)
set "OUTDIR=%~dp0"
if not "%~2"=="" set "OUTDIR=%~2\"
set "ICONARG="
if not "%~3"=="" set ICONARG=/win32icon:"%~3"
rem a plain launcher (no icon or art - each game builds its own with build-launcher.bat)
set "KITDIR=%TEMP%\coop-installer-kit"
if not exist "%KITDIR%" mkdir "%KITDIR%"
call "%~dp0..\launcher\build-launcher.bat" "" "%KITDIR%" < nul
if not exist "%KITDIR%\Play.exe" (echo Launcher build failed. & pause & exit /b 1)
rem the GameGuru MAX version the exe was built for: first line of version.ini
set "VERFILE=%KITDIR%\version.txt"
set /p VERLINE=<"%MAXDIR%\version.ini"
> "%VERFILE%" echo %VERLINE%
"%CSC%" /nologo /target:winexe /platform:anycpu /optimize+ %ICONARG% /out:"%OUTDIR%GameGuruMAX-CoopSetup.exe" ^
 /resource:"%MAXDIR%\GameGuruMAX.exe",coop.exe ^
 /resource:"%KITDIR%\Play.exe",coop.play ^
 /resource:"%~dp0..\launcher\How to play.txt",coop.howto ^
 /resource:"%VERFILE%",coop.version ^
 /r:System.Windows.Forms.dll /r:System.Drawing.dll "%~dp0CoopInstaller.cs"
set "ERR=%errorlevel%"
rmdir /s /q "%KITDIR%"
if not "%ERR%"=="0" (echo Build failed. & pause & exit /b 1)
echo Built "%OUTDIR%GameGuruMAX-CoopSetup.exe" for %VERLINE%
