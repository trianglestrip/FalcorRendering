@echo off
setlocal
REM Interactive PBRT viewer - Kitchen scene (scene-v4.pbrt)

set "ROOT=%~dp0"
set "EXEDIR=%ROOT%build\windows-vs2022\bin\Release"
set "EXE=%EXEDIR%\pbrt_viewer.exe"
set "SCENE=D:\gitProject\VLR_WF\models\kitchen\scene-v4.pbrt"

if not exist "%EXE%" (
    echo [INFO] pbrt_viewer.exe not found. Building first...
    call "%ROOT%build_pbrt_renderer.bat"
    if errorlevel 1 (
        echo [ERROR] Build failed.
        pause
        exit /b 1
    )
)

if not exist "%EXE%" (
    echo [ERROR] pbrt_viewer.exe still missing:
    echo         %EXE%
    pause
    exit /b 1
)

if not exist "%SCENE%" (
    echo [ERROR] Scene file not found:
    echo         %SCENE%
    pause
    exit /b 1
)

echo Starting PBRT Viewer...
echo Scene: %SCENE%
echo.

start "PBRT Viewer - Kitchen" /D "%EXEDIR%" "%EXE%" --preview --scene "%SCENE%"

endlocal
exit /b 0
