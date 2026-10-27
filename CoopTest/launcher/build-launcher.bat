@echo off
rem Builds the game launcher ("Play.exe") from CoopLauncher.cs with the C# compiler that ships with Windows.
rem Usage: build-launcher.bat [icon.ico] [output folder] [launcher name] [background image] [logo image]
rem   icon: e.g. your project's project256.ico (default: Windows' generic icon)
rem   output folder: default is this folder; point it at an exported game to drop it straight in.
rem   launcher name: default "Play" (gives Play.exe)
rem   background image: picture behind the buttons (png/jpg, landscape) - baked into the exe
rem   logo image: transparent png shown at the top (without one, the title is written as text)
rem Pass "" to skip an argument, e.g.  build-launcher.bat "" "" "" "art\KeyArt.png"
setlocal
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (echo .NET Framework 4 C# compiler not found. & pause & exit /b 1)
set "ICONARG="
if not "%~1"=="" set ICONARG=/win32icon:"%~1"
set "OUTDIR=%~dp0"
if not "%~2"=="" set "OUTDIR=%~2\"
set "OUTNAME=Play"
if not "%~3"=="" set "OUTNAME=%~3"
set "BGARG="
if not "%~4"=="" set BGARG=/resource:"%~4",launcher.background
set "LOGOARG="
if not "%~5"=="" set LOGOARG=/resource:"%~5",launcher.logo
"%CSC%" /nologo /target:winexe /platform:x64 /optimize+ %ICONARG% %BGARG% %LOGOARG% /out:"%OUTDIR%%OUTNAME%.exe" /r:System.Windows.Forms.dll /r:System.Drawing.dll "%~dp0CoopLauncher.cs"
if errorlevel 1 (echo Build failed. & pause & exit /b 1)
echo Built "%OUTDIR%%OUTNAME%.exe"
