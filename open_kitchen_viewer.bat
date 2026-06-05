@echo off
REM Open PBRT Offline Renderer in interactive GUI mode with the kitchen scene.

set EXE=%~dp0build\windows-vs2022\bin\Release\PBRTOfflineRenderer.exe
set SCENE=D:\gitProject\VLR_WF\models\kitchen\scene-v4.pbrt

if not exist "%EXE%" (
    echo [INFO] PBRTOfflineRenderer.exe not found, building...
    call "%~dp0build_pbrt_renderer.bat"
    if errorlevel 1 (
        echo [ERROR] Build failed.
        pause
        exit /b 1
    )
)

if not exist "%SCENE%" (
    echo [ERROR] Scene file not found: %SCENE%
    pause
    exit /b 1
)

echo Opening PBRT Offline Renderer...
echo Scene: %SCENE%
echo.

start "PBRT Kitchen Viewer" "%EXE%" --scene "%SCENE%"
exit /b 0
