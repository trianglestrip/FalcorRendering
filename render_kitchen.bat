@echo off
REM PBRT Offline Renderer - Kitchen Scene
REM Launch the renderer with the kitchen PBRT scene

set EXE=%~dp0build\windows-vs2022\bin\Release\PBRTOfflineRenderer.exe
set SCENE=D:\gitProject\VLR_WF\models\kitchen\scene-v4.pbrt
set OUTPUT=D:\gitProject\VLR_WF\models\kitchen\scene-v4_render.png

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

echo Starting PBRT Offline Renderer...
echo Scene : %SCENE%
echo Output: %OUTPUT%
echo.

"%EXE%" --headless --single-frame --scene "%SCENE%" --output "%OUTPUT%"
if errorlevel 1 (
    if exist "%OUTPUT%" (
        echo.
        echo Render process returned a non-zero code, but output exists:
        echo %OUTPUT%
        exit /b 0
    )
    echo.
    echo Render failed.
    pause
    exit /b 1
)

echo.
echo Render complete: %OUTPUT%
exit /b 0
