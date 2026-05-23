@echo off
REM PBRT Offline Renderer - Kitchen Scene
REM Launch the renderer with the kitchen PBRT scene

set EXE=%~dp0build\windows-vs2022\bin\Release\PBRTOfflineRenderer.exe
set SCENE=D:\gitProject\VLR_WF\models\kitchen\scene-v4.pbrt
set OUTPUT=D:\gitProject\VLR_WF\models\kitchen\scene-v4_render.png

if not exist "%EXE%" (
    echo [ERROR] PBRTOfflineRenderer.exe not found!
    echo Please build the project first.
    pause
    exit /b 1
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

"%EXE%" --scene "%SCENE%" --output "%OUTPUT%"

pause
