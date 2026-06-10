@echo off
setlocal
:: ============================================================
:: Falcor OBJ Viewer Launcher
:: Usage: run_viewer.bat [path/to/model.obj]
:: ============================================================

set SCRIPT_DIR=%~dp0
set OUT_DIR=%SCRIPT_DIR%build\Release

cd /d "%OUT_DIR%"

:: Remove NaniteRaster if present (has missing deps)
if exist "plugins\NaniteRaster.dll" del /q "plugins\NaniteRaster.dll"

:: Regenerate plugins.json from actual DLLs
powershell -NoProfile -Command "$d=Get-ChildItem plugins\*.dll|ForEach-Object{'\"'+$_.BaseName+'\"'}; '[ '+($d -join ', ')+' ]'|Set-Content plugins\plugins.json"

:: Launch with optional OBJ file
if "%~1"=="" (
    echo Launching interactive viewer...
    start "" FalcorObjViewer.exe
) else (
    echo Loading: %~1
    start "" FalcorObjViewer.exe "%~f1"
)
